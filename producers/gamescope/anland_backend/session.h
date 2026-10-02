/* Gamescope-specific shape adapter. All presentation/transport ownership is
 * delegated to the one public producer foundation; no Gamescope types here. */
#ifndef ANLAND_GAMESCOPE_SESSION_H
#define ANLAND_GAMESCOPE_SESSION_H
#include "anland_de_backend.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_gamescope_session anland_gamescope_session;
typedef struct anland_gamescope_target {
    anland_de_target_t selection;
    anland_buffer_desc_t buffer; /* fd is caller-owned; initialize/close it */
} anland_gamescope_target;

/* First implementation is explicitly legacy-only, never silently downgrades AWL. */
anland_gamescope_session *anland_gamescope_session_open(const char *endpoint);
void anland_gamescope_session_close(anland_gamescope_session *session);
int anland_gamescope_session_pump(anland_gamescope_session *session, int timeout_ms);
int anland_gamescope_session_dispatch(anland_gamescope_session *session,
    anland_scene_event_t *events, size_t capacity, size_t *count);
int anland_gamescope_session_output(anland_gamescope_session *session,
    anland_device_output_t *output);
int anland_gamescope_session_target(anland_gamescope_session *session,
    anland_gamescope_target *target);
/* Validation must run before rendering and again after GPU completion. */
int anland_gamescope_session_target_current(anland_gamescope_session *session,
    const anland_gamescope_target *target);
/* acquire_fence_fd is BORROWED; -1 is legal only after GPU work is complete. */
int anland_gamescope_session_submit(anland_gamescope_session *session,
    const anland_gamescope_target *target, int acquire_fence_fd, uint64_t *commit_id);
int anland_gamescope_session_reconnect(anland_gamescope_session *session);
int anland_gamescope_session_reopen(anland_gamescope_session *session, const char *endpoint);
void anland_gamescope_session_drop(anland_gamescope_session *session);
/* Borrowed public device for input/audio integration, never close it directly. */
anland_device *anland_gamescope_session_device(anland_gamescope_session *session);
#ifdef __cplusplus
}
#endif
#endif
