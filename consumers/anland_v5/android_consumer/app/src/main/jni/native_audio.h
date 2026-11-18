#ifndef NATIVE_AUDIO_H
#define NATIVE_AUDIO_H

#include "display_consumer.h"

/*
 * Consumer-side audio bridge over the display library's audio socket.
 *
 *   - playback: reads desktop PCM datagrams from the socket and writes them to an
 *                AAudio output stream (the Android speaker);
 *   - capture : reads the Android mic via an AAudio input stream and writes PCM
 *                datagrams to the socket (the producer exposes them as a Linux
 *                recording source). Only active while the mic is enabled.
 *
 * The playback AAudio stream stays open and is driven by an AAudio callback. The
 * socket thread appends PCM into a small ring buffer; the callback outputs an
 * inaudible keepalive when Linux is idle. This keeps Android's mixer awake across
 * short UI sounds without reconnecting the display pipeline. The capture worker
 * opens the input stream while forwarding is enabled and a session is live.
 * The active display_ctx (and thus the live audio fd) is swapped via
 * audio_set_ctx(). The display library owns the audio fd.
 *
 * The bridge is per-instance: each consumer window owns its own audio_bridge (its
 * own AAudio playback + capture streams, connected to its own producer). Multiple
 * playback streams are mixed by AudioFlinger, so every window's desktop audio is
 * audible regardless of focus; the capture streams share the single physical mic.
 */
typedef struct audio_bridge audio_bridge;

/* Allocate a bridge (no streams open yet) / tear one down (implies audio_stop). */
audio_bridge *audio_create(void);
void audio_destroy(audio_bridge *b);

void audio_start(audio_bridge *b);
void audio_stop(audio_bridge *b);

/* Point the bridge at the current connection (its audio fd is fetched live via
 * get_audio_fd), or NULL to detach. Mirrors the event thread's use of s->ctx. */
void audio_set_ctx(audio_bridge *b, display_ctx *ctx);

/* Enable/disable microphone capture (the up path). Off by default; the Java layer
 * turns it on only once RECORD_AUDIO has been granted. */
void audio_set_mic_enabled(audio_bridge *b, int enabled);

/* Latency presets, separately for the speaker (playback) and microphone (capture)
 * paths, in milliseconds. 0 = let the engine pick the default quantum. The value is
 * converted to a frame count using the device's negotiated rate and forwarded to the
 * producer, which applies it as the PipeWire node.latency. Takes effect immediately
 * (the formats are re-announced on the live connection). */
void audio_set_latency(audio_bridge *b, int speaker_ms, int mic_ms);

/* Keep the AAudio output stream hot with a near-silent keepalive (bursty UI sounds
 * always play immediately), or let it idle-stop after silence to save standby power.
 * Default is disabled. Takes effect on the playback thread's next loop pass. */
void audio_set_keepalive(audio_bridge *b, int enabled);

#endif
