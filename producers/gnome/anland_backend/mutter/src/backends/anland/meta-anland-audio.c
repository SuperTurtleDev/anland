/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-anland-audio.h"

#include "backends/anland/common/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

#define DEFAULT_RATE          48000
#define DEFAULT_PLAY_CHANNELS 2
#define DEFAULT_CAP_CHANNELS  1
#define MIC_RING_BYTES        (48000 * 2 * (int) sizeof (int16_t))
#define MAX_DGRAM             (64 * 1024)
#define RECONNECT_SECS        1
#define MIN_AUDIO_RATE     8000
#define MAX_AUDIO_RATE   384000
#define MAX_AUDIO_CHANNELS     2
#define MAX_AUDIO_QUANTUM  65536
#define KEEPALIVE_FRAMES       480
#define KEEPALIVE_BYTES        (KEEPALIVE_FRAMES * DEFAULT_PLAY_CHANNELS * (int) sizeof (int16_t))

struct _MetaAnlandAudio
{
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;
  struct spa_source *reconnect_timer;
  bool pw_connected;

  struct pw_stream *capture;
  struct spa_hook capture_listener;
  struct pw_stream *source;
  struct spa_hook source_listener;

  uint32_t play_rate;
  uint32_t play_channels;
  uint32_t cap_rate;
  uint32_t cap_channels;
  uint32_t play_quantum;
  uint32_t cap_quantum;

  int audio_fd;
  struct spa_source *io;

  uint8_t *ring;
  size_t ring_size;
  size_t ring_head;
  size_t ring_tail;
  size_t ring_fill;

  uint8_t silence[KEEPALIVE_BYTES];

  uint8_t rx[MAX_DGRAM];
  bool loop_started;
};

static int connect_stream (struct pw_stream  *stream,
                           enum spa_direction  direction,
                           uint32_t            rate,
                           uint32_t            channels,
                           uint32_t            quantum);
static const struct spa_pod * build_format (struct spa_pod_builder *builder,
                                             uint32_t                rate,
                                             uint32_t                channels);
static void set_latency (struct pw_stream *stream,
                         uint32_t          quantum,
                         uint32_t          rate);

static void
ring_reset (MetaAnlandAudio *audio)
{
  audio->ring_head = audio->ring_tail = audio->ring_fill = 0;
}

static void
ring_write (MetaAnlandAudio *audio,
            const uint8_t   *data,
            size_t           size)
{
  size_t first;

  if (size > audio->ring_size)
    {
      data += size - audio->ring_size;
      size = audio->ring_size;
    }

  if (audio->ring_fill + size > audio->ring_size)
    {
      size_t drop = audio->ring_fill + size - audio->ring_size;

      audio->ring_tail = (audio->ring_tail + drop) % audio->ring_size;
      audio->ring_fill -= drop;
    }

  first = MIN (audio->ring_size - audio->ring_head, size);
  memcpy (audio->ring + audio->ring_head, data, first);
  memcpy (audio->ring, data + first, size - first);
  audio->ring_head = (audio->ring_head + size) % audio->ring_size;
  audio->ring_fill += size;
}

static size_t
ring_read (MetaAnlandAudio *audio,
           uint8_t         *data,
           size_t           size)
{
  size_t available = MIN (size, audio->ring_fill);
  size_t first = MIN (audio->ring_size - audio->ring_tail, available);

  memcpy (data, audio->ring + audio->ring_tail, first);
  memcpy (data + first, audio->ring, available - first);
  audio->ring_tail = (audio->ring_tail + available) % audio->ring_size;
  audio->ring_fill -= available;
  return available;
}

static void
on_capture_process (void *user_data)
{
  MetaAnlandAudio *audio = user_data;
  struct pw_buffer *buffer;
  struct spa_data *data;

  buffer = pw_stream_dequeue_buffer (audio->capture);
  if (!buffer)
    return;

  data = &buffer->buffer->datas[0];
  if (audio->audio_fd >= 0)
    {
      const uint8_t *payload = audio->silence;
      size_t payload_size = sizeof (audio->silence);

      if (data->data && data->chunk && data->chunk->size > 0)
        {
          payload = (uint8_t *) data->data + data->chunk->offset;
          payload_size = data->chunk->size;
        }

      struct audio_msg header = {
        .type = AUDIO_MSG_PCM,
        .size = payload_size,
      };
      struct iovec iov[2] = {
        { .iov_base = &header, .iov_len = sizeof (header) },
        { .iov_base = (void *) payload, .iov_len = payload_size },
      };
      struct msghdr message = {
        .msg_iov = iov,
        .msg_iovlen = G_N_ELEMENTS (iov),
      };

      /* The PipeWire loop must not stall on a slow or disconnected consumer. */
      sendmsg (audio->audio_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

  pw_stream_queue_buffer (audio->capture, buffer);
}

static void
on_source_process (void *user_data)
{
  MetaAnlandAudio *audio = user_data;
  struct pw_buffer *buffer;
  struct spa_data *data;
  uint32_t stride;
  uint32_t frames;
  uint32_t size;
  size_t available;

  buffer = pw_stream_dequeue_buffer (audio->source);
  if (!buffer)
    return;

  data = &buffer->buffer->datas[0];
  if (!data->data || !data->chunk)
    {
      pw_stream_queue_buffer (audio->source, buffer);
      return;
    }

  stride = sizeof (int16_t) * audio->cap_channels;
  frames = data->maxsize / stride;
  if (buffer->requested && buffer->requested < frames)
    frames = buffer->requested;
  size = frames * stride;

  available = ring_read (audio, data->data, size);
  if (available < size)
    memset ((uint8_t *) data->data + available, 0, size - available);

  data->chunk->offset = 0;
  data->chunk->stride = stride;
  data->chunk->size = size;
  pw_stream_queue_buffer (audio->source, buffer);
}

static const struct pw_stream_events capture_events = {
  PW_VERSION_STREAM_EVENTS,
  .process = on_capture_process,
};

static const struct pw_stream_events source_events = {
  PW_VERSION_STREAM_EVENTS,
  .process = on_source_process,
};

static void
apply_format (MetaAnlandAudio          *audio,
              const struct audio_format *format)
{
  gboolean playback = format->role == AUDIO_ROLE_PLAYBACK;
  uint32_t rate = format->rate ? format->rate : DEFAULT_RATE;
  uint32_t channels = format->channels ? format->channels :
    (playback ? DEFAULT_PLAY_CHANNELS : DEFAULT_CAP_CHANNELS);
  uint32_t *current_rate = playback ? &audio->play_rate : &audio->cap_rate;
  uint32_t *current_channels = playback ? &audio->play_channels :
    &audio->cap_channels;
  uint32_t *current_quantum = playback ? &audio->play_quantum :
    &audio->cap_quantum;
  struct pw_stream *stream = playback ? audio->capture : audio->source;
  gboolean format_changed;
  gboolean quantum_changed;

  format_changed = rate != *current_rate || channels != *current_channels;
  quantum_changed = format->quantum != *current_quantum;
  if (!format_changed && !quantum_changed)
    return;

  *current_rate = rate;
  *current_channels = channels;
  *current_quantum = format->quantum;

  if (!audio->pw_connected || !stream)
    return;

  if (format_changed)
    {
      uint8_t buffer[1024];
      struct spa_pod_builder builder = SPA_POD_BUILDER_INIT (buffer,
                                                              sizeof (buffer));
      const struct spa_pod *params[1] = {
        build_format (&builder, rate, channels),
      };

      set_latency (stream, format->quantum, rate);
      pw_stream_update_params (stream, params, G_N_ELEMENTS (params));
    }
  else if (format->quantum > 0)
    {
      set_latency (stream, format->quantum, rate);
    }
}

static bool
valid_format (const struct audio_format *format)
{
  return (format->role == AUDIO_ROLE_PLAYBACK ||
          format->role == AUDIO_ROLE_CAPTURE) &&
         format->format == AUDIO_FORMAT_S16LE &&
         format->rate >= MIN_AUDIO_RATE &&
         format->rate <= MAX_AUDIO_RATE &&
         format->channels > 0 &&
         format->channels <= MAX_AUDIO_CHANNELS &&
         format->quantum <= MAX_AUDIO_QUANTUM;
}

static void
on_audio_readable (void     *user_data,
                   int       fd,
                   uint32_t  mask)
{
  MetaAnlandAudio *audio = user_data;

  if (mask & (SPA_IO_ERR | SPA_IO_HUP) || !(mask & SPA_IO_IN))
    return;

  for (;;)
    {
      struct audio_msg header;
      size_t available;
      ssize_t received = recv (fd, audio->rx, sizeof (audio->rx),
                               MSG_DONTWAIT | MSG_TRUNC);

      if (received <= 0)
        break;
      if ((size_t) received > sizeof (audio->rx) ||
          (size_t) received < sizeof (header))
        continue;

      memcpy (&header, audio->rx, sizeof (header));
      available = (size_t) received - sizeof (header);
      if (header.type == AUDIO_MSG_FORMAT)
        {
          struct audio_format format;

          if (header.size != sizeof (format) || header.size != available)
            continue;

          memcpy (&format, audio->rx + sizeof (header), sizeof (format));
          if (valid_format (&format))
            apply_format (audio, &format);
          continue;
        }

      if (header.type != AUDIO_MSG_PCM || header.size != available ||
          header.size % (sizeof (int16_t) * audio->cap_channels) != 0)
        continue;

      ring_write (audio, audio->rx + sizeof (header), header.size);
    }
}

static void
arm_reconnect (MetaAnlandAudio *audio)
{
  struct timespec interval = { .tv_sec = RECONNECT_SECS, .tv_nsec = 0 };

  pw_loop_update_timer (pw_thread_loop_get_loop (audio->loop),
                        audio->reconnect_timer, &interval, NULL, false);
}

static void
on_core_error (void        *user_data,
               uint32_t     id,
               int          seq,
               int          result,
               const char  *message)
{
  MetaAnlandAudio *audio = user_data;

  (void) seq;
  (void) message;

  if (id != PW_ID_CORE || result != -EPIPE)
    return;

  audio->pw_connected = false;
  arm_reconnect (audio);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .error = on_core_error,
};

static const struct spa_pod *
build_format (struct spa_pod_builder *builder,
              uint32_t                rate,
              uint32_t                channels)
{
  struct spa_audio_info_raw info = {
    .format = SPA_AUDIO_FORMAT_S16_LE,
    .rate = rate,
    .channels = channels,
  };

  if (channels >= 2)
    {
      info.position[0] = SPA_AUDIO_CHANNEL_FL;
      info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
  else
    {
      info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }

  return spa_format_audio_raw_build (builder, SPA_PARAM_EnumFormat, &info);
}

static void
set_latency (struct pw_stream *stream,
             uint32_t          quantum,
             uint32_t          rate)
{
  char latency[32];
  struct spa_dict_item items[] = {
    SPA_DICT_ITEM_INIT (PW_KEY_NODE_LATENCY, latency),
  };
  struct spa_dict properties = SPA_DICT_INIT (items, G_N_ELEMENTS (items));

  if (quantum == 0)
    return;

  g_snprintf (latency, sizeof (latency), "%u/%u", quantum, rate);
  pw_stream_update_properties (stream, &properties);
}

static int
connect_stream (struct pw_stream  *stream,
                enum spa_direction  direction,
                uint32_t            rate,
                uint32_t            channels,
                uint32_t            quantum)
{
  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT (buffer,
                                                          sizeof (buffer));
  const struct spa_pod *params[1] = {
    build_format (&builder, rate, channels),
  };

  set_latency (stream, quantum, rate);
  return pw_stream_connect (stream, direction, PW_ID_ANY,
                            PW_STREAM_FLAG_AUTOCONNECT |
                            PW_STREAM_FLAG_MAP_BUFFERS |
                            PW_STREAM_FLAG_RT_PROCESS,
                            params, G_N_ELEMENTS (params));
}

static void
teardown_pw (MetaAnlandAudio *audio)
{
  if (audio->capture)
    {
      spa_hook_remove (&audio->capture_listener);
      pw_stream_destroy (audio->capture);
      audio->capture = NULL;
    }

  if (audio->source)
    {
      spa_hook_remove (&audio->source_listener);
      pw_stream_destroy (audio->source);
      audio->source = NULL;
    }

  if (audio->core)
    {
      spa_hook_remove (&audio->core_listener);
      pw_core_disconnect (audio->core);
      audio->core = NULL;
    }
}

static int
build_pw (MetaAnlandAudio *audio)
{
  audio->core = pw_context_connect (audio->context, NULL, 0);
  if (!audio->core)
    return -1;

  pw_core_add_listener (audio->core, &audio->core_listener, &core_events, audio);

  audio->capture = pw_stream_new (
    audio->core, "anland-speaker",
    pw_properties_new (PW_KEY_MEDIA_TYPE, "Audio",
                       PW_KEY_MEDIA_CLASS, "Audio/Sink",
                       PW_KEY_NODE_NAME, "anland-speaker",
                       PW_KEY_NODE_DESCRIPTION, "Anland remote speaker",
                       PW_KEY_PRIORITY_SESSION, "1010",
                       PW_KEY_PRIORITY_DRIVER, "1010",
                       PW_KEY_NODE_ALWAYS_PROCESS, "true",
                       PW_KEY_NODE_PAUSE_ON_IDLE, "false",
                       PW_KEY_NODE_SUSPEND_ON_IDLE, "false",
                       "session.suspend-timeout-seconds", "0",
                       NULL));
  if (!audio->capture)
    return -1;

  pw_stream_add_listener (audio->capture, &audio->capture_listener,
                          &capture_events, audio);

  audio->source = pw_stream_new (
    audio->core, "anland-mic",
    pw_properties_new (PW_KEY_MEDIA_TYPE, "Audio",
                       PW_KEY_MEDIA_CLASS, "Audio/Source",
                       PW_KEY_NODE_NAME, "anland-mic",
                       PW_KEY_NODE_DESCRIPTION, "Anland remote microphone",
                       PW_KEY_PRIORITY_SESSION, "1010",
                       PW_KEY_PRIORITY_DRIVER, "1010",
                       NULL));
  if (!audio->source)
    return -1;

  pw_stream_add_listener (audio->source, &audio->source_listener,
                          &source_events, audio);

  if (connect_stream (audio->capture, PW_DIRECTION_INPUT, audio->play_rate,
                      audio->play_channels, audio->play_quantum) < 0 ||
      connect_stream (audio->source, PW_DIRECTION_OUTPUT, audio->cap_rate,
                      audio->cap_channels, audio->cap_quantum) < 0)
    return -1;

  return 0;
}

static void
on_reconnect_timer (void     *user_data,
                    uint64_t  expirations)
{
  MetaAnlandAudio *audio = user_data;

  (void) expirations;

  if (audio->pw_connected)
    return;

  teardown_pw (audio);
  if (build_pw (audio) == 0)
    {
      audio->pw_connected = true;
    }
  else
    {
      teardown_pw (audio);
      arm_reconnect (audio);
    }
}

void
meta_anland_audio_set_fd (MetaAnlandAudio *audio,
                          int              fd)
{
  struct pw_loop *loop;
  int owned_fd = -1;

  if (!audio)
    return;

  if (fd >= 0)
    {
      owned_fd = fcntl (fd, F_DUPFD_CLOEXEC, 0);
      if (owned_fd < 0)
        g_warning ("Anland failed to duplicate audio fd: %s", strerror (errno));
    }

  pw_thread_loop_lock (audio->loop);

  loop = pw_thread_loop_get_loop (audio->loop);
  if (audio->io)
    {
      pw_loop_destroy_source (loop, audio->io);
      audio->io = NULL;
    }

  if (audio->audio_fd >= 0)
    close (audio->audio_fd);
  audio->audio_fd = owned_fd;
  ring_reset (audio);

  if (owned_fd >= 0)
    {
      audio->io = pw_loop_add_io (loop, owned_fd, SPA_IO_IN, false,
                                  on_audio_readable, audio);
      if (!audio->io)
        {
          close (audio->audio_fd);
          audio->audio_fd = -1;
        }
    }

  pw_thread_loop_unlock (audio->loop);
}

MetaAnlandAudio *
meta_anland_audio_new (void)
{
  MetaAnlandAudio *audio;

  pw_init (NULL, NULL);

  audio = g_new0 (MetaAnlandAudio, 1);
  if (!audio)
    goto fail_init;

  audio->audio_fd = -1;
  audio->play_rate = DEFAULT_RATE;
  audio->play_channels = DEFAULT_PLAY_CHANNELS;
  audio->cap_rate = DEFAULT_RATE;
  audio->cap_channels = DEFAULT_CAP_CHANNELS;
  audio->ring_size = MIC_RING_BYTES;
  audio->ring = g_malloc (audio->ring_size);
  if (!audio->ring)
    goto fail;

  audio->loop = pw_thread_loop_new ("anland-audio", NULL);
  if (!audio->loop)
    goto fail;

  audio->context = pw_context_new (pw_thread_loop_get_loop (audio->loop),
                                   NULL, 0);
  if (!audio->context)
    goto fail;

  audio->reconnect_timer = pw_loop_add_timer (
    pw_thread_loop_get_loop (audio->loop), on_reconnect_timer, audio);
  if (!audio->reconnect_timer)
    goto fail;

  if (pw_thread_loop_start (audio->loop) < 0)
    goto fail;
  audio->loop_started = true;

  pw_thread_loop_lock (audio->loop);
  if (build_pw (audio) == 0)
    {
      audio->pw_connected = true;
    }
  else
    {
      teardown_pw (audio);
      arm_reconnect (audio);
    }
  pw_thread_loop_unlock (audio->loop);

  return audio;

fail:
  if (audio->loop_started)
    pw_thread_loop_stop (audio->loop);
  if (audio->reconnect_timer)
    pw_loop_destroy_source (pw_thread_loop_get_loop (audio->loop),
                            audio->reconnect_timer);
  if (audio->context)
    pw_context_destroy (audio->context);
  if (audio->loop)
    pw_thread_loop_destroy (audio->loop);
  g_free (audio->ring);
  g_free (audio);
fail_init:
  return NULL;
}

void
meta_anland_audio_free (MetaAnlandAudio *audio)
{
  if (!audio)
    return;

  if (audio->loop_started)
    pw_thread_loop_stop (audio->loop);
  teardown_pw (audio);
  if (audio->io)
    pw_loop_destroy_source (pw_thread_loop_get_loop (audio->loop), audio->io);
  if (audio->reconnect_timer)
    pw_loop_destroy_source (pw_thread_loop_get_loop (audio->loop),
                            audio->reconnect_timer);
  if (audio->audio_fd >= 0)
    close (audio->audio_fd);
  if (audio->context)
    pw_context_destroy (audio->context);
  if (audio->loop)
    pw_thread_loop_destroy (audio->loop);
  g_free (audio->ring);
  g_free (audio);
}