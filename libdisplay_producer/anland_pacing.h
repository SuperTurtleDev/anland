/* Producer-side frame pacing measurements; this does not alter the wire ABI. */
#ifndef ANLAND_PACING_H
#define ANLAND_PACING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_pacing {
    uint64_t frames_submitted;
    uint64_t frames_flipped;
    uint64_t frames_acked;
    uint64_t submit_to_flip_last_ns;
    uint64_t submit_to_flip_mean_ns;
    uint64_t submit_to_flip_worst_ns;
    uint64_t flip_to_ack_last_ns;
    uint64_t flip_to_ack_mean_ns;
    uint64_t flip_to_ack_worst_ns;
    uint64_t frame_lifetime_worst_ns;
    uint64_t frames_over_interval;
    uint64_t frames_dropped;
    uint64_t submit_to_flip_samples;
    uint64_t flip_to_ack_samples;
    uint64_t refresh_interval_ns;
} anland_pacing;

uint64_t anland_pacing_now_ns(void);
void anland_pacing_reset(anland_pacing *p);
void anland_pacing_note_interval(anland_pacing *p, uint64_t refresh_mhz);
void anland_pacing_note_submitted(anland_pacing *p);
void anland_pacing_note_flip(anland_pacing *p, uint64_t submitted_at_ns,
                             uint64_t flipped_at_ns);
void anland_pacing_note_ack(anland_pacing *p, uint64_t submitted_at_ns,
                            uint64_t flipped_at_ns, uint64_t acked_at_ns);
void anland_pacing_note_dropped(anland_pacing *p);
int anland_pacing_format(const anland_pacing *p, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
