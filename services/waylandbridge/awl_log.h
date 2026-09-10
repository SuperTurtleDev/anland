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

#endif /* AWL_LOG_H */
