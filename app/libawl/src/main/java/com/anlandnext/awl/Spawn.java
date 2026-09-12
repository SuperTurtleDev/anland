package com.anlandnext.awl;

/**
 * fork+exec helper (libawlspawn.so): Java's Runtime.exec cannot pass open
 * fds to child processes, and the wayland connection handoff needs a real
 * fork — the child inherits the fd across fork+exec and finds it via the
 * WAYLAND_SOCKET environment variable (the libwayland-client convention).
 * The child's stdio is wired to pipes whose parent ends are handed back,
 * so plain-printf clients stay loggable from Java. Plumbing only.
 */
final class Spawn {
    static {
        System.loadLibrary("awlspawn");
    }

    /**
     * fd = raw int (the caller detached it — this side owns and closes it).
     * Returns { pid, stdinFd, stdoutFd, stderrFd } (raw ints, adopted by
     * the caller via ParcelFileDescriptor.adoptFd), or null on failure
     * (all fds closed either way). The child is reaped by the helper.
     */
    static native int[] nativeSpawn(int fd, String file, String[] args);

    private Spawn() { }
}
