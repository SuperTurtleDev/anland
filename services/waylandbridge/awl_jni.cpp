/* awl_jni.cpp — adapter-layer JNI (v2 window driven)
 *
 * Java (service/Activity) ↔ logic layer + GPU renderer.
 * Protocol events → Java uplink: AwlHost.onWindowCreated/onWindowDestroyed/onWindowTitle
 */
#include "awl.h"
#include "awl_renderer.hpp"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>

#define AWL_TAG "anland-adapt"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

static JavaVM* g_vm;
static jclass g_host_cls;
static jmethodID g_mid_win_created;
static jmethodID g_mid_win_destroyed;
static jmethodID g_mid_win_title;
static uid_t g_app_uid = 0;   /* app uid recorded at process load (before privilege elevation) */

/* ---------------- Java uplink ---------------- */

static JNIEnv* attach_thread(bool* attached) {
    JNIEnv* env = nullptr;
    *attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        *attached = true;
        return env;
    }
    return nullptr;
}

static void call_void1(jmethodID mid, jlong a) {
    JNIEnv* env; bool at;
    if ((env = attach_thread(&at)) == nullptr || !g_host_cls) { if (at) g_vm->DetachCurrentThread(); return; }
    env->CallStaticVoidMethod(g_host_cls, mid, a);
    if (at) g_vm->DetachCurrentThread();
}

static void call_win(jmethodID mid, jlong id, jint w, jint h,
                     const char* title, jint popup) {
    JNIEnv* env; bool at;
    if ((env = attach_thread(&at)) == nullptr || !g_host_cls) { if (at) g_vm->DetachCurrentThread(); return; }
    jstring jtitle = title ? env->NewStringUTF(title) : nullptr;
    if (popup >= 0)
        env->CallStaticVoidMethod(g_host_cls, mid, id, w, h, jtitle, popup);
    else
        env->CallStaticVoidMethod(g_host_cls, mid, id, jtitle);
    if (jtitle) env->DeleteLocalRef(jtitle);
    if (at) g_vm->DetachCurrentThread();
}

/* ---------------- logic-layer callbacks (wayland event thread) ---------------- */

static void cb_window_created(void* user, uint64_t id, int32_t w, int32_t h,
                              const char* title, int is_popup) {
    LOGI("window %llu created %dx%d popup=%d '%s'",
         (unsigned long long)id, w, h, is_popup, title ? title : "");
    call_win(g_mid_win_created, (jlong)id, w, h, title, is_popup);
}

static void cb_window_destroyed(void* user, uint64_t id) {
    LOGI("window %llu destroyed", (unsigned long long)id);
    awl_renderer_attach(id, nullptr);   /* release GL resources first */
    call_void1(g_mid_win_destroyed, (jlong)id);
}

static void cb_window_title(void* user, uint64_t id, const char* title) {
    call_win(g_mid_win_title, (jlong)id, 0, 0, title, -1);
}

static void cb_window_dirty(void* user, uint64_t id) {
    awl_renderer_request_render(id);   /* render thread does get_buffer → GL → swap */
}

static awl_window_callbacks_t k_cbs = {
    .user = nullptr,
    .window_created = cb_window_created,
    .window_destroyed = cb_window_destroyed,
    .window_title = cb_window_title,
    .window_dirty = cb_window_dirty,
};

/* ---------------- socket ---------------- */

static int create_listen_socket(const char* path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOGE("socket path too long: %s", path);
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, path);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        LOGE("bind/listen %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0666);   /* any uid in the container can connect (/data/local/tmp debug area) */
    LOGI("wayland socket: %s", path);
    return fd;
}

/* ---------------- JNI entry ---------------- */

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    g_app_uid = getuid();
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
        jclass local = env->FindClass("com/anlandnext/AwlHost");
        if (local) {
            g_host_cls = (jclass)env->NewGlobalRef(local);
            g_mid_win_created = env->GetStaticMethodID(
                    g_host_cls, "onWindowCreated", "(JIILjava/lang/String;I)V");
            g_mid_win_destroyed = env->GetStaticMethodID(
                    g_host_cls, "onWindowDestroyed", "(J)V");
            g_mid_win_title = env->GetStaticMethodID(
                    g_host_cls, "onWindowTitle", "(JLjava/lang/String;)V");
        } else {
            LOGE("AwlHost class not found");
        }
    }
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_anlandnext_AwlHost_nativeStart(JNIEnv* env, jclass,
                                        jstring socket_path,
                                        jint w, jint h, jint hz, jint dpi) {
    const char* path = env->GetStringUTFChars(socket_path, nullptr);
    int fd = create_listen_socket(path);
    env->ReleaseStringUTFChars(socket_path, path);
    if (fd < 0) return -1;
    awl_display_info_t info = {};
    info.width = (uint32_t)w;
    info.height = (uint32_t)h;
    info.refresh_hz = hz > 0 ? hz : 60;
    info.dpi = dpi;
    info.scale = 1;
    return awl_server_start(fd, &info, &k_cbs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_anlandnext_AwlHost_nativeStop(JNIEnv*, jclass) {
    awl_server_stop();
    awl_renderer_shutdown();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_anlandnext_AwlHost_nativeIsRunning(JNIEnv*, jclass) {
    return awl_server_is_running();
}

/* ---------------- SukiSU(KSU) in-place privilege elevation ----------------
 * Path (SukiSU-Ultra kernel/supercall):
 *   1. Grant probe: sucompat rewrites faccessat("/system/bin/su") of granted
 *      processes to sh → returns 0; when not granted the real file does not
 *      exist → ENOENT.
 *      Never call reboot when not granted — app seccomp has no allowance,
 *      SIGSYS kills the process.
 *   2. syscall(reboot, 0xDEADBEEF, 0xCAFEBABE, 0, &fd) → kprobe installs the [ksu_driver] fd
 *      (the granted process's seccomp cache already allows __NR_reboot via
 *      the setresuid hook)
 *   3. ioctl(fd, GRANT_ROOT) → escape_with_root_profile() → commit root creds
 *
 * root is only used to create the socket dir; setresuid back to app uid
 * immediately after — a uid 0 process sending notifications/startActivity
 * gets rejected by system_server (pkg uid mismatch).
 */
#define KSU_INSTALL_MAGIC1 0xDEADBEEFu
#define KSU_INSTALL_MAGIC2 0xCAFEBABEu
#define KSU_IOCTL_GRANT_ROOT _IOC(_IOC_NONE, 0x4B, 1, 0)
#define AWL_SOCK_DIR "/data/local/tmp/awl"

extern "C" JNIEXPORT jint JNICALL
Java_com_anlandnext_AwlHost_nativeBecomeRoot(JNIEnv*, jclass) {
    if (geteuid() == 0 && access(AWL_SOCK_DIR, F_OK) == 0) return 0;
    if (faccessat(AT_FDCWD, "/system/bin/su", F_OK, 0) != 0) {
        LOGI("ksu: not granted (su invisible)");
        return -1;
    }
    int fd = -1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
    if (fd < 0) {
        LOGE("ksu: install fd failed (%s)", strerror(errno));
        return -1;
    }
    long rc = ioctl(fd, KSU_IOCTL_GRANT_ROOT, 0UL);
    close(fd);
    LOGI("ksu: grant root rc=%ld euid=%d", rc, geteuid());
    if (rc != 0 || geteuid() != 0) return -1;

    /* one-time root prep: 0777 socket dir (any uid in the container can write) */
    mkdir(AWL_SOCK_DIR, 0777);
    chmod(AWL_SOCK_DIR, 0777);

    /* restore app uid — the whole Java/binder chain works as a normal app */
    uid_t app = (uid_t)g_app_uid;
    if (setresuid(app, app, app) != 0) {
        LOGE("ksu: restore app uid %d failed (%s)", app, strerror(errno));
        return -1;
    }
    LOGI("ksu: restored app uid=%d", getuid());
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_anlandnext_AwlHost_nativeWindowSurface(JNIEnv* env, jclass,
                                                jlong id, jobject surface) {
    if (!surface) {
        awl_renderer_attach((uint64_t)id, nullptr);
        return;
    }
    ANativeWindow* nw = ANativeWindow_fromSurface(env, surface);
    if (awl_renderer_attach((uint64_t)id, nw) == 0)
        awl_renderer_request_render((uint64_t)id);   /* render the first frame */
    ANativeWindow_release(nw);   /* renderer holds its own reference */
}

extern "C" JNIEXPORT void JNICALL
Java_com_anlandnext_AwlHost_nativeWindowResize(JNIEnv*, jclass,
                                               jlong id, jint w, jint h) {
    awl_window_resize((uint64_t)id, w, h);
}
