#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "anland_pacing.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

uint64_t anland_pacing_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void anland_pacing_reset(anland_pacing *p)
{
    if (p)
        memset(p, 0, sizeof(*p));
}

static void pace_mean(uint64_t *mean, uint64_t sample, uint64_t count)
{
    if (!count)
        return;
    if (sample > *mean)
        *mean += (sample - *mean) / count;
    else
        *mean -= (*mean - sample) / count;
}

void anland_pacing_note_interval(anland_pacing *p, uint64_t refresh_mhz)
{
    if (!p || !refresh_mhz || refresh_mhz > 1000000000ull)
        return;
    p->refresh_interval_ns = 1000000000000ull / refresh_mhz;
}

void anland_pacing_note_submitted(anland_pacing *p)
{
    if (p)
        p->frames_submitted++;
}

void anland_pacing_note_flip(anland_pacing *p, uint64_t submitted_at_ns,
                             uint64_t flipped_at_ns)
{
    if (!p)
        return;
    p->frames_flipped++;
    if (!submitted_at_ns || flipped_at_ns < submitted_at_ns)
        return;
    const uint64_t duration = flipped_at_ns - submitted_at_ns;
    p->submit_to_flip_last_ns = duration;
    if (duration > p->submit_to_flip_worst_ns)
        p->submit_to_flip_worst_ns = duration;
    p->submit_to_flip_samples++;
    pace_mean(&p->submit_to_flip_mean_ns, duration, p->submit_to_flip_samples);
}

void anland_pacing_note_ack(anland_pacing *p, uint64_t submitted_at_ns,
                            uint64_t flipped_at_ns, uint64_t acked_at_ns)
{
    if (!p)
        return;
    p->frames_acked++;
    if (flipped_at_ns && acked_at_ns >= flipped_at_ns) {
        const uint64_t duration = acked_at_ns - flipped_at_ns;
        p->flip_to_ack_last_ns = duration;
        if (duration > p->flip_to_ack_worst_ns)
            p->flip_to_ack_worst_ns = duration;
        p->flip_to_ack_samples++;
        pace_mean(&p->flip_to_ack_mean_ns, duration, p->flip_to_ack_samples);
        if (p->refresh_interval_ns && duration > p->refresh_interval_ns)
            p->frames_over_interval++;
    }
    if (submitted_at_ns && acked_at_ns >= submitted_at_ns) {
        const uint64_t lifetime = acked_at_ns - submitted_at_ns;
        if (lifetime > p->frame_lifetime_worst_ns)
            p->frame_lifetime_worst_ns = lifetime;
    }
}

void anland_pacing_note_dropped(anland_pacing *p)
{
    if (p)
        p->frames_dropped++;
}

int anland_pacing_format(const anland_pacing *p, char *buf, size_t size)
{
    if (!p || !buf || !size)
        return -1;
    const int n = snprintf(buf, size,
        "pacing submitted=%llu flipped=%llu acked=%llu dropped=%llu "
        "over_interval=%llu submit_to_flip_mean_us=%llu submit_to_flip_worst_us=%llu "
        "submit_to_flip_samples=%llu flip_to_ack_mean_us=%llu flip_to_ack_worst_us=%llu "
        "flip_to_ack_samples=%llu lifetime_worst_us=%llu interval_us=%llu",
        (unsigned long long)p->frames_submitted,
        (unsigned long long)p->frames_flipped,
        (unsigned long long)p->frames_acked,
        (unsigned long long)p->frames_dropped,
        (unsigned long long)p->frames_over_interval,
        (unsigned long long)(p->submit_to_flip_mean_ns / 1000ull),
        (unsigned long long)(p->submit_to_flip_worst_ns / 1000ull),
        (unsigned long long)p->submit_to_flip_samples,
        (unsigned long long)(p->flip_to_ack_mean_ns / 1000ull),
        (unsigned long long)(p->flip_to_ack_worst_ns / 1000ull),
        (unsigned long long)p->flip_to_ack_samples,
        (unsigned long long)(p->frame_lifetime_worst_ns / 1000ull),
        (unsigned long long)(p->refresh_interval_ns / 1000ull));
    return n < 0 || (size_t)n >= size ? -1 : n;
}
