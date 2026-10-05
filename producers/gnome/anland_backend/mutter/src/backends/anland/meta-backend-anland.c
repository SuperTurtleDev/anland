/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-backend-anland.h"

#include "backends/anland/meta-anland-audio.h"
#include "backends/anland/meta-anland-camera.h"
#include "backends/anland/meta-anland-clipboard.h"
#include "backends/anland/meta-anland-input.h"
#include "backends/anland/libdisplay_producer/anland_de_backend.h"
#include "backends/anland/libdisplay_producer/anland_device.h"
#include "backends/meta-backend-private.h"
#include "backends/meta-fd-source.h"
#include "backends/meta-monitor-manager-private.h"
#include "backends/meta-renderer.h"
#include "backends/meta-renderer-view.h"
#include "backends/meta-stage-view-private.h"
#include "backends/meta-virtual-monitor.h"
#include "backends/native/meta-renderer-native-private.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/meta-wayland-compositor.h"
#include "wayland/meta-wayland.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANLAND_RECONNECT_INTERVAL_MS 200
/* Protocol pixel-format convention shared by the daemon and every consumer
 * (see anland_device.h: 1 = ABGR8888). */
#define ANLAND_FORMAT_RGBA_8888 1
#define ANLAND_MAX_RESOURCE_FDS 64

enum
{
  PROP_0,

  PROP_ANLAND_SOCKET,
};

struct _MetaBackendAnland
{
  MetaBackendNative parent;

  char *socket_path;
  /* The shared presentation facade. It owns the scene, the device and the
   * presentation backend, so every frame transaction goes through the same
   * contract KWin uses; this backend keeps only the Cogl/Clutter translation. */
  anland_de_backend *present_backend;
  /* Borrowed from present_backend (anland_de_backend_device()). Input, clipboard,
   * audio and camera resources still go through it directly. */
  anland_device *device;
  /* Legacy output layer: the flattened full-output target the legacy
   * presentation backend presents. Layer identity is the DE's. */
  uint64_t output_layer;
  /* Commit handed to the presentation backend, awaiting PRESENTED/DROPPED. */
  uint64_t pending_commit_id;
  MetaVirtualMonitor *virtual_monitor;
  /* Connector state as reported by the shared device layer. */
  anland_device_output_t output;
  guint reconnect_source_id;
  MetaAnlandAudio *audio;
  MetaAnlandCamera *camera;
  MetaAnlandInput *input;
  MetaAnlandClipboard *clipboard;

  GSource *buffer_ready_source;
  GSource *input_source;
  MetaStageView *stage_view;
  CoglFramebuffer *placeholder_framebuffer;
  CoglFramebuffer *buffer_framebuffers[ANLAND_DEVICE_MAX_BUFS];
  int current_buffer;
  /* Generation of the consumer session this backend is serving, and the number of
   * buffers imported for it. A render target is only usable when it belongs to
   * this generation: a target published by a previous session names a slot in a
   * buffer set this backend no longer holds. */
  uint64_t session_generation;
  uint32_t buffer_count;
  gboolean consumer_active;
  gboolean frame_clock_inhibited;
  gboolean frame_pending;
  gboolean warned_sync_fallback;
  int64_t pending_global_frame_counter;
  int64_t pending_view_frame_counter;
  unsigned int presentation_sequence;
};

G_DEFINE_TYPE (MetaBackendAnland, meta_backend_anland, META_TYPE_BACKEND_NATIVE)

static void deactivate_consumer (MetaBackendAnland *backend);
static void invalidate_consumer (MetaBackendAnland *backend);
static void dispatch_present_events (MetaBackendAnland *backend);
static void release_stage_view (MetaBackendAnland *backend);
static void schedule_reconnect (MetaBackendAnland *backend);

static void
on_context_started (MetaContext        *context,
                    MetaBackendAnland *backend)
{
  backend->clipboard = meta_anland_clipboard_new (META_BACKEND (backend));
  if (!backend->clipboard)
    g_warning ("Failed to initialize Anland clipboard bridge");
}

static gboolean
read_text_payload (MetaBackendAnland  *backend,
                   uint32_t            size,
                   const char         *name,
                   GBytes            **contents)
{
  g_autofree char *text = NULL;

  *contents = NULL;
  if (size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE)
    {
      g_warning ("Anland rejected oversized %s payload", name);
      return FALSE;
    }

  text = g_malloc (size + 1);
  if (anland_device_read_input (backend->device, text, size, 100) != 1)
    return FALSE;
  text[size] = '\0';

  if (memchr (text, '\0', size) || !g_utf8_validate (text, size, NULL))
    {
      g_warning ("Anland ignored invalid UTF-8 %s payload", name);
      return TRUE;
    }

  *contents = g_bytes_new (text, size);
  return TRUE;
}

static gboolean
handle_extended_input (MetaBackendAnland       *backend,
                       const anland_device_input_t *event)
{
  g_autoptr (GBytes) contents = NULL;
  g_autofree char *input_text = NULL;
  const char *text;
  guint size;

  switch (event->type)
    {
    case ANLAND_DEVICE_IN_CLIPBOARD:
      if (!read_text_payload (backend, event->clipboard.size, "clipboard",
                              &contents))
        return FALSE;
      break;
    case ANLAND_DEVICE_IN_TEXT_INPUT:
      if (!read_text_payload (backend, event->text_input.size, "text input",
                              &contents))
        return FALSE;
      break;
    default:
      return FALSE;
    }

  if (!contents)
    return TRUE;

  size = g_bytes_get_size (contents);
  if (size == 0)
    return TRUE;

  text = g_bytes_get_data (contents, NULL);
  if (event->type == ANLAND_DEVICE_IN_CLIPBOARD)
    return meta_anland_clipboard_set_from_consumer (backend->clipboard,
                                                    contents);

  input_text = g_strndup (text, size);

  {
    MetaContext *context = meta_backend_get_context (META_BACKEND (backend));
    MetaWaylandCompositor *compositor =
      meta_context_get_wayland_compositor (context);

    if (compositor && meta_wayland_text_input_commit_string (
          meta_wayland_compositor_get_text_input (compositor), input_text))
      return TRUE;
  }

  return meta_anland_input_inject_text (backend->input, input_text);
}

static void
close_resource_fds (int *fds,
                    int  n_fds)
{
  int i;

  for (i = 0; i < n_fds; i++)
    {
      if (fds[i] >= 0)
        close (fds[i]);
    }
}

static gboolean
set_resource_fds_cloexec (int *fds,
                          int  n_fds)
{
  int i;

  for (i = 0; i < n_fds; i++)
    {
      int flags = fcntl (fds[i], F_GETFD);

      if (flags < 0 || fcntl (fds[i], F_SETFD, flags | FD_CLOEXEC) < 0)
        return FALSE;
    }

  return TRUE;
}

static gboolean
handle_resource_input (MetaBackendAnland       *backend,
                       const anland_device_input_t *event)
{
  int fds[ANLAND_MAX_RESOURCE_FDS];
  int n_fds = 0;

  if (event->resource.fdnum == 0 ||
      event->resource.fdnum > G_N_ELEMENTS (fds) ||
      anland_device_read_fds (backend->device, fds, G_N_ELEMENTS (fds),
                              &n_fds, 100) != 1 ||
      n_fds != (int) event->resource.fdnum)
    {
      close_resource_fds (fds, n_fds);
      return FALSE;
    }

  if (!set_resource_fds_cloexec (fds, n_fds))
    {
      close_resource_fds (fds, n_fds);
      return FALSE;
    }

  if (event->resource.type == ANLAND_DEVICE_SERVICE_CAMERA && backend->camera)
    {
      meta_anland_camera_set_resources (backend->camera, fds[0], &fds[1],
                                        n_fds - 1);
      return TRUE;
    }

  close_resource_fds (fds, n_fds);
  return TRUE;
}

static void
request_camera_resources (MetaBackendAnland *backend)
{
  if (!backend->camera)
    return;

  anland_device_request_resources (backend->device,
                                   ANLAND_DEVICE_SERVICE_CAMERA, NULL);
}

static char *
get_default_socket_path (void)
{
  const char *socket_path = g_getenv ("ANLAND_SOCKET");
  const char *tmpdir;
  const char *prefix;

  if (socket_path && *socket_path)
    return g_strdup (socket_path);

  tmpdir = g_getenv ("TMPDIR");
  if (tmpdir && *tmpdir)
    return g_build_filename (tmpdir, "anland", "display_daemon.sock", NULL);

  prefix = g_getenv ("PREFIX");
  if (prefix && g_str_has_prefix (prefix, "/data/data/com.termux/"))
    return g_strdup ("/data/data/com.termux/files/usr/tmp/anland/display_daemon.sock");

  return g_strdup ("/tmp/anland/display_daemon.sock");
}

/* The connector state now arrives through the shared device layer, which already
 * rejects a malformed handshake. Keep the range checks: a consumer may legally
 * report a refresh rate this Compositor cannot drive, and passing that straight
 * into the virtual monitor would make Mutter create an unusable mode. */
static gboolean
validate_output (const anland_device_output_t  *output,
                 GError                       **error)
{
  if (output->width == 0 || output->height == 0 ||
      output->width > G_MAXINT || output->height > G_MAXINT ||
      (output->refresh_mhz != 0 && output->refresh_mhz < 1000) ||
      output->refresh_mhz > 1000000)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Anland daemon provided invalid screen information");
      return FALSE;
    }

  return TRUE;
}

/* Refresh the cached connector state from the device layer. */
static gboolean
refresh_output (MetaBackendAnland  *backend,
                GError            **error)
{
  if (anland_device_get_outputs (backend->device, &backend->output, 1) != 1)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Anland daemon provided no output information");
      return FALSE;
    }

  return validate_output (&backend->output, error);
}

static void
clear_device (MetaBackendAnland *backend)
{
  deactivate_consumer (backend);

  /* present_backend owns the scene and the device; DE code only borrowed them. */
  if (backend->present_backend)
    anland_de_backend_destroy (backend->present_backend);
  backend->present_backend = NULL;
  backend->device = NULL;
  backend->output_layer = 0;
  backend->pending_commit_id = 0;
}

static gboolean
connect_device (MetaBackendAnland  *backend,
                GError            **error)
{
  anland_de_backend_config_t config;
  anland_layer_desc_t layer_desc;

  if (backend->present_backend)
    return TRUE;

  /* The socket path is only a hint: the shared device layer also probes
   * $ANLAND_SOCKET and the well-known locations a container or desktop session
   * mounts the daemon on, and records which one answered. */
  config = (anland_de_backend_config_t) {
    .present = { .endpoint = backend->socket_path },
    .name = "mutter",
  };

  backend->present_backend = anland_de_backend_create (&config);
  if (!backend->present_backend)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                     "Failed to connect to Anland daemon at %s",
                     backend->socket_path);
      return FALSE;
    }

  backend->device = anland_de_backend_device (backend->present_backend);
  if (!backend->device)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Anland presentation backend exposed no device");
      clear_device (backend);
      return FALSE;
    }

  /* One layer represents this output. The legacy presentation flattens the scene,
   * so Mutter keeps compositing all windows into the output buffer; the id is a
   * DE-side handle, not a claim about the number of real windows. */
  layer_desc = (anland_layer_desc_t) {
    .parent_id = 0,
    .kind = ANLAND_LAYER_NORMAL,
    .name = "mutter-output",
    .opacity = 1.0f,
    .visible = TRUE,
  };

  if (anland_de_backend_add_window_desc (backend->present_backend, 1,
                                         &layer_desc,
                                         &backend->output_layer) != 0)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to create the Anland output layer");
      clear_device (backend);
      return FALSE;
    }

  if (!refresh_output (backend, error))
    {
      clear_device (backend);
      return FALSE;
    }

  return TRUE;
}

static gboolean
update_virtual_monitor (MetaBackendAnland  *backend,
                        GError            **error)
{
  MetaMonitorManager *monitor_manager;
  float refresh_rate;

  if (!backend->device)
    return FALSE;

  monitor_manager = meta_backend_get_monitor_manager (META_BACKEND (backend));
  refresh_rate = backend->output.refresh_mhz > 0 ?
    backend->output.refresh_mhz / 1000.0f : 60.0f;

  if (backend->virtual_monitor)
    {
      release_stage_view (backend);
#ifdef ANLAND_MUTTER_50
      {
        GList *mode_infos = NULL;
        mode_infos = g_list_append (mode_infos,
                                    meta_virtual_mode_info_new (
                                      backend->output.width,
                                      backend->output.height,
                                      refresh_rate));
        meta_virtual_monitor_set_modes (backend->virtual_monitor, mode_infos);
        g_list_free_full (mode_infos,
                          (GDestroyNotify) meta_virtual_mode_info_free);
      }
#else
      meta_virtual_monitor_set_mode (backend->virtual_monitor,
                                     backend->output.width,
                                     backend->output.height,
                                     refresh_rate);
#endif
      meta_monitor_manager_reload (monitor_manager);
      return TRUE;
    }

#ifdef ANLAND_MUTTER_50
  g_autoptr (MetaVirtualMonitorInfo) info =
    meta_virtual_monitor_info_new_simple (backend->output.width,
                                          backend->output.height,
                                          refresh_rate,
                                          "Anland", "Anland-1", "Anland-1");
#else
  g_autoptr (MetaVirtualMonitorInfo) info =
    meta_virtual_monitor_info_new (backend->output.width,
                                   backend->output.height,
                                   refresh_rate,
                                   "Anland", "Anland-1", "Anland-1");
#endif
  backend->virtual_monitor =
    meta_monitor_manager_create_virtual_monitor (monitor_manager, info, error);
  if (!backend->virtual_monitor)
    return FALSE;

  meta_monitor_manager_reload (monitor_manager);
  return TRUE;
}

static void
inhibit_frame_clock (MetaBackendAnland *backend)
{
  if (!backend->stage_view || backend->frame_clock_inhibited)
    return;

  clutter_frame_clock_inhibit (
    clutter_stage_view_get_frame_clock (CLUTTER_STAGE_VIEW (backend->stage_view)));
  backend->frame_clock_inhibited = TRUE;
}

static void
uninhibit_frame_clock (MetaBackendAnland *backend)
{
  if (!backend->stage_view || !backend->frame_clock_inhibited)
    return;

  clutter_frame_clock_uninhibit (
    clutter_stage_view_get_frame_clock (CLUTTER_STAGE_VIEW (backend->stage_view)));
  backend->frame_clock_inhibited = FALSE;
}

static void
release_stage_view (MetaBackendAnland *backend)
{
  if (!backend->stage_view)
    return;

  g_return_if_fail (!backend->consumer_active);
  g_return_if_fail (!backend->frame_pending);

  uninhibit_frame_clock (backend);
  g_clear_object (&backend->placeholder_framebuffer);
  g_clear_object (&backend->stage_view);
}

static MetaStageView *
find_stage_view (MetaBackendAnland *backend)
{
  MetaRenderer *renderer;
  MetaCrtc *crtc;
  MetaRendererView *renderer_view;

  if (!backend->virtual_monitor)
    return NULL;

  renderer = meta_backend_get_renderer (META_BACKEND (backend));
  crtc = meta_virtual_monitor_get_crtc (backend->virtual_monitor);
  renderer_view = meta_renderer_get_view_for_crtc (renderer, crtc);

  return renderer_view ? META_STAGE_VIEW (renderer_view) : NULL;
}

static gboolean
ensure_stage_view (MetaBackendAnland  *backend,
                   GError            **error)
{
  MetaStageView *stage_view = find_stage_view (backend);

  if (!stage_view)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Anland virtual monitor does not have a renderer view yet");
      return FALSE;
    }

  if (backend->stage_view == stage_view)
    return TRUE;

  if (backend->consumer_active || backend->frame_pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "Anland renderer view changed while a consumer was active");
      return FALSE;
    }

  if (backend->frame_clock_inhibited)
    uninhibit_frame_clock (backend);

  g_set_object (&backend->stage_view, stage_view);
  g_set_object (&backend->placeholder_framebuffer,
                clutter_stage_view_get_onscreen (CLUTTER_STAGE_VIEW (stage_view)));
  return TRUE;
}

static gboolean
validate_buffer_info (const anland_device_fb_t  *fb,
                      GError                   **error)
{
  struct stat stat_buf;
  uint64_t required_size;

  if (fb->format != ANLAND_FORMAT_RGBA_8888 ||
      fb->modifier != DRM_FORMAT_MOD_LINEAR ||
      fb->width > G_MAXUINT32 / 4u ||
      fb->stride < fb->width * 4u ||
      fb->height > G_MAXUINT64 / fb->stride ||
      fb->offset > G_MAXUINT64 -
        (uint64_t) fb->stride * fb->height)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Anland daemon provided an invalid dma-buf description");
      return FALSE;
    }

  required_size = fb->offset + (uint64_t) fb->stride * fb->height;
  if (fstat (fb->fd, &stat_buf) == 0 && stat_buf.st_size > 0 &&
      required_size > (uint64_t) stat_buf.st_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Anland dma-buf is smaller than its advertised layout");
      return FALSE;
    }

  return TRUE;
}

static void
complete_pending_frame (MetaBackendAnland *backend)
{
  ClutterFrameInfo frame_info;

  if (!backend->stage_view || !backend->frame_pending)
    return;

  frame_info = (ClutterFrameInfo) {
    .global_frame_counter = backend->pending_global_frame_counter,
    .view_frame_counter = backend->pending_view_frame_counter,
    .refresh_rate = clutter_stage_view_get_refresh_rate (
      CLUTTER_STAGE_VIEW (backend->stage_view)),
    .presentation_time = g_get_monotonic_time (),
    .flags = CLUTTER_FRAME_INFO_FLAG_NONE,
    .sequence = ++backend->presentation_sequence,
  };
  clutter_stage_view_notify_presented (CLUTTER_STAGE_VIEW (backend->stage_view),
                                       &frame_info);
  backend->frame_pending = FALSE;
}

static void
deactivate_consumer (MetaBackendAnland *backend)
{
  meta_anland_audio_set_fd (backend->audio, -1);
  meta_anland_camera_clear (backend->camera);
  meta_anland_clipboard_set_device (backend->clipboard, NULL);

  if (backend->input_source)
    {
      g_source_destroy (backend->input_source);
      g_clear_pointer (&backend->input_source, g_source_unref);
    }

  if (backend->input)
    meta_anland_input_reset (backend->input);

  if (backend->buffer_ready_source)
    {
      g_source_destroy (backend->buffer_ready_source);
      g_clear_pointer (&backend->buffer_ready_source, g_source_unref);
    }

  if (backend->stage_view)
    {
      clutter_stage_view_set_present_func (
        CLUTTER_STAGE_VIEW (backend->stage_view), NULL, NULL);

      if (backend->current_buffer >= 0 &&
          backend->placeholder_framebuffer &&
          clutter_stage_view_get_onscreen (CLUTTER_STAGE_VIEW (backend->stage_view)) ==
            backend->buffer_framebuffers[backend->current_buffer])
        clutter_stage_view_replace_framebuffer (
          CLUTTER_STAGE_VIEW (backend->stage_view),
          backend->placeholder_framebuffer);

      if (backend->frame_pending)
        clutter_stage_view_notify_ready (CLUTTER_STAGE_VIEW (backend->stage_view));
    }

  for (int i = 0; i < ANLAND_DEVICE_MAX_BUFS; i++)
    g_clear_object (&backend->buffer_framebuffers[i]);

  backend->consumer_active = FALSE;
  backend->current_buffer = -1;
  /* No render target is usable once the session is gone: its slot index belongs
   * to a buffer set this backend has just dropped. */
  backend->buffer_count = 0;
  backend->frame_pending = FALSE;
  /* No commit can complete once the session is gone: forget it here so a late
   * PRESENTED/DROPPED from the old session cannot end a future frame. */
  backend->pending_commit_id = 0;
  inhibit_frame_clock (backend);
}

/* Tear the consumer session down because THIS backend cannot use it (bad
 * payload, unusable resource fds, malformed buffer index, failed import).
 *
 * The shared layer's drop_session() is deliberate about NOT firing the transport
 * fallback callback — the caller is the one dropping the session — so the local
 * cleanup and the reconnect tick have to be issued here. Without this the backend
 * would keep a deactivated session with no timer running and the compositor would
 * stop presenting for good. */
static void
invalidate_consumer (MetaBackendAnland *backend)
{
  deactivate_consumer (backend);

  if (backend->present_backend)
    anland_de_backend_drop_session (backend->present_backend);

  /* Retire the work that session was carrying (COMMIT_DROPPED + BUFFER_RELEASED)
   * so no stale commit or release fence survives into the next session. */
  dispatch_present_events (backend);

  schedule_reconnect (backend);
}

/* Adopt the buffer slot the consumer published: that is the render target the
 * next frame must draw into. The shared layer owns buffer selection, so this
 * reads the target from it instead of decoding the event, and rejects a target
 * that belongs to a session this backend is not serving. */
static void
apply_render_target (MetaBackendAnland *backend)
{
  anland_de_target_t target;
  uint32_t index;

  if (!backend->consumer_active || !backend->stage_view)
    return;

  if (anland_de_backend_get_target (backend->present_backend, &target) != 0)
    return;

  if (target.generation != backend->session_generation)
    {
      /* A target from a previous session names a slot in a buffer set this
       * backend no longer holds: acting on it would present a buffer it never
       * imported. */
      g_warning ("Anland ignored a render target from session %" G_GUINT64_FORMAT,
                 target.generation);
      return;
    }

  index = target.index;
  if (index >= target.count || index >= backend->buffer_count ||
      index >= ANLAND_DEVICE_MAX_BUFS || !backend->buffer_framebuffers[index])
    {
      g_warning ("Anland published render target %u without an imported buffer",
                 index);
      invalidate_consumer (backend);
      return;
    }

  if (backend->current_buffer != (int) index)
    {
      clutter_stage_view_replace_framebuffer (
        CLUTTER_STAGE_VIEW (backend->stage_view),
        backend->buffer_framebuffers[index]);
      backend->current_buffer = (int) index;
    }

  clutter_stage_view_add_redraw_clip (CLUTTER_STAGE_VIEW (backend->stage_view),
                                      NULL);
  uninhibit_frame_clock (backend);
  clutter_stage_view_schedule_update_now (CLUTTER_STAGE_VIEW (backend->stage_view));
}

static void
apply_output_change (MetaBackendAnland       *backend,
                     const anland_scene_event_t *event)
{
  g_autoptr (GError) error = NULL;

  if (event->u.output.width == 0 || event->u.output.height == 0)
    return;

  /* The shared layer republishes output state on every new session. The initial
   * notification can be dispatched after activate_consumer() imported buffers;
   * identical state must not tear down that newly activated renderer view. */
  if (backend->virtual_monitor &&
      backend->output.width == event->u.output.width &&
      backend->output.height == event->u.output.height &&
      backend->output.refresh_mhz == event->u.output.refresh_mhz)
    return;

  /* Match the display-refresh path: detach presentation and retire the local
   * pending frame before release_stage_view(), then re-import for the new view.
   * This is a view change, not a transport-session drop. */
  deactivate_consumer (backend);
  backend->output.width = event->u.output.width;
  backend->output.height = event->u.output.height;
  backend->output.refresh_mhz = event->u.output.refresh_mhz;

  if (!update_virtual_monitor (backend, &error))
    g_warning ("Failed to apply the Anland output change: %s",
               error ? error->message : "unknown error");

  schedule_reconnect (backend);
}

/* Translate the shared presentation events into Mutter's rendering model.
 *
 * The scene owns the frame accounting now, so this backend must not infer
 * completion from the consumer index: it reacts to what the shared layer reports.
 * A Clutter frame is ended exactly once, either as presented (PRESENTED) or as
 * finished-without-presentation (COMMIT_DROPPED). */
static void
dispatch_present_events (MetaBackendAnland *backend)
{
  anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
  size_t count = 0;
  size_t i;

  if (!backend->present_backend)
    return;

  if (anland_de_backend_dispatch (backend->present_backend, events,
                                  ANLAND_SCENE_EVENT_QUEUE, &count) != 0)
    return;

  for (i = 0; i < count; i++)
    {
      switch (events[i].type)
        {
        case ANLAND_SCENE_EVENT_PRESENTED:
          if (events[i].commit_id == backend->pending_commit_id)
            {
              backend->pending_commit_id = 0;
              complete_pending_frame (backend);
            }
          break;
        case ANLAND_SCENE_EVENT_COMMIT_DROPPED:
          if (events[i].commit_id == backend->pending_commit_id)
            {
              backend->pending_commit_id = 0;
              /* The frame never reached the consumer: end it without claiming a
               * presentation, or the frame clock would wait forever. */
              if (backend->frame_pending && backend->stage_view)
                {
                  backend->frame_pending = FALSE;
                  clutter_stage_view_notify_ready (
                    CLUTTER_STAGE_VIEW (backend->stage_view));
                }
            }
          break;
        case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
          /* The release fence is owned by the receiver. */
          if (events[i].u.released.release_fence_fd >= 0)
            close (events[i].u.released.release_fence_fd);
          break;
        case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
          /* The shared layer recorded the target; adopt it from there so the
           * generation check happens in one place. */
          apply_render_target (backend);
          break;
        case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
          apply_output_change (backend, &events[i]);
          break;
        }
    }
}

static gboolean
on_stage_view_present (ClutterStageView *stage_view,
                       ClutterFrame     *frame,
                       int64_t           global_frame_counter,
                       gpointer          user_data)
{
  MetaBackendAnland *backend = user_data;
  CoglFramebuffer *framebuffer;
  CoglContext *cogl_context;
  anland_layer_state_t state;
  uint64_t commit_id = 0;
  int fence_fd = -1;

  if (!backend->consumer_active || !backend->device ||
      !anland_device_is_connected (backend->device))
    return FALSE;

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  cogl_context = cogl_framebuffer_get_context (framebuffer);
  cogl_framebuffer_flush (framebuffer);

  #ifdef ANLAND_MUTTER_50
  if (cogl_context_has_winsys_feature (cogl_context,
                                       COGL_WINSYS_FEATURE_SYNC_FD))
#else
  if (cogl_context_has_feature (cogl_context, COGL_FEATURE_ID_SYNC_FD))
#endif
    fence_fd = cogl_context_get_latest_sync_fd (cogl_context);

  /* Native fence support is a capability, not a guarantee that this particular
   * export succeeded. Without an fd, wait for rendering before committing the
   * buffer to the consumer. */
  if (fence_fd < 0)
    {
      if (!backend->warned_sync_fallback)
        {
          g_warning ("Anland native fence unavailable; using synchronous rendering");
          backend->warned_sync_fallback = TRUE;
        }
      cogl_framebuffer_finish (framebuffer);
    }

  /* Submit through the shared commit contract instead of pushing the fence and
   * frame-done straight to the transport: the scene owns submit/presented
   * accounting, and this backend only reports what it rendered. The output layer
   * covers the whole output because the legacy presentation flattens the scene.
   *
   * The buffer id comes from the shared render target, which is the same slot
   * apply_render_target() pointed the stage view at. Reading the consumer's
   * selection here instead would let a frame be rendered into one buffer and
   * submitted as another. */
  anland_de_target_t target;

  if (anland_de_backend_get_target (backend->present_backend, &target) != 0 ||
      target.generation != backend->session_generation ||
      target.index >= target.count)
    {
      if (fence_fd >= 0)
        close (fence_fd);
      clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_IDLE);
      return TRUE;
    }

  state = (anland_layer_state_t) {
    .layer_id = backend->output_layer,
    .buffer_id = (uint64_t) target.index + 1,
    .destination = {
      .width = backend->output.width,
      .height = backend->output.height,
    },
    .z_order = 0,
    .opacity = 1.0f,
    .visible = TRUE,
    .acquire_fence_fd = fence_fd,
    /* The whole committed buffer counts as damaged: the legacy transport does not
     * carry damage, and Mutter's own damage tracking already produced this frame. */
    .damage = NULL,
    .damage_count = 0,
  };

  const int rc = anland_de_backend_commit (backend->present_backend, &state, 1,
                                           &commit_id);

  /* acquire_fence_fd is BORROWED: the adapter dups what it needs for the flip, so
   * this frame's descriptor is still ours and must be released on every path. */
  if (fence_fd >= 0)
    close (fence_fd);

  if (rc != 0)
    {
      /* The commit was refused (back-pressure or no session): the frame did not
       * reach the consumer, so it must not be reported as pending presented. */
      clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_IDLE);
      return TRUE;
    }

  backend->pending_commit_id = commit_id;
  backend->pending_global_frame_counter = global_frame_counter;
  backend->pending_view_frame_counter = clutter_frame_get_count (frame);
  backend->frame_pending = TRUE;

  if (anland_de_backend_present (backend->present_backend) != 0)
    {
      /* Accepted but not handed over. The scene will report the drop, which ends
       * the frame; do not leave it counted as in flight here. */
      backend->frame_pending = FALSE;
      backend->pending_commit_id = 0;
      clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_IDLE);
      return TRUE;
    }

  inhibit_frame_clock (backend);
  clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_PENDING_PRESENTED);
  return TRUE;
}

static gboolean
input_source_prepare (gpointer user_data)
{
  return FALSE;
}

static gboolean
input_source_dispatch (gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);
  anland_device_input_t event;
  int result;

  if (!backend->consumer_active || !backend->device || !backend->input ||
      !anland_device_is_connected (backend->device))
    return G_SOURCE_REMOVE;

  while ((result = anland_device_poll_input (
             backend->device, &event, 0)) > 0)
    {
      MetaAnlandInputEventResult input_result;

      input_result = meta_anland_input_handle_event (backend->input,
                                                      &event,
                                                      backend->output.width,
                                                      backend->output.height);
      if (input_result == META_ANLAND_INPUT_EVENT_NEEDS_PAYLOAD)
        {
          if (!handle_extended_input (backend, &event))
            {
              invalidate_consumer (backend);
              return G_SOURCE_REMOVE;
            }
        }
      else if (input_result == META_ANLAND_INPUT_EVENT_NEEDS_RESOURCE_FDS)
        {
          if (!handle_resource_input (backend, &event))
            {
              invalidate_consumer (backend);
              return G_SOURCE_REMOVE;
            }
        }
      else if (input_result == META_ANLAND_INPUT_EVENT_ERROR)
        {
          invalidate_consumer (backend);
          return G_SOURCE_REMOVE;
        }

      if (!backend->consumer_active)
        return G_SOURCE_REMOVE;
    }

  /* A negative result means the data channel is unusable. It may or may not
   * have gone through the transport's own fallback path, so finish the job
   * here when the session is still nominally up. */
  if (result < 0)
    {
      if (backend->consumer_active)
        invalidate_consumer (backend);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
buffer_ready_source_prepare (gpointer user_data)
{
  return FALSE;
}

static gboolean
buffer_ready_source_dispatch (gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);

  if (!backend->consumer_active || !backend->present_backend)
    return G_SOURCE_REMOVE;

  /* The shared layer owns the eventfd: it turns the consumer's acknowledgement
   * into PRESENTED and publishes the next render target. This backend must not
   * read the fd itself, or the two would race for the same signal. */
  if (anland_de_backend_pump (backend->present_backend, 0) != 0)
    {
      invalidate_consumer (backend);
      return G_SOURCE_REMOVE;
    }

  dispatch_present_events (backend);

  return backend->consumer_active ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean
activate_consumer (MetaBackendAnland *backend)
{
  MetaRenderer *renderer = meta_backend_get_renderer (META_BACKEND (backend));
  MetaRendererNative *renderer_native = META_RENDERER_NATIVE (renderer);
  CoglFramebuffer *framebuffers[ANLAND_DEVICE_MAX_BUFS] = { NULL, };
  anland_device_fb_t probe = { .fd = -1 };
  uint32_t probe_width;
  uint32_t probe_height;
  int n_buffers;
  int buffer_ready_fd;
  int buffer_ready_source_fd = -1;
  int input_fd;
  int input_source_fd = -1;
  int i;
  g_autoptr (GError) error = NULL;

  n_buffers = anland_device_fb_count (backend->device);
  if (n_buffers < 1 || n_buffers > ANLAND_DEVICE_MAX_BUFS ||
      anland_device_get_fb (backend->device, 0, &probe) < 0)
    goto failed;

  /* anland_device_get_fb() returns a caller-owned dup. Only the geometry is
   * needed here, so hand the descriptor back immediately: every early return
   * below (virtual monitor rebuild, stage view not ready, placeholder size
   * mismatch, refresh-rate mismatch) would otherwise leak one fd per attempt. */
  probe_width = probe.width;
  probe_height = probe.height;
  close (probe.fd);
  probe.fd = -1;

  if (probe_width != backend->output.width ||
      probe_height != backend->output.height)
    {
      backend->output.width = probe_width;
      backend->output.height = probe_height;
      if (!update_virtual_monitor (backend, &error))
        g_warning ("Failed to update the Anland virtual monitor: %s",
                   error->message);
      return FALSE;
    }

  if (!ensure_stage_view (backend, &error))
    return FALSE;

  if (!backend->clipboard)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Anland clipboard bridge is unavailable");
      goto failed;
    }

  if (cogl_framebuffer_get_width (backend->placeholder_framebuffer) !=
        (int) probe_width ||
      cogl_framebuffer_get_height (backend->placeholder_framebuffer) !=
        (int) probe_height)
    return FALSE;

  if (backend->output.refresh_mhz > 0 &&
      fabsf (clutter_stage_view_get_refresh_rate (
               CLUTTER_STAGE_VIEW (backend->stage_view)) -
             backend->output.refresh_mhz / 1000.0f) > 0.01f)
    return FALSE;

  for (i = 0; i < n_buffers; i++)
    {
      anland_device_fb_t fb = { .fd = -1 };
      int fd;
      uint32_t stride;
      uint32_t offset;
      uint64_t modifier;

      if (anland_device_get_fb (backend->device, i, &fb) < 0)
        goto failed;

      if (!validate_buffer_info (&fb, &error))
        {
          close (fb.fd);
          fb.fd = -1;
          goto failed;
        }

      fd = fb.fd;
      stride = fb.stride;
      offset = fb.offset;
      modifier = fb.modifier;
      framebuffers[i] = meta_renderer_native_create_dma_buf_framebuffer (
        renderer_native, fb.width, fb.height,
        DRM_FORMAT_ABGR8888, 1, &fd, &stride, &offset, &modifier, &error);

      /* meta_renderer_native_create_dma_buf_framebuffer() imports the dmabuf
       * into an EGLImage synchronously (EGL_IMAGE_PRESERVED_KHR), so the dup can
       * be closed as soon as the import returned — the framebuffer keeps the
       * kernel-side buffer alive through its own reference. This holds on the
       * failure path too: the import either took its own reference or did not
       * use the fd at all. */
      close (fb.fd);
      fb.fd = -1;

      if (!framebuffers[i])
        {
          g_prefix_error (&error,
                          "Failed to import Anland buffer %d "
                          "(%ux%u, stride %u, offset %u, "
                          "modifier %" G_GUINT64_FORMAT "): ",
                          i, fb.width, fb.height,
                          fb.stride, fb.offset, fb.modifier);
          goto failed;
        }
    }

  buffer_ready_fd = anland_device_buffer_ready_fd (backend->device);
  buffer_ready_source_fd = fcntl (buffer_ready_fd, F_DUPFD_CLOEXEC, 3);
  if (buffer_ready_source_fd < 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to duplicate Anland buffer-ready eventfd");
      goto failed;
    }

  input_fd = anland_device_data_fd (backend->device);
  input_source_fd = fcntl (input_fd, F_DUPFD_CLOEXEC, 3);
  if (input_source_fd < 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to duplicate Anland data fd");
      goto failed;
    }

  for (i = 0; i < n_buffers; i++)
    backend->buffer_framebuffers[i] = framebuffers[i];
  backend->consumer_active = TRUE;
  meta_anland_clipboard_set_device (backend->clipboard, backend->device);
  backend->current_buffer = -1;
  backend->pending_commit_id = 0;
  /* This backend now serves the session whose buffers it just imported, so it
   * must record the generation that session's targets are stamped with. */
  backend->session_generation = anland_scene_generation (
    anland_de_backend_scene (backend->present_backend));
  backend->buffer_count = (uint32_t) n_buffers;
  inhibit_frame_clock (backend);
  clutter_stage_view_set_present_func (CLUTTER_STAGE_VIEW (backend->stage_view),
                                       on_stage_view_present, backend);
  backend->buffer_ready_source = meta_create_fd_source (
    buffer_ready_source_fd, "[mutter] Anland buffer ready",
    buffer_ready_source_prepare, buffer_ready_source_dispatch, backend, NULL);
  g_source_attach (backend->buffer_ready_source, NULL);
  backend->input_source = meta_create_fd_source (
    input_source_fd, "[mutter] Anland input",
    input_source_prepare, input_source_dispatch, backend, NULL);
  g_source_attach (backend->input_source, NULL);
  meta_anland_audio_set_fd (backend->audio,
                            anland_device_audio_fd (
                              backend->device));
  request_camera_resources (backend);
  /* An initial reconnect publishes an event, but rebuilding a renderer view
   * during the SAME consumer session does not publish a second one. Drain any
   * events, then adopt the current target explicitly if still active; the
   * generation and imported-buffer checks in apply_render_target() keep this
   * safe across session changes. */
  dispatch_present_events (backend);
  if (backend->consumer_active)
    apply_render_target (backend);
  /* An output event may have rebuilt the view and deactivated us. Keep the

   * current reconnect timer alive until that new view is actually activated. */
  return backend->consumer_active;

failed:
  g_warning ("Failed to activate Anland consumer buffers: %s",
             error ? error->message : "invalid transport state");
  for (i = 0; i < ANLAND_DEVICE_MAX_BUFS; i++)
    g_clear_object (&framebuffers[i]);
  if (buffer_ready_source_fd >= 0)
    close (buffer_ready_source_fd);
  if (input_source_fd >= 0)
    close (input_source_fd);
  /* Same rule as every other self-inflicted session drop: release what this
   * backend borrowed and make sure a reconnect tick is pending. */
  invalidate_consumer (backend);
  return FALSE;
}

static gboolean reconnect_cb (gpointer user_data);

static void
schedule_reconnect (MetaBackendAnland *backend)
{
  if (backend->reconnect_source_id)
    return;

  backend->reconnect_source_id =
    g_timeout_add (ANLAND_RECONNECT_INTERVAL_MS, reconnect_cb, backend);
}

static void
on_device_fallback (void *user_data)
{
  MetaBackendAnland *backend = user_data;

  deactivate_consumer (backend);
  schedule_reconnect (backend);
}

static void
on_input_display_refresh (uint32_t refresh_mhz,
                          gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);

  if (backend->output.refresh_mhz == refresh_mhz)
    return;

  backend->output.refresh_mhz = refresh_mhz;
  deactivate_consumer (backend);
  if (!update_virtual_monitor (backend, NULL))
    {
      clear_device (backend);
      return;
    }

  schedule_reconnect (backend);
}

static gboolean
reconnect_cb (gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);

  if (!backend->present_backend)
    {
      if (!connect_device (backend, NULL))
        return G_SOURCE_CONTINUE;
      if (!update_virtual_monitor (backend, NULL))
        {
          clear_device (backend);
          return G_SOURCE_CONTINUE;
        }
      anland_device_set_fallback_cb (backend->device, on_device_fallback,
                                     backend);
    }

  /* A dead daemon invalidates the whole connection: drop the facade so the next
   * tick builds a fresh one. Polling reconnect() cannot help while the control
   * connection is gone. */
  if (!anland_device_is_daemon_alive (backend->device))
    {
      clear_device (backend);
      return G_SOURCE_CONTINUE;
    }

  /* Reconnect waits for the old session's events to drain before reusing buffer
   * identities. Keep draining on every tick, even if retirement takes more than
   * one dispatch batch, so activation is not required to finish teardown. */
  dispatch_present_events (backend);

  /* The shared layer runs the handshake and publishes the first render target
   * on success. */
  if (anland_de_backend_reconnect (backend->present_backend) != 0)
    return G_SOURCE_CONTINUE; /* no consumer yet, or unusable selection */

  if (!activate_consumer (backend))
    return G_SOURCE_CONTINUE;

  backend->reconnect_source_id = 0;
  return G_SOURCE_REMOVE;
}

static gboolean
meta_backend_anland_init_post (MetaBackend  *backend,
                               GError      **error)
{
  MetaBackendClass *parent_class =
    META_BACKEND_CLASS (meta_backend_anland_parent_class);
  MetaBackendAnland *backend_anland = META_BACKEND_ANLAND (backend);

  if (!parent_class->init_post (backend, error))
    return FALSE;

  backend_anland->input = meta_anland_input_new (
    backend, on_input_display_refresh, backend_anland, error);
  if (!backend_anland->input)
    return FALSE;

  backend_anland->audio = meta_anland_audio_new ();
  if (!backend_anland->audio)
    g_warning ("Failed to initialize Anland audio bridge");

  backend_anland->camera = meta_anland_camera_new ();
  if (!backend_anland->camera)
    g_warning ("Failed to initialize Anland camera bridge");

  g_signal_connect_object (meta_backend_get_context (backend), "started",
                           G_CALLBACK (on_context_started), backend, 0);

  if (!connect_device (backend_anland, error))
    return FALSE;

  anland_device_set_fallback_cb (backend_anland->device, on_device_fallback,
                                 backend_anland);
  return TRUE;
}

static void
meta_backend_anland_update_stage (MetaBackend *backend)
{
  MetaBackendAnland *backend_anland = META_BACKEND_ANLAND (backend);
  MetaBackendClass *parent_class =
    META_BACKEND_CLASS (meta_backend_anland_parent_class);
  gboolean had_stage_view = backend_anland->stage_view != NULL;

  if (backend_anland->consumer_active)
    deactivate_consumer (backend_anland);
  release_stage_view (backend_anland);

  parent_class->update_stage (backend);

  if (had_stage_view && backend_anland->present_backend)
    schedule_reconnect (backend_anland);
}

static void
meta_backend_anland_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (object);

  switch (prop_id)
    {
    case PROP_ANLAND_SOCKET:
      if (g_value_get_string (value))
        {
          g_free (backend->socket_path);
          backend->socket_path = g_value_dup_string (value);
        }
      break;
    default:
      G_OBJECT_CLASS (meta_backend_anland_parent_class)->set_property (object,
                                                                        prop_id,
                                                                        value,
                                                                        pspec);
      break;
    }
}

static void
meta_backend_anland_dispose (GObject *object)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (object);

  g_clear_handle_id (&backend->reconnect_source_id, g_source_remove);
  clear_device (backend);
  uninhibit_frame_clock (backend);
  g_clear_pointer (&backend->audio, meta_anland_audio_free);
  g_clear_pointer (&backend->camera, meta_anland_camera_free);
  g_clear_pointer (&backend->clipboard, meta_anland_clipboard_free);
  g_clear_pointer (&backend->input, meta_anland_input_free);
  g_clear_object (&backend->placeholder_framebuffer);
  g_clear_object (&backend->stage_view);
  g_clear_object (&backend->virtual_monitor);
  g_clear_pointer (&backend->socket_path, g_free);

  G_OBJECT_CLASS (meta_backend_anland_parent_class)->dispose (object);
}

static void
meta_backend_anland_class_init (MetaBackendAnlandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaBackendClass *backend_class = META_BACKEND_CLASS (klass);

  object_class->set_property = meta_backend_anland_set_property;
  object_class->dispose = meta_backend_anland_dispose;
  backend_class->init_post = meta_backend_anland_init_post;
  backend_class->update_stage = meta_backend_anland_update_stage;

  g_object_class_install_property (
    object_class, PROP_ANLAND_SOCKET,
    g_param_spec_string ("anland-socket", NULL, NULL, NULL,
                         G_PARAM_WRITABLE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS));
}

static void
meta_backend_anland_init (MetaBackendAnland *backend_anland)
{
  backend_anland->socket_path = get_default_socket_path ();
  backend_anland->current_buffer = -1;
}

gboolean
meta_backend_anland_setup (MetaBackendAnland  *backend,
                           GError            **error)
{
  if (!update_virtual_monitor (backend, error))
    return FALSE;

  schedule_reconnect (backend);
  return TRUE;
}
