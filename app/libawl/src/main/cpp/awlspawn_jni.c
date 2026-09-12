/* awlspawn_jni.c — fork+exec with an inherited fd + stdio pipes (libawl
 * native helper)
 *
 * Two things Java cannot express, so this helper exists:
 *   1. the wayland connection fd must be inherited across fork+exec
 *      (Runtime.exec passes no open fds);
 *   2. the child's stdio must be real pipes handed back to Java — the
 *      client logs with plain printf/fprintf (portable, future
 *      glibc/proot), and the app reads them through the returned fds.
 *
 * nativeSpawn(waylandFd, file, args) → int[]{ pid, stdinFd, stdoutFd,
 * stderrFd }: pipes for 0/1/2 are dup2'ed onto the child before execve;
 * Java gets the parent ends (stdin = write end, stdout/stderr = read
 * ends, adopted via ParcelFileDescriptor.adoptFd).
 *
 * Robustness: envp/argv are built BEFORE the fork and execve follows right
 * after (nothing mallocs or locks in the child); the parent closes the
 * child-side pipe ends AND its own wayland-fd copy (a leaked copy would
 * keep the connection alive after the child exits); the child is reaped on
 * a detached thread (no zombie, no global SIGCHLD games that would break
 * Java's own Process machinery). The wayland fd's FD_CLOEXEC is cleared
 * explicitly (libcore's Os calls set it defensively); pipe fds are created
 * O_CLOEXEC so they never leak into unrelated later execs, and dup2 on
 * 0/1/2 clears the flag on exactly the descriptors that must survive.
 */
#define _GNU_SOURCE   /* bionic: pipe2 needs __USE_GNU */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jni.h>

extern char** environ;

static void* reaper(void* arg) {
    int st;
    pid_t pid = (pid_t)(intptr_t)arg;
    while (waitpid(pid, &st, 0) < 0) {}   /* EINTR retry; ECHILD = already gone */
    return NULL;
}

static void reap_async(pid_t pid) {
    pthread_t t;
    if (pthread_create(&t, NULL, reaper, (void*)(intptr_t)pid) == 0)
        pthread_detach(t);
    else
        reaper((void*)(intptr_t)pid);   /* no thread available: reap inline */
}

JNIEXPORT jintArray JNICALL
Java_com_anlandnext_awl_Spawn_nativeSpawn(JNIEnv* env, jclass cls, jint fd,
                                          jstring file, jobjectArray args)
{
    (void)cls;
    if (fd < 0 || !file) return NULL;

    /* argv[0] = the exe path; caller args follow */
    const char* cfile = (*env)->GetStringUTFChars(env, file, NULL);
    if (!cfile) return NULL;
    jsize n = args ? (*env)->GetArrayLength(env, args) : 0;
    char** argv = calloc((size_t)n + 2, sizeof(char*));
    jsize ok = argv && (argv[0] = strdup(cfile)) != NULL;
    for (jsize i = 0; i < n && ok; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, args, i);
        const char* cs = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
        argv[i + 1] = cs ? strdup(cs) : NULL;
        if (cs && s) (*env)->ReleaseStringUTFChars(env, s, cs);
        ok = argv[i + 1] != NULL;
    }

    /* envp = environ + WAYLAND_SOCKET=<fd>, all pre-fork */
    size_t nenv = 0;
    while (environ && environ[nenv]) nenv++;
    char** envp = calloc(nenv + 2, sizeof(char*));
    char fds[32];
    if (envp) {
        for (size_t i = 0; i < nenv; i++) envp[i] = environ[i];
        snprintf(fds, sizeof fds, "WAYLAND_SOCKET=%d", fd);
        envp[nenv] = fds;
    }

    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
    pid_t pid = -1;
    if (ok && envp && argv && argv[0]
        && pipe2(in_p, O_CLOEXEC) == 0 && pipe2(out_p, O_CLOEXEC) == 0
        && pipe2(err_p, O_CLOEXEC) == 0) {
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0) (void)fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
        pid = fork();
        if (pid == 0) {
            dup2(in_p[0], 0);
            dup2(out_p[1], 1);
            dup2(err_p[1], 2);
            for (int i = 0; i < 2; i++) {
                close(in_p[i]); close(out_p[i]); close(err_p[i]);
            }
            execve(cfile, argv, envp);
            _exit(127);
        }
        /* parent: drop the child-side ends; keep ours */
        close(in_p[0]);
        close(out_p[1]);
        close(err_p[1]);
        close(fd);   /* the child owns its forked reference from here */
    } else {
        close(fd);
    }

    for (jsize i = 0; argv && argv[i]; i++) free(argv[i]);
    free(argv);
    free(envp);
    (*env)->ReleaseStringUTFChars(env, file, cfile);

    if (pid < 0) {   /* fork never happened (or failed): close what we kept */
        if (in_p[1] >= 0) close(in_p[1]);
        if (out_p[0] >= 0) close(out_p[0]);
        if (err_p[0] >= 0) close(err_p[0]);
        return NULL;
    }
    reap_async(pid);

    jint res[4] = { pid, in_p[1], out_p[0], err_p[0] };
    jintArray out = (*env)->NewIntArray(env, 4);
    if (!out) {   /* no array: the fds are ours alone now — close them */
        close(in_p[1]);
        close(out_p[0]);
        close(err_p[0]);
        return NULL;
    }
    (*env)->SetIntArrayRegion(env, out, 0, 4, res);
    return out;
}
