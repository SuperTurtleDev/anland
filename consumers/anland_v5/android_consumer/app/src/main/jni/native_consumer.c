#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "anw_hidden.h"
#include "camera_service.h"
#include "display_consumer.h"
#include "native_audio.h"
#include "protocol.h"
#include "socket_utils.h"
#include "tracy_zones.h"

#define TAG "Anland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define PIXEL_FORMAT_RGBA_8888 1
#define MAX_COLLECT_BUFS 8
#define BYTES_PER_RGBA_PIXEL 4U
#define ACQUIRE_FENCE_TIMEOUT_MS 1000

/* Saved JVM reference for event-thread JNI callbacks. Process-global (the JVM is);
 * the per-thread env is attached as needed. The activity callback target is
 * per-instance -> consumer_state.clipboard_obj. */
static JavaVM *g_jvm = NULL;

/* ANativeWindow hidden-API function pointers: loaded once, read-only afterwards, so
 * safe to share across instances. */
static struct anw_api api;
static bool api_loaded = false;
static void on_fallback(void *userdata);

static void on_exit_fallback(void *userdata);
struct consumer_state {
    pthread_mutex_t lock;
    ANativeWindow *window;
    display_ctx *ctx;
    pthread_t render_thread;
    volatile bool running;

    //Note: it is Deamon's Reconnect, not Fallback Flag
    //Fallback is maintained by display lib, and the consumer should not care about it.
    volatile bool need_reconnect;

    int buf_count;
    int dmabuf_fds[MAX_COLLECT_BUFS];
    struct buf_info dmabuf_infos[MAX_COLLECT_BUFS];
    ANativeWindowBuffer *buf_anb[MAX_COLLECT_BUFS];

    int screen_w;
    int screen_h;

    // Latest display refresh rate (milli-Hz) reported from Java. Read on
    // (re)connect to seed the producer; updated live by nativeSetRefreshRate.
    volatile uint32_t refresh_mhz;

    // Event (output) thread
    pthread_t event_thread;
    volatile bool event_running;
    /* True while event_thread holds a started-but-not-yet-joined thread. stop only
     * signals (never joins -- on_fallback can run ON the event thread); the join is
     * deferred to the next start_event_thread() (create time), which runs on the
     * render thread and so cannot self-join. */
    bool event_thread_joinable;

    /* Connection config, set from Java via nativeConfigure() and read on each
     * (re)connect in do_connect(). Guarded by cfg_lock. Per-instance. */
    pthread_mutex_t cfg_lock;
    char cfg_socket_path[256];
    bool cfg_use_root;
    char cfg_helper_path[512];
    char cfg_bridge_path[512];
    int  cfg_custom_width;
    int  cfg_custom_height;
    bool cfg_topapp_enable;
    char cfg_topapp_path[512];
    int cfg_topapp_mode;
    char cfg_topapp_stops[192];
    /* Snapshot used by the helper lifecycle after do_connect() drops cfg_lock. */
    bool topapp_run_enable;
    char topapp_run_path[512];
    int topapp_run_mode;
    char topapp_run_stops[192];

    /* Foreground-scheduling state, serialized by topapp_lock. topapp_root_pid
     * is the HOST pid of the producer's session tree root (find_tree_root via
     * UNIX_DIAG): mode 2 promotes its whole tree, mode 1 uses it both as the
     * namespace anchor for the O(1) "set" switches and as the restore root --
     * on disconnect/fallback the whole tree goes back to the root cgroups in
     * one sweep, covering every pid mode 1 boosted. Written by the topapp_*
     * helpers / the event thread's scheduling handler from arbitrary
     * threads. */
    pthread_mutex_t topapp_lock;
    pid_t topapp_root_pid;

    /* Pointer-motion delta tracking (per-instance). */
    bool  motion_has_last;
    float motion_last_x, motion_last_y;

    /* Clipboard callback target: the Java object whose nativeSetClipboardText /
     * nativeClipListening / nativeClipboardSync the event thread calls (per-instance). */
    jobject clipboard_obj;

    /* Owning MainActivity: on_fallback() calls its onFallback() when the display lib
     * drops the connection, so Java can probe the daemon socket and close the window
     * if the daemon is gone (per-instance global ref). */
    jobject activity_obj;

    /* Per-instance audio bridge (own AAudio streams, own producer). */
    audio_bridge *audio;

    /* Per-instance camera service registration; userdata points back at this state
     * so the camera layer can tell instances apart (see camera_service.c). */
    struct service_info camera_svc;
};

static void topapp_handle_scheduling_event(struct consumer_state *s,
                                           const struct OutputEvent *event);

struct dmabuf_fdinfo {
    unsigned long long size;
    char exporter[64];
};

static bool query_dmabuf_fdinfo(int fd, struct dmabuf_fdinfo *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    int info_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (info_fd < 0)
        return false;

    char data[2048];
    ssize_t count = read(info_fd, data, sizeof(data) - 1);
    close(info_fd);
    if (count <= 0)
        return false;
    data[count] = '\0';

    memset(info, 0, sizeof(*info));
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        unsigned long long size = 0;
        char exporter[sizeof(info->exporter)];
        if (sscanf(line, "size:%llu", &size) == 1) {
            info->size = size;
        } else if (sscanf(line, "exp_name:%63s", exporter) == 1) {
            strncpy(info->exporter, exporter, sizeof(info->exporter) - 1);
        }
    }
    return info->size > 0 && info->exporter[0] != '\0';
}

static bool wait_for_acquire_fence(int fence_fd)
{
    struct pollfd pfd = {
        .fd = fence_fd,
        .events = POLLIN,
    };
    int result;
    do {
        pfd.revents = 0;
        result = poll(&pfd, 1, ACQUIRE_FENCE_TIMEOUT_MS);
    } while (result < 0 && errno == EINTR);

    if (result > 0 && (pfd.revents & POLLIN))
        return true;

    if (result == 0) {
        LOGE("acquire fence %d timed out after %d ms",
             fence_fd, ACQUIRE_FENCE_TIMEOUT_MS);
    } else if (result < 0) {
        LOGE("acquire fence %d poll failed: %s", fence_fd, strerror(errno));
    } else {
        LOGE("acquire fence %d signaled unexpected poll events 0x%x",
             fence_fd, pfd.revents);
    }
    return false;
}

/* queueBuffer/cancelBuffer take ownership of fence_fd, including on paths where
 * the caller did not wait on the acquire fence. Never close fence_fd after this. */
static int finish_dequeued_buffer(ANativeWindow *window,
                                  ANativeWindowBuffer *buffer,
                                  int fence_fd,
                                  bool queue)
{
    int result = queue
        ? api.queueBuffer(window, buffer, fence_fd)
        : api.cancelBuffer(window, buffer, fence_fd);
    if (result != 0) {
        LOGE("%s failed: %d", queue ? "queueBuffer" : "cancelBuffer", result);
    }
    return result;
}

/* This transport currently carries one packed RGBA plane. ANativeWindow does
 * not expose vendor-private handle plane metadata, so never infer an offset or
 * stride from handle->data[]. The metadata-free path is valid only when the
 * allocation exactly matches the public stride-by-height geometry: that proves
 * this supported plane starts at byte zero. */
static bool describe_packed_rgba_buffer(const ANativeWindowBuffer *anb,
                                        int expected_width,
                                        int expected_height,
                                        struct buf_info *info,
                                        unsigned long long *required_size)
{
    if (anb->format != PIXEL_FORMAT_RGBA_8888 ||
        anb->width != expected_width || anb->height != expected_height ||
        anb->stride < anb->width || anb->layerCount != 1) {
        LOGE("unsupported Surface layout: format=%d size=%dx%d expected=%dx%d "
             "stride=%d layers=%llu",
             anb->format, anb->width, anb->height, expected_width,
             expected_height, anb->stride,
             (unsigned long long)anb->layerCount);
        return false;
    }

    const unsigned long long stride_bytes =
        (unsigned long long)(unsigned int)anb->stride * BYTES_PER_RGBA_PIXEL;
    if (stride_bytes > UINT32_MAX) {
        LOGE("Surface stride exceeds protocol range: %llu", stride_bytes);
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->stride = (uint32_t)stride_bytes;
    info->width = (uint32_t)anb->width;
    info->height = (uint32_t)anb->height;
    info->format = PIXEL_FORMAT_RGBA_8888;
    info->modifier = 0;
    info->offset = 0;
    *required_size = (unsigned long long)info->offset
                   + stride_bytes * (unsigned long long)info->height;
    return true;
}

static int find_surface_dmabuf(const ANativeWindowBuffer *anb,
                               unsigned long long required_size,
                               struct dmabuf_fdinfo *selected_info)
{
    int best_fd = -1;
    int best_index = -1;
    int best_rank = -1;
    struct stat best_stat = {0};
    bool ambiguous = false;

    for (int i = 0; i < anb->handle->numFds; i++) {
        const int candidate = anb->handle->data[i];
        const int flags = fcntl(candidate, F_GETFL);
        const int access_mode = flags >= 0 ? flags & O_ACCMODE : O_RDONLY;
        struct stat candidate_stat = {0};
        const bool stat_ok = fstat(candidate, &candidate_stat) == 0;
        struct dmabuf_fdinfo candidate_info = {0};
        const bool is_dmabuf = query_dmabuf_fdinfo(candidate, &candidate_info);
        const bool writable = flags >= 0 &&
            (access_mode == O_WRONLY || access_mode == O_RDWR);
        const bool exact_size = is_dmabuf &&
            candidate_info.size == required_size;
        const bool valid = stat_ok && writable && exact_size;
        const int rank = access_mode == O_RDWR ? 1 : 0;

        LOGI("    handle_fd[%d/%d]=%d flags=0x%x writable=%d dmabuf=%d "
             "exporter=%s size=%llu required=%llu inode=%llu exact=%d",
             i, anb->handle->numFds, candidate, flags, writable, is_dmabuf,
             is_dmabuf ? candidate_info.exporter : "-",
             is_dmabuf ? candidate_info.size : 0ULL, required_size,
             stat_ok ? (unsigned long long)candidate_stat.st_ino : 0ULL,
             exact_size);

        if (!valid)
            continue;

        if (rank > best_rank) {
            best_fd = candidate;
            best_index = i;
            best_rank = rank;
            best_stat = candidate_stat;
            *selected_info = candidate_info;
            ambiguous = false;
        } else if (rank == best_rank &&
                   (candidate_stat.st_dev != best_stat.st_dev ||
                    candidate_stat.st_ino != best_stat.st_ino)) {
            LOGE("equally ranked dma-buf candidates at handle indices %d and %d",
                 best_index, i);
            ambiguous = true;
        }
    }

    if (best_fd < 0) {
        LOGE("no writable exact-size dma-buf matches the %llu-byte Surface plane",
             required_size);
        return -1;
    }
    if (ambiguous) {
        LOGE("ambiguous Surface dma-buf handle; refusing a vendor-specific guess");
        return -1;
    }

    LOGI("selected handle_fd[%d]=%d exporter=%s size=%llu rank=%d",
         best_index, best_fd, selected_info->exporter, selected_info->size,
         best_rank);
    return best_fd;
}

static int collect_dmabufs(struct consumer_state *s)
{
    ANativeWindow *win = s->window;
    const int target = s->buf_count;
    int found = 0;

    LOGI("collecting %d direct Surface dma-bufs via dequeue/queue", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int acquire_fence = -1;
        const int dequeue_result =
            api.dequeueBuffer(win, &anb, &acquire_fence);
        if (dequeue_result != 0 || !anb) {
            LOGE("dequeueBuffer failed on attempt %d: %d",
                 attempt, dequeue_result);
            if (acquire_fence >= 0)
                close(acquire_fence);
            break;
        }

        if (acquire_fence >= 0) {
            if (!wait_for_acquire_fence(acquire_fence)) {
                if (finish_dequeued_buffer(win, anb, acquire_fence, false) != 0)
                    break;
                continue;
            }
            close(acquire_fence);
            acquire_fence = -1;
        }

        if (!anb->handle || anb->handle->numFds < 1) {
            LOGE("dequeued buffer has no fd-bearing handle on attempt %d",
                 attempt);
            if (finish_dequeued_buffer(win, anb, -1, false) != 0)
                break;
            continue;
        }

        bool duplicate = false;
        for (int i = 0; i < found; i++) {
            if (s->buf_anb[i] == anb) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            if (finish_dequeued_buffer(win, anb, -1, true) != 0)
                break;
            continue;
        }

        struct buf_info buffer_info;
        unsigned long long required_size = 0;
        if (!describe_packed_rgba_buffer(anb, s->screen_w, s->screen_h,
                                         &buffer_info, &required_size)) {
            if (finish_dequeued_buffer(win, anb, -1, false) != 0)
                break;
            continue;
        }

        struct dmabuf_fdinfo selected_info = {0};
        const int fd =
            find_surface_dmabuf(anb, required_size, &selected_info);
        if (fd < 0) {
            if (finish_dequeued_buffer(win, anb, -1, false) != 0)
                break;
            continue;
        }

        const int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (dup_fd < 0) {
            LOGE("failed to duplicate Surface dma-buf %d: %s",
                 found, strerror(errno));
            if (finish_dequeued_buffer(win, anb, -1, false) != 0)
                break;
            continue;
        }
        if (finish_dequeued_buffer(win, anb, -1, true) != 0) {
            close(dup_fd);
            break;
        }

        s->buf_anb[found] = anb;
        s->dmabuf_fds[found] = dup_fd;
        s->dmabuf_infos[found] = buffer_info;
        LOGI("  direct buf[%d]: anb=%p fd=%d dup=%d exporter=%s size=%llu "
             "%ux%u stride_bytes=%u offset=%u",
             found, (void *)anb, fd, dup_fd, selected_info.exporter,
             selected_info.size, buffer_info.width, buffer_info.height,
             buffer_info.stride, buffer_info.offset);
        found++;
    }

    if (found < target) {
        LOGE("only collected %d/%d direct Surface dma-bufs", found, target);
        for (int i = 0; i < found; i++) {
            close(s->dmabuf_fds[i]);
            s->dmabuf_fds[i] = -1;
            s->buf_anb[i] = NULL;
            memset(&s->dmabuf_infos[i], 0, sizeof(s->dmabuf_infos[i]));
        }
        s->buf_count = 0;
        return -1;
    }

    s->buf_count = found;
    LOGI("collected %d direct Surface dma-bufs", found);
    return 0;
}

static void cleanup_dmabufs(struct consumer_state *s)
{
    for (int i = 0; i < s->buf_count; i++) {
        if (s->dmabuf_fds[i] >= 0) {
            close(s->dmabuf_fds[i]);
            s->dmabuf_fds[i] = -1;
        }
        s->buf_anb[i] = NULL;
        memset(&s->dmabuf_infos[i], 0, sizeof(s->dmabuf_infos[i]));
    }
    s->buf_count = 0;
}

/* Report the current display refresh rate to the producer over the data
 * channel, reusing the InputEvent framing (see INPUT_TYPE_DISPLAY_REFRESH).
 * No-op when disconnected or rate unknown. */
static void send_refresh_rate(struct consumer_state *s)
{
    if (!s->ctx || s->refresh_mhz == 0)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_DISPLAY_REFRESH,
        .display = { .refresh_mhz = s->refresh_mhz },
    };
    push_input_event(s->ctx, &ev);
}

/*
 * Event thread: listens for output events (clipboard, etc.) from the producer
 * on the data_fd. Runs while s->event_running is true.
 */
static void *event_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("event thread started");

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("event thread: AttachCurrentThread failed");
        return NULL;
    }

    /* Find classes/methods once */
    jclass ctxClass = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID setClipMethod = (*env)->GetMethodID(env, ctxClass, "nativeSetClipboardText", "(Ljava/lang/String;)V");
    if (!setClipMethod) {
        LOGE("event thread: nativeSetClipboardText not found");
        (*g_jvm)->DetachCurrentThread(g_jvm);
        return NULL;
    }

    /* CONSUMER_VAR_* callbacks land on the owning MainActivity (var, value). */
    jmethodID setVarMethod = NULL;
    if (s->activity_obj) {
        jclass actClass = (*env)->GetObjectClass(env, s->activity_obj);
        setVarMethod = (*env)->GetMethodID(env, actClass, "nativeSetConsumerVar", "(II)V");
        (*env)->DeleteLocalRef(env, actClass);
        if (!setVarMethod)
            LOGE("event thread: nativeSetConsumerVar not found");
    }

    while (s->event_running) {
        if (!s->ctx) {
            usleep(50000);
            continue;
        }

        struct OutputEvent ev;
        TracyCZoneN(zPoll, "poll_output_event", 1);
        int ret = poll_output_event(s->ctx, &ev, 500);
        TracyCZoneEnd(zPoll);
        if (ret <= 0)
            continue;

        if (ev.type == OUTPUT_TYPE_RESOURCES_REQUEST) {
            /* Producer is asking for a service's fds (e.g. camera). The display lib
             * matches the type against the registered services and sends the
             * pre-created fds back over SCM_RIGHTS. */
            handle_resource_request(s->ctx, &ev);
        } else if (ev.type == OUTPUT_TYPE_CLIPBOARD && ev.clipboard.size > 0) {
            char *buf = malloc(ev.clipboard.size + 1);
            if (!buf)
                continue;

            if (poll_output_event_extend_data(s->ctx, buf, ev.clipboard.size, 5000) == 1) {
                buf[ev.clipboard.size] = '\0';
                jstring jstr = (*env)->NewStringUTF(env, buf);
                if (jstr) {
                    (*env)->CallVoidMethod(env, s->clipboard_obj, setClipMethod, jstr);
                    (*env)->DeleteLocalRef(env, jstr);
                }
            }
            free(buf);
        } else if (ev.type == OUTPUT_TYPE_SET_CONSUMER_VAR) {
            /* Producer asserts a transient runtime override. CONSUMER_VAR_CAPTURE_MOUSE
             * forces pointer capture on for Wayland pointer lock (games); 0 releases.
             * Forwarded to MainActivity, which marshals to the UI thread. */
            if (setVarMethod && s->activity_obj)
                (*env)->CallVoidMethod(env, s->activity_obj, setVarMethod,
                                       (jint)ev.set_consumer_var.var, (jint)ev.set_consumer_var.value);
        } else if (ev.type == OUTPUT_TYPE_SCHEDULING) {
            topapp_handle_scheduling_event(s, &ev);
        } else {
            /* Unknown or zero-length event: drain any trailing data if size > 0 */
            LOGI("event thread: unknown output event type=%u size=%u", ev.type, ev.clipboard.size);
        }
    }

    (*g_jvm)->DetachCurrentThread(g_jvm);
    LOGI("event thread stopped");
    return NULL;
}

static void join_event_thread(struct consumer_state *s)
{
    /* Idempotent. MUST be called only from a non-event thread (render / JNI teardown);
     * never from on_fallback (which may run on the event thread). */
    if (s->event_thread_joinable) {
        pthread_join(s->event_thread, NULL);
        s->event_thread_joinable = false;
    }
}

static void start_event_thread(struct consumer_state *s)
{
    if (s->event_running)
        return;
    /* Reap the previous stopped-but-unjoined thread before spawning a new one. Runs on
     * the render thread (on_exit_fallback), so this join can't self-deadlock. */
    join_event_thread(s);
    s->event_running = true;
    if (pthread_create(&s->event_thread, NULL, event_thread_func, s) == 0)
        s->event_thread_joinable = true;
    else
        s->event_running = false;
}

static void stop_event_thread(struct consumer_state *s)
{
    /* Signal only -- do NOT join here. enter_fallback()->on_fallback() can execute on
     * the event thread itself, so joining would self-deadlock. The handle stays in
     * event_thread (event_thread_joinable) and is reaped at create time by the next
     * start_event_thread() (or do_connect's reconnect path, both on the render
     * thread). */
    s->event_running = false;
}

/*
 * "Connect with root" handshake. The app cannot connect() to a root-owned
 * daemon socket directly, so it listens on a bridge socket, launches the bundled
 * helper through `su -c`, and the helper (as root) connects to the daemon and
 * passes the connected fd back over the bridge. Returns the received fd (caller
 * owns it) or -1 on failure.
 */
static int recv_fd_via_root_helper(const char *daemon_sock,
                                   const char *helper_path,
                                   const char *bridge_path)
{
    if (helper_path[0] == '\0' || bridge_path[0] == '\0') {
        LOGE("root helper: helper/bridge path not configured");
        return -1;
    }

    unlink(bridge_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        LOGE("root helper: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, bridge_path, sizeof(addr.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("root helper: bind(%s) failed: %s", bridge_path, strerror(errno));
        close(lfd);
        return -1;
    }
    /* Root helper runs in a different SELinux/uid context; make the socket file
     * reachable. (Root bypasses DAC, but be permissive anyway.) */
    chmod(bridge_path, 0777);

    if (listen(lfd, 1) < 0) {
        LOGE("root helper: listen() failed: %s", strerror(errno));
        close(lfd);
        unlink(bridge_path);
        return -1;
    }

    /* Build the command su runs: "<helper> <daemon_sock> <bridge_path>". */
    char inner[1100];
    snprintf(inner, sizeof(inner), "%s %s %s",
             helper_path, daemon_sock, bridge_path);

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("root helper: fork() failed: %s", strerror(errno));
        close(lfd);
        unlink(bridge_path);
        return -1;
    }
    if (pid == 0) {
        execlp("su", "su", "-c", inner, (char *)NULL);
        _exit(127);   /* su not found / exec failed */
    }

    /* Wait for the helper to connect (root prompt may take a while). */
    int fd = -1;
    struct pollfd pfd = { .fd = lfd, .events = POLLIN };
    if (poll(&pfd, 1, 30000) > 0 && (pfd.revents & POLLIN)) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd >= 0) {
            char b;
            int got = 0;
            if (recv_fds(cfd, &b, 1, &fd, 1, &got) < 0 || got < 1)
                fd = -1;
            close(cfd);
        }
    } else {
        LOGE("root helper: timed out waiting for helper connection");
    }

    int status = 0;
    waitpid(pid, &status, 0);
    close(lfd);
    unlink(bridge_path);

    if (fd < 0)
        LOGE("root helper: did not receive daemon fd (su status=%d)", status);
    return fd;
}

/*
 * Foreground scheduling ("启用前台调度 (root)"). No resident helper: each
 * transition is one `su -c libsettopapp.so ...` invocation.
 *
 * Mode 1 (focused app): at exit-fallback the consumer asks the helper for the
 * HOST pid of the session anchor (UNIX_DIAG lookup, "noset"); that anchor
 * both pins the producer's PID namespace and its /proc/<pid>/root/proc.
 * Every SCHEDULING report from the producer (producer subtree at init, then
 * the focused client subtree on each activation change) becomes an O(1) "set"
 * call that setns()es into the anchor's PID namespace and writes the
 * container pid straight into the cgroup files. The producer orders the
 * reports self-contained (off before on), so no consumer-side tracking is
 * needed.
 *
 * Mode 2 (whole session): unchanged legacy behaviour -- promote the whole
 * producer tree at exit-fallback.
 *
 * Both modes restore identically on fallback/stop: one "restore" sweep over
 * the whole tree rooted at the scanned session root, which covers every pid
 * either mode boosted.
 */

static bool safe_helper_path(const char *path)
{
    if (!path || path[0] != '/')
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (!(isalnum(*p) || *p == '/' || *p == '.' || *p == '_'
              || *p == '-' || *p == '+' || *p == '=' || *p == ':'
              || *p == '~')) {
            return false;
        }
    }
    return true;
}

/* Run `su -c "<cmd>"`, capture its first stdout line. Returns the parsed pid
 * (>0), or -1 on any failure (no su, helper missing, no output). Blocks for
 * the duration of the helper; called from the render/event threads where a
 * short stall at a session boundary is acceptable. */
static pid_t run_su_capture_pid(const char *cmd)
{
    int link[2];
    if (pipe(link) < 0) {
        LOGE("settopapp: pipe failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("settopapp: fork failed: %s", strerror(errno));
        close(link[0]);
        close(link[1]);
        return -1;
    }
    if (pid == 0) {
        /* su prompt/progress output must not pollute the pid line. */
        close(link[0]);
        dup2(link[1], STDOUT_FILENO);
        close(link[1]);
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(link[1]);

    char line[32];
    ssize_t n = read(link[0], line, sizeof(line) - 1);
    close(link[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOGE("settopapp: helper failed (status=%d, read=%zd)", status, n);
        return -1;
    }
    line[n] = '\0';
    int got = atoi(line);
    return got > 0 ? (pid_t)got : -1;
}

/* Run `su -c "<cmd>"` ignoring its output; returns the su exit status, or -1
 * on fork/exec failure. Used for the mode-1 "set" switches, which report
 * success through logcat rather than stdout. */
static int run_su_status(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("settopapp: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Resolve the producer (holder of the peer of data socket `data_fd`) to its
 * session-tree-root HOST pid via the helper. promote=false only discovers
 * ("noset"); promote=true additionally moves the whole tree into top-app
 * (mode 2), honouring the optional custom stop-name list. Returns the root
 * pid, or -1. Called at exit-fallback, i.e. the producer is connected right
 * now, so the diag lookup has a live peer; data_fd is live for this whole
 * call (enter_fallback closes it only after this returns). In mode 1 the
 * returned anchor pins the producer's PID namespace (/proc/<pid>/ns/pid) and
 * its /proc/<pid>/root/proc for the later "set" calls. */
static pid_t topapp_discover_root(struct consumer_state *s, int data_fd,
                                  bool promote)
{
    struct stat st;
    if (data_fd < 0 || fstat(data_fd, &st) < 0 || st.st_ino == 0
        || !safe_helper_path(s->topapp_run_path)) {
        LOGE("settopapp: data fd not resolvable");
        return -1;
    }

    /* The inode identifies our end of the data socketpair; the helper resolves
     * it to the peer the producer holds. The optional stop-name list bounds
     * the upward PPID scan in both modes: mode 2 uses the scanned root as the
     * promote tree root, mode 1 as the namespace anchor (and restore root). */
    char cmd[sizeof(s->topapp_run_path) + sizeof(s->topapp_run_stops) + 32];
    if (s->topapp_run_stops[0] != '\0' && promote)
        snprintf(cmd, sizeof(cmd), "%s %llu stops=%s", s->topapp_run_path,
                 (unsigned long long)st.st_ino, s->topapp_run_stops);
    else if (s->topapp_run_stops[0] != '\0')
        snprintf(cmd, sizeof(cmd), "%s %llu stops=%s noset", s->topapp_run_path,
                 (unsigned long long)st.st_ino, s->topapp_run_stops);
    else if (promote)
        snprintf(cmd, sizeof(cmd), "%s %llu", s->topapp_run_path,
                 (unsigned long long)st.st_ino);
    else
        snprintf(cmd, sizeof(cmd), "%s %llu noset", s->topapp_run_path,
                 (unsigned long long)st.st_ino);

    pid_t root = run_su_capture_pid(cmd);
    if (root > 0)
        LOGI("settopapp: tree root %d %s", (int)root,
             promote ? "promoted to top-app" : "resolved (anchor)");
    return root;
}

/* Mode 1: O(1) switch of one container pid (or its subtree) between top-app
 * and the root groups, via the anchor's PID namespace. */
static int topapp_set(struct consumer_state *s, pid_t container_pid,
                      bool on, bool settree)
{
    pthread_mutex_lock(&s->topapp_lock);
    const pid_t anchor = s->topapp_root_pid;
    pthread_mutex_unlock(&s->topapp_lock);
    if (anchor <= 0)
        return -1;

    char cmd[sizeof(s->topapp_run_path) + 96];
    const int length = snprintf(cmd, sizeof(cmd), "%s set %d %d opt=%s settree=%s",
                                s->topapp_run_path, (int)anchor, (int)container_pid,
                                on ? "on" : "off", settree ? "true" : "false");
    if (length <= 0 || (size_t)length >= sizeof(cmd))
        return -1;
    return run_su_status(cmd);
}

/* Undo every mode-1 boost of this connection: the focused pid (if any) and
 * KWin's own subtree back to the root groups. */
/* Undo every boost of this connection: restore the whole scanned tree rooted
 * at topapp_root_pid back to the root cgroups. Shared by both modes -- the
 * anchor tree covers KWin's subtree and every focused client subtree mode 1
 * boosted, and mode 2 promoted exactly that tree in the first place. Safe to
 * call with the producer already gone: restore walks the root's tree as it
 * exists NOW (leftover processes still move back; dead ones skip). */
static void topapp_restore_tree(struct consumer_state *s)
{
    pthread_mutex_lock(&s->topapp_lock);
    pid_t root = s->topapp_root_pid;
    s->topapp_root_pid = 0;
    pthread_mutex_unlock(&s->topapp_lock);

    if (root <= 0)
        return;

    char cmd[sizeof(s->topapp_run_path) + 32];
    snprintf(cmd, sizeof(cmd), "%s %d restore", s->topapp_run_path, (int)root);
    if (run_su_capture_pid(cmd) > 0)
        LOGI("settopapp: tree root %d restored", (int)root);
}

static void topapp_handle_scheduling_event(struct consumer_state *s,
                                           const struct OutputEvent *event)
{
    /* Only mode 1 consumes per-pid reports (mode 2 promoted the whole tree
     * once at exit-fallback), and only when the feature is enabled for this
     * connection -- with foreground scheduling off the producer may still
     * send events (it cannot know the Android-side setting), and they must
     * not spawn su invocations. */
    if (s->topapp_run_mode != 1 || !s->topapp_run_enable)
        return;

    pthread_mutex_lock(&s->topapp_lock);
    const bool have_anchor = s->topapp_root_pid > 0;
    pthread_mutex_unlock(&s->topapp_lock);
    if (!have_anchor)
        return;

    /* Self-contained switch: the producer already ordered the events so an
     * "off" for the previous client arrives before the "on" for the next.
     * Every event maps 1:1 onto one O(1) helper "set" invocation. */
    const bool on = event->scheduling.flags & SCHEDULING_FLAG_ON;
    const bool settree = event->scheduling.flags & SCHEDULING_FLAG_SETTREE;

    if (event->scheduling.pid == 0 && !on)
        return;   /* nothing to restore; no focused client reported */

    if (topapp_set(s, event->scheduling.pid, on, settree) != 0)
        LOGE("settopapp: set pid=%d on=%d tree=%d failed",
             (int)event->scheduling.pid, on, settree);
    else
        LOGI("settopapp: pid %d %s (%s)", (int)event->scheduling.pid,
             on ? "promoted" : "restored",
             settree ? "tree" : "process");
}

static int do_connect(struct consumer_state *s)
{
    /* Snapshot the connection config for this attempt. */
    pthread_mutex_lock(&s->cfg_lock);
    bool use_root = s->cfg_use_root;
    char sock_path[sizeof(s->cfg_socket_path)];
    char helper_path[sizeof(s->cfg_helper_path)];
    char bridge_path[sizeof(s->cfg_bridge_path)];
    memcpy(sock_path, s->cfg_socket_path, sizeof(sock_path));
    memcpy(helper_path, s->cfg_helper_path, sizeof(helper_path));
    memcpy(bridge_path, s->cfg_bridge_path, sizeof(bridge_path));
    s->topapp_run_enable = s->cfg_topapp_enable;
    memcpy(s->topapp_run_path, s->cfg_topapp_path, sizeof(s->topapp_run_path));
    s->topapp_run_mode = s->cfg_topapp_mode == 2 ? 2 : 1;
    memcpy(s->topapp_run_stops, s->cfg_topapp_stops,
           sizeof(s->topapp_run_stops));
    pthread_mutex_unlock(&s->cfg_lock);

    const char *sock = sock_path;

    /* A boosted producer from the previous session must not keep its top-app
     * priority across a reconnect cycle; restore the whole tree before the
     * old ctx dies. */
    topapp_restore_tree(s);

    if (s->ctx) {
        audio_set_ctx(s->audio, NULL);   /* detach audio before the old ctx (and its fd) dies */
        stop_event_thread(s);
        join_event_thread(s);
        disconnect(s->ctx);
        s->ctx = NULL;
    }
    cleanup_dmabufs(s);

    ANativeWindow *win = s->window;
    pthread_mutex_lock(&s->cfg_lock);
    int cw = s->cfg_custom_width;
    int ch = s->cfg_custom_height;
    pthread_mutex_unlock(&s->cfg_lock);

    if (cw > 0 && ch > 0) {
        s->screen_w = cw;
        s->screen_h = ch;
    } else {
       s->screen_w = ANativeWindow_getWidth(win);
       s->screen_h = ANativeWindow_getHeight(win);
    }
    if (s->screen_w <= 0 || s->screen_h <= 0) {
        LOGE("invalid Surface geometry request: %dx%d",
             s->screen_w, s->screen_h);
        return -1;
    }

    /* dequeueBuffer needs the window connected to an API first (ANativeWindow_lock
     * did this internally). Disconnect first so reconnect is idempotent. */
    anw_api_disconnect(win, ANW_API_CPU);
    if (anw_api_connect(win, ANW_API_CPU) != 0) {
        LOGE("api_connect(CPU) failed");
        return -1;
    }
    const uint64_t cpu_usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
                             | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    const int usage_result = api.setUsage(win, cpu_usage);
    LOGI("setUsage(CPU_READ_OFTEN|CPU_WRITE_OFTEN)=%d", usage_result);
    if (usage_result != 0)
        return -1;


    const int geometry_result =
        ANativeWindow_setBuffersGeometry(
            win, s->screen_w, s->screen_h,
            AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    if (geometry_result != 0) {
        LOGE("setBuffersGeometry(%dx%d, RGBA8888) failed: %d",
             s->screen_w, s->screen_h, geometry_result);
        return -1;
    }

    int min_undequeued = 0;
    const int query_result =
        api.query(win, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS,
                  &min_undequeued);
    if (query_result != 0 || min_undequeued < 0 ||
        min_undequeued > MAX_COLLECT_BUFS - 2) {
        LOGE("query(min undequeued) failed or unsupported: result=%d value=%d",
             query_result, min_undequeued);
        return -1;
    }
    const int total = min_undequeued + 2;
    const int count_result = api.setBufferCount(win, (size_t)total);
    if (count_result != 0) {
        LOGE("setBufferCount(%d) failed: %d", total, count_result);
        return -1;
    }

    s->buf_count = total;
    if (collect_dmabufs(s) < 0)
        return -1;

    LOGI("connecting to %s (%dx%d, %d bufs, root=%d)", sock,
         s->screen_w, s->screen_h, s->buf_count, use_root);

    if (use_root) {
        int ctrl_fd = recv_fd_via_root_helper(sock, helper_path, bridge_path);
        if (ctrl_fd < 0) {
            LOGE("root helper connect failed");
            return -1;
        }
        if (connect_to_deamon_with_fd(&s->ctx, ctrl_fd) < 0) {
            LOGE("connect_to_deamon_with_fd failed");
            return -1;
        }
    } else if (connect_to_deamon(&s->ctx, sock) < 0) {
        LOGE("connect_to_deamon failed");
        return -1;
    }

    set_screen_info(s->ctx, s->screen_w, s->screen_h,
                    PIXEL_FORMAT_RGBA_8888, s->refresh_mhz);
    push_dmabufs(s->ctx, s->dmabuf_fds, s->dmabuf_infos, s->buf_count);

    /* Register the camera service only when it was initialised (i.e. the user
     * enabled it in settings and granted CAMERA). The service_info lives in this
     * per-instance state (outlives the ctx) and carries userdata=s so the camera
     * layer knows which instance's client to serve. The producer drives it via
     * RESOURCES_REQUEST (handled on the event thread). */
    if (camera_service_is_ready()) {
        s->camera_svc.type = SERVICE_TYPE_CAMERA;
        s->camera_svc.allocate_resource = camera_allocate_resource;
        s->camera_svc.free_resource = camera_free_resource;
        s->camera_svc.userdata = s;
        allocate_services(s->ctx, &s->camera_svc, 1);
    }

    set_fallback_callback(s->ctx, on_fallback, s);
    set_exit_fallback_callback(s->ctx, on_exit_fallback, s);

    audio_set_ctx(s->audio, s->ctx);   /* audio fd is now live; threads pick it up via get_audio_fd */

    s->need_reconnect = false;
    LOGI("connected");
    return 0;
}

static void on_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    LOGI("fallback triggered");

    /* The producer is gone (or the link broke): its top-app boost must end
     * now, before any process can leak foreground priority for a whole
     * session. One tree sweep covers every pid either mode boosted. */
    topapp_restore_tree(s);

    audio_set_ctx(s->audio, NULL);   /* the old audio channel is invalid; stop touching it */

    /* Let the owning MainActivity probe the daemon socket and close the window if the
     * daemon is gone. onFallback() marshals itself to the UI thread on the Java side. */
    if (g_jvm && s->activity_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->activity_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "onFallback", "()V");
            if (mid)
                (*env)->CallVoidMethod(env, s->activity_obj, mid);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    // Disable clip listener on Java side before stopping event thread
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env)->CallVoidMethod(env, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    stop_event_thread(s);
}

static void on_exit_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    LOGI("exit fallback triggered");

    /* on_fallback() detached the bridge after its old socket was closed. Restore
     * the new connection before top-app discovery or any JNI early return. */
    audio_set_ctx(s->audio, s->ctx);

    /* Producer (re)connected. Mode 2 promotes the whole tree right here;
     * mode 1 only discovers the namespace anchor now -- the per-pid boosts
     * happen when the event thread drains KWin's producer-identity and
     * active-window reports queued on the data socket. Runs before anything
     * JNI below so a failure here never breaks the session restart.
     * cfg_topapp_enable is read locklessly like every other cfg read on this
     * path (Java only reconfigures between connections). */
    if (s->topapp_run_enable && s->topapp_run_path[0] != '\0') {
        const int data_fd = get_data_fd(s->ctx);
        pid_t root = topapp_discover_root(s, data_fd, s->topapp_run_mode == 2);
        if (data_fd >= 0)
            close(data_fd);
        if (root > 0) {
            pthread_mutex_lock(&s->topapp_lock);
            s->topapp_root_pid = root;
            pthread_mutex_unlock(&s->topapp_lock);
        }
    }

    send_refresh_rate(s);

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("on_exit_fallback: AttachCurrentThread failed");
        return;
    }

    // Enable clip listener on Java side
    jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID listenMid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
    if (listenMid)
        (*env)->CallVoidMethod(env, s->clipboard_obj, listenMid, JNI_TRUE);

    start_event_thread(s);

    // Initial clipboard sync: read current system clipboard and send to producer
    jmethodID syncMethod = (*env)->GetMethodID(env, cls, "nativeClipboardSync", "()V");
    if (syncMethod)
        (*env)->CallVoidMethod(env, s->clipboard_obj, syncMethod);

    (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void *render_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("render thread started");

    while (s->running) {
        if (s->need_reconnect) {
            LOGI("reconnecting...");
            TracyCZoneN(zConnect, "do_connect", 1);
            int rc = do_connect(s);
            TracyCZoneEnd(zConnect);
            if (rc < 0) {
                usleep(500000);
                continue;
            }
        }

        ANativeWindowBuffer *anb = NULL;
        int acquire_fence = -1;
        TracyCZoneN(zDequeue, "dequeueBuffer", 1);
        const int dequeue_result =
            api.dequeueBuffer(s->window, &anb, &acquire_fence);
        TracyCZoneEnd(zDequeue);
        if (dequeue_result != 0 || !anb) {
            if (acquire_fence >= 0)
                close(acquire_fence);
            usleep(16000);
            continue;
        }

        /* The producer may write only after SurfaceFlinger's acquire fence
         * signals. On timeout/error, pass the still-owned fence back through
         * cancelBuffer; never close it and continue rendering. */
        if (acquire_fence >= 0) {
            TracyCZoneN(zAcqFence, "acquire fence wait", 1);
            const bool ready = wait_for_acquire_fence(acquire_fence);
            TracyCZoneEnd(zAcqFence);
            if (!ready) {
                finish_dequeued_buffer(s->window, anb, acquire_fence, false);
                usleep(16000);
                continue;
            }
            close(acquire_fence);
            acquire_fence = -1;
        }

        int idx = -1;
        for (int i = 0; i < s->buf_count; i++) {
            if (s->buf_anb[i] == anb) {
                idx = i;
                break;
            }
        }

        if (idx < 0) {
            finish_dequeued_buffer(s->window, anb, -1, false);
            usleep(16000);
            continue;
        }

        TracyCZoneN(zSelect, "select_dmabuf", 1);
        const int select_result = select_dmabuf(s->ctx, idx);
        TracyCZoneEnd(zSelect);
        if (select_result < 0) {
            finish_dequeued_buffer(s->window, anb, -1, false);
            usleep(16000);
            continue;
        }

        /* The producer writes this exact Android Surface DMA-BUF. A successful
         * completion may return an optional render fence; queueBuffer takes
         * ownership so SurfaceFlinger waits before reading. Failure means no
         * completion was observed, so cancel rather than present the buffer. */
        int render_fence = -1;
        TracyCZoneN(zRefresh, "refresh_done (direct Surface render)", 1);
        const int refresh_result = refresh_done(s->ctx, &render_fence);
        TracyCZoneEnd(zRefresh);
        if (refresh_result < 0) {
            LOGE("refresh completion failed; cancelling dequeued buffer");
            if (finish_dequeued_buffer(s->window, anb, -1, false) != 0)
                s->need_reconnect = true;
            usleep(16000);
            continue;
        }
        if (finish_dequeued_buffer(s->window, anb, render_fence, true) != 0) {
            s->need_reconnect = true;
            usleep(16000);
            continue;
        }
        TracyCFrameMark;
    }

    LOGI("render thread stopped");
    return NULL;
}

/* ---------- JNI ---------- */

static void copy_jstring(JNIEnv *env, jstring js, char *dst, size_t dstsz)
{
    if (!js) {
        dst[0] = '\0';
        return;
    }
    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    if (s) {
        strncpy(dst, s, dstsz - 1);
        dst[dstsz - 1] = '\0';
        (*env)->ReleaseStringUTFChars(env, js, s);
    } else {
        dst[0] = '\0';
    }
}

/* Every JNI entry point below takes a jlong handle -- the consumer_state* returned
 * by nativeCreate -- so multiple instances (windows) coexist in one process. */
#define STATE(h) ((struct consumer_state *)(uintptr_t)(h))

JNIEXPORT jlong JNICALL
Java_com_anland_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer_state *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->cfg_lock, NULL);
    pthread_mutex_init(&s->topapp_lock, NULL);
    s->cfg_topapp_mode = 1;
    strncpy(s->cfg_socket_path, "/data/local/tmp/display_daemon.sock",
            sizeof(s->cfg_socket_path) - 1);
    s->audio = audio_create();
    LOGI("instance %p created", (void *)s);
    return (jlong)(uintptr_t)s;
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    /* Stop the transport (mirrors nativeStop), release the camera client + audio
     * bridge, then free. */
    pthread_mutex_lock(&s->lock);
    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }
    if (s->ctx) {
        stop_event_thread(s);
        join_event_thread(s);
        topapp_restore_tree(s);
        disconnect(s->ctx);
        s->ctx = NULL;
    } else {
        topapp_restore_tree(s);
    }
    cleanup_dmabufs(s);
    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }
    pthread_mutex_unlock(&s->lock);

    audio_destroy(s->audio);
    s->audio = NULL;
    camera_release_client(s);   /* window gone: tear down its camera channels */

    if (s->clipboard_obj && g_jvm) {
        JNIEnv *e = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = ((*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL) == 0);
        if (e) {
            (*e)->DeleteGlobalRef(e, s->clipboard_obj);
            if (s->activity_obj)
                (*e)->DeleteGlobalRef(e, s->activity_obj);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    s->activity_obj = NULL;
    pthread_mutex_destroy(&s->topapp_lock);
    pthread_mutex_destroy(&s->cfg_lock);
    pthread_mutex_destroy(&s->lock);
    LOGI("instance %p destroyed", (void *)s);
    free(s);
}

/* Mark this instance focused (real camera frames go to the focused instance; others
 * get blank frames). Called from Java on window focus gain. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetFocused(
    JNIEnv *env, jclass clazz, jlong handle, jboolean focused)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (s && focused)
        camera_set_focus(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeConfigure(
    JNIEnv *env, jclass clazz, jlong handle, jstring socketPath, jboolean useRoot,
    jstring helperPath, jstring bridgePath, jboolean topappEnable, jstring topappPath,
    jint topappMode, jstring topappStops)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    char tmp[sizeof(s->cfg_socket_path)];
    copy_jstring(env, socketPath, tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        memcpy(s->cfg_socket_path, tmp, sizeof(s->cfg_socket_path));
    s->cfg_use_root = (useRoot == JNI_TRUE);
    copy_jstring(env, helperPath, s->cfg_helper_path, sizeof(s->cfg_helper_path));
    copy_jstring(env, bridgePath, s->cfg_bridge_path, sizeof(s->cfg_bridge_path));
    s->cfg_topapp_enable = (topappEnable == JNI_TRUE);
    copy_jstring(env, topappPath, s->cfg_topapp_path, sizeof(s->cfg_topapp_path));
    s->cfg_topapp_mode = topappMode == 2 ? 2 : 1;
    copy_jstring(env, topappStops, s->cfg_topapp_stops,
                 sizeof(s->cfg_topapp_stops));
    for (char *p = s->cfg_topapp_stops; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) {
            s->cfg_topapp_stops[0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("configured: socket=%s root=%d helper=%s bridge=%s topapp=%d mode=%d stops=%s",
         s->cfg_socket_path, s->cfg_use_root, s->cfg_helper_path, s->cfg_bridge_path,
         s->cfg_topapp_enable, s->cfg_topapp_mode, s->cfg_topapp_stops);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetCustomResolution(
    JNIEnv* env, jclass clazz, jlong handle, jint width, jint height)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    s->cfg_custom_width = width;
    s->cfg_custom_height = height;
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("custom resolution: %dx%d", width, height);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStart(
    JNIEnv *env, jclass clazz, jlong handle, jobject surface, jobject clipboardTarget,
    jobject activityTarget)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    if (!api_loaded) {
        if (anw_api_load(&api) < 0) {
            LOGE("failed to load ANativeWindow hidden API");
            return;
        }
        api_loaded = true;
    }

    pthread_mutex_lock(&s->lock);

    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }

    if (s->ctx) {
        disconnect(s->ctx);
        s->ctx = NULL;
    }
    s->motion_has_last = false;
    cleanup_dmabufs(s);

    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }

    s->window = ANativeWindow_fromSurface(env, surface);
    if (!s->window) {
        LOGE("ANativeWindow_fromSurface failed");
        pthread_mutex_unlock(&s->lock);
        return;
    }

    /* Save JVM (process-global) and this instance's clipboard callback target. */
    if (!g_jvm) {
        (*env)->GetJavaVM(env, &g_jvm);
    }
    if (s->clipboard_obj) {
        (*env)->DeleteGlobalRef(env, s->clipboard_obj);
    }
    /* Static natives have no `thiz`; the Java layer passes the object whose
     * nativeSetClipboardText / nativeClipListening / nativeClipboardSync the
     * event thread calls back into (the Clipboard instance). */
    s->clipboard_obj = (*env)->NewGlobalRef(env, clipboardTarget);

    /* Owning MainActivity for the fallback callback (see on_fallback). */
    if (s->activity_obj) {
        (*env)->DeleteGlobalRef(env, s->activity_obj);
    }
    s->activity_obj = activityTarget ? (*env)->NewGlobalRef(env, activityTarget) : NULL;

    s->running = true;
    s->need_reconnect = true;
    pthread_create(&s->render_thread, NULL, render_thread_func, s);

    /* Audio streams live independently of the connection; the render thread attaches
     * the fd via audio_set_ctx() once connected. */
    audio_start(s->audio);

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->lock);

    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }

    /* Stop audio before the ctx (and its fd) is torn down. */
    audio_set_ctx(s->audio, NULL);
    audio_stop(s->audio);

    if (s->ctx) {
        stop_event_thread(s);
        join_event_thread(s);
        topapp_restore_tree(s); /* stopping the pipeline ends the boost */
        disconnect(s->ctx);
        s->ctx = NULL;
    } else {
        topapp_restore_tree(s);
    }

    // Disable clip listener on Java side
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env2 = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env2, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env2, NULL) == 0)
                attached = true;
        }
        if (env2) {
            jclass cls = (*env2)->GetObjectClass(env2, s->clipboard_obj);
            jmethodID mid = (*env2)->GetMethodID(env2, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env2)->CallVoidMethod(env2, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    cleanup_dmabufs(s);

    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetRefreshRate(
    JNIEnv *env, jclass clazz, jlong handle, jfloat hz)
{
    struct consumer_state *s = STATE(handle);
    if (!s || hz <= 0.0f)
        return;
    s->refresh_mhz = (uint32_t)(hz * 1000.0f + 0.5f);
    // Apply live if already connected; otherwise do_connect() seeds it.
    send_refresh_rate(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouch(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jfloat x, jfloat y, jint pointer_id)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action, .x = x, .y = y, .pointer_id = pointer_id },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouchFrame(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH_FRAME,
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendKey(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jint keycode)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_KEY,
        .key = { .action = action, .keycode = keycode },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseMotion(
    JNIEnv *env, jclass clazz, jlong handle, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;

    if (dx == 0.0f && dy == 0.0f && s->motion_has_last) {
        dx = x - s->motion_last_x;
        dy = y - s->motion_last_y;
    }

    s->motion_last_x = x;
    s->motion_last_y = y;
    s->motion_has_last = true;

    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_MOTION,
        .pointer_motion = { .x = x, .y = y, .dx = dx, .dy = dy },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseButton(
    JNIEnv *env, jclass clazz, jlong handle, jint button, jboolean pressed)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_BUTTON,
        .pointer_button = { .button = button, .pressed = pressed ? 1 : 0 },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseScroll(
    JNIEnv *env, jclass clazz, jlong handle, jint axis, jfloat value)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_AXIS,
        .pointer_axis = { .axis = axis, .value = value, .discrete = 0 },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendClipboard(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = (uint32_t)len },
    };
    push_input_event_with_length(s->ctx, &ev, buf, len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTextInput(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_TEXT_INPUT,
        .text_input = { .size = (uint32_t)len },
    };
    push_input_event_with_length(s->ctx, &ev, buf, len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetMicEnabled(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_mic_enabled(s->audio, enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioLatency(
    JNIEnv *env, jclass clazz, jlong handle, jint speakerMs, jint micMs)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_latency(s->audio, speakerMs, micMs);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioKeepalive(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_keepalive(s->audio, enabled == JNI_TRUE);
}
