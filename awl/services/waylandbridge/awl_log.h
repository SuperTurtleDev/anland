/* awl_log.h — logging macros shared by every native layer
 *
 * LOGI  lifecycle milestones (client/window creation and teardown, state
 *       transitions, one-shot init diagnostics) — kept in release builds
 * LOGE  errors and abnormal conditions — always kept
 * LOGD  hot-path tracing (event-loop dispatch: per-attach / per-commit /
 *       per-frame / per-buffer requests, render-thread buffer imports) —
 *       compiled out unless AWL_LOG_DEBUG is defined. Release builds (the
 *       Makefile default) do not define it; `make native-debug` (or cmake
 *       -DAWL_LOG_DEBUG=ON) builds it back in. When compiled out, the
 *       arguments are not evaluated.
 *
 * Per-file tag: #define AWL_TAG "..." before including this header
 * (default "anland-wl").
 */
#ifndef AWL_LOG_H
#define AWL_LOG_H

/*
 * Host vs bionic logging.
 *
 *   AWL_LOG_BIONIC (Android cross build): route through android/log.h →
 *   logcat, exactly as before.
 *
 *   default (host build, e.g. `cmake -S awl -B build/awl` on the Debian
 *   container): plain stderr — lets the whole logic layer be compiled and
 *   tested locally without an NDK sysroot. Everything else (protocol state
 *   machines, locks, tablet, ...) is identical in both modes.
 */

#ifdef AWL_LOG_BIONIC

#include <android/log.h>

#ifndef AWL_TAG
#define AWL_TAG "anland-wl"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  AWL_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, AWL_TAG, __VA_ARGS__)

#ifdef AWL_LOG_DEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, AWL_TAG, __VA_ARGS__)
#else
#define LOGD(...) do {} while (0)
#endif

#else /* host build */

#include <stdio.h>

#ifndef AWL_TAG
#define AWL_TAG "anland-wl"
#endif

#define LOGI(fmt, ...) \
    do { fprintf(stderr, "I " AWL_TAG ": " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) \
    do { fprintf(stderr, "E " AWL_TAG ": " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGD(fmt, ...) \
    do { if (0) fprintf(stderr, "D " AWL_TAG ": " fmt "\n", ##__VA_ARGS__); } while (0)

#endif /* AWL_LOG_BIONIC */
#endif /* AWL_LOG_H */