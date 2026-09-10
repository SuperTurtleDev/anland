/* awl_sched.c — foreground scheduling (attached window's client → top-app)
 *
 * Ported from the anland settopapp root helper (consumers/anland_v5/
 * android_consumer jni/settopapp.c): the whole process subtree of a wayland
 * client is moved into Android's top-app cgroups while one of its windows is
 * attached to an Activity, and back to the root groups on detach. The daemon
 * itself is boosted the same way while any window is attached (the attach
 * count lives in the adapter's g_wins — first attach boosts us, last detach
 * falls us back; the adapter calls us with getpid()).
 *
 * Everything the legacy helper needed is gone here: the daemon is a host root
 * process, so the connect-time cached client->pid (wl_client_get_credentials)
 * is already a global pid — /proc/<pid> and the cgroup files are writable
 * directly, no su, no setns, no anchor discovery, no collect-then-write
 * snapshot (that two-phase design existed for the seccomp sandbox). The walk
 * just recurses /proc/<pid>/task/<tid>/children and writes each pid as it is
 * found; the process tree is a tree (no cycles), a dead pid's write simply
 * fails and the walk moves on.
 *
 * Stateless and synchronous: no init, no worker, no refcounts — each call is
 * an independent /proc walk plus per-pid append writes (cgroup.procs takes
 * one "pid\n" command per write, atomic), so concurrent calls are safe. The
 * callers run on binder/dispatch threads with no daemon locks held; an
 * attached tree of a few dozen processes costs ~ms once per attach.
 *
 * Known trade-offs (accepted, minimal by design):
 *   - no per-client refcount: closing one of several windows of the same
 *     client (chrome multi-window = one connection) restores the whole tree
 *     until the next attach/detach event for that client;
 *   - an Xwayland window boosts the Xwayland process tree only — X clients
 *     are its socket peers, not its children (mini-wm could report
 *     _NET_WM_PID later).
 */
#include "awl_log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Android cgroup mounts (same paths as settopapp): cpuctl = scheduling
 * policy, cpuset = big-core placement; on = the framework's top-app group,
 * off = each hierarchy's root group. A missing hierarchy (open fails) is
 * skipped — logged once per file, never per call. */
static const char k_cpuctl_on[]  = "/dev/cpuctl/top-app/cgroup.procs";
static const char k_cpuctl_off[] = "/dev/cpuctl/cgroup.procs";
static const char k_cpuset_on[]  = "/dev/cpuset/top-app/cgroup.procs";
static const char k_cpuset_off[] = "/dev/cpuset/cgroup.procs";

/* Fork-bomb guard: the walk writes as it goes, so bound the total instead
 * of a snapshot table. A desktop client tree is a few dozen processes. */
#define AWL_SCHED_MAX_PIDS 1024

/* One LOGE per missing hierarchy file (bit i of an atomic mask), not per
 * call — a device without /dev/cpuset must not spam every attach. */
static void log_open_fail(const char* path)
{
    static const char* files[4] = { k_cpuctl_on, k_cpuctl_off, k_cpuset_on, k_cpuset_off };
    static _Atomic unsigned mask;
    for (int i = 0; i < 4; i++)
        if (files[i] == path) {
            if (!(atomic_fetch_or(&mask, 1u << i) & (1u << i)))
                LOGE("sched: cannot open %s (%s) — hierarchy skipped",
                     path, strerror(errno));
            return;
        }
}

static void write_pid(const char* path, pid_t pid)
{
    /* O_APPEND so repeated pid writes accumulate (cgroupfs takes each write
     * as one command and ignores the offset; the flag keeps behaviour
     * identical against a plain file too). */
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        log_open_fail(path);
        return;
    }
    (void)!write(fd, buf, (size_t)len);   /* dead pid → kernel rejects, walk moves on */
    close(fd);
}

/* Write pid into both target groups, then recurse into every child listed
 * under /proc/<pid>/task/<tid>/children (children hang off their parent
 * thread, so every thread's file must be scanned). */
static void sched_walk(pid_t pid, int on, int* count)
{
    if (*count >= AWL_SCHED_MAX_PIDS)
        return;
    write_pid(on ? k_cpuctl_on : k_cpuctl_off, pid);
    write_pid(on ? k_cpuset_on : k_cpuset_off, pid);
    (*count)++;

    char tpath[64];
    snprintf(tpath, sizeof(tpath), "/proc/%d/task", (int)pid);
    DIR* d = opendir(tpath);
    if (!d)
        return;   /* process died mid-walk — nothing left under it */
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        char cpath[96];
        snprintf(cpath, sizeof(cpath), "/proc/%d/task/%s/children",
                 (int)pid, de->d_name);
        FILE* f = fopen(cpath, "r");
        if (!f)
            continue;
        int child;
        while (fscanf(f, "%d", &child) == 1)
            if (child > 0)
                sched_walk((pid_t)child, on, count);
        fclose(f);
    }
    closedir(d);
}

void awl_sched_set(pid_t pid, int on)
{
    if (pid <= 0)
        return;
    int count = 0;
    sched_walk(pid, on, &count);
    LOGI("sched: %s root %d → %d pids", on ? "ON" : "OFF", (int)pid, count);
}
