/* Anland device layer regression test (Mutter side).
 *
 * The backend no longer carries its own protocol fork or transport glue: it
 * talks to the shared DRM-like device layer (libdisplay_producer/anland_device.h)
 * and this test drives that layer against a mock daemon, exactly the way the
 * Mutter backend does in production. It covers the full lifecycle the backend
 * depends on:
 *
 *   connect (consumer absent → consumer appears)
 *   connector state, dmabuf import, buffer-ready eventfd, audio socketpair
 *   input events (pointer / touch / display refresh), variable-length payloads
 *   resource fds (camera), output events, render fence + frame-done
 *   consumer loss (fallback) and re-establishment with the same device handle
 *
 * It also pins the wire ABI shared by the daemon and the Android consumer.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for memfd/dup(); DE build systems already define it */
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "anland_device.h"
#include "protocol.h"
#include "socket_utils.h"

/* The producer, the daemon and the consumer must agree on these layouts: a
 * mismatch here silently mis-decodes every message on the wire. */
_Static_assert (sizeof (struct ctrl_msg) == 8,
                "Anland control header ABI changed");
_Static_assert (sizeof (struct data_msg) == 8,
                "Anland data header ABI changed");
_Static_assert (sizeof (struct screen_info) == 16,
                "Anland screen info ABI changed");
_Static_assert (sizeof (struct buf_info) == 28,
                "Anland buffer metadata ABI changed");
_Static_assert (sizeof (struct InputEvent) == 20,
                "Anland input event ABI changed");
_Static_assert (sizeof (struct OutputEvent) == 20,
                "Anland output event ABI changed");
_Static_assert (sizeof (struct audio_format) == 20,
                "Anland audio format ABI changed");
_Static_assert (sizeof (struct audio_msg) == 8,
                "Anland audio header ABI changed");
/* The public input struct must stay layout-compatible with the wire event so
 * the device layer can hand events through without decoding them. */
_Static_assert (sizeof (anland_device_input_t) == sizeof (struct InputEvent),
                "anland_device_input_t must match the wire InputEvent");

typedef struct
{
  int ready;
  int fence;
  int data;
  int audio;
} MockPeers;

typedef struct
{
  int listen_fd;
  GMutex lock;
  MockPeers peers;
  gboolean delay_first_pickup;
  gboolean failed;
} MockDaemon;

static void
close_fd (int *fd)
{
  if (*fd >= 0)
    {
      close (*fd);
      *fd = -1;
    }
}

static void
close_peers (MockDaemon *daemon)
{
  MockPeers peers;

  g_mutex_lock (&daemon->lock);
  peers = daemon->peers;
  daemon->peers = (MockPeers) { -1, -1, -1, -1 };
  g_mutex_unlock (&daemon->lock);

  close_fd (&peers.ready);
  close_fd (&peers.fence);
  close_fd (&peers.data);
  close_fd (&peers.audio);
}

static MockPeers
get_peers (MockDaemon *daemon)
{
  MockPeers peers;

  g_mutex_lock (&daemon->lock);
  peers = daemon->peers;
  g_mutex_unlock (&daemon->lock);
  return peers;
}

static int
new_memfd (const char *name,
           size_t      size)
{
  int fd = memfd_create (name, MFD_CLOEXEC);

  if (fd < 0 || ftruncate (fd, (off_t) size) < 0)
    {
      close_fd (&fd);
      return -1;
    }

  return fd;
}

static gboolean
send_screen_info (int control_fd)
{
  struct ctrl_msg header = {
    .type = CTRL_MSG_SCREEN_INFO,
    .size = sizeof (struct screen_info),
  };
  struct screen_info info = {
    .width = 1920,
    .height = 1080,
    .format = 1,
    .refresh = 120000,
  };

  return send_all (control_fd, &header, sizeof (header)) == 0 &&
         send_all (control_fd, &info, sizeof (info)) == 0;
}

static gboolean
send_buffers (int data_fd)
{
  struct data_msg header = {
    .type = DATA_MSG_BUFS_READY,
    .size = 2 * sizeof (struct buf_info),
  };
  struct buf_info infos[2] = {
    { .stride = 5120, .width = 1280, .height = 720,
      .format = 1, .modifier = 0, .offset = 0 },
    { .stride = 5120, .width = 1280, .height = 720,
      .format = 1, .modifier = 0, .offset = 0 },
  };
  int fds[2] = {
    new_memfd ("anland-device-buffer-0", 4096),
    new_memfd ("anland-device-buffer-1", 4096),
  };
  gboolean result;

  result = fds[0] >= 0 && fds[1] >= 0 &&
           send_fds (data_fd, &header, sizeof (header), fds, 2) == 0 &&
           send_all (data_fd, infos, sizeof (infos)) == 0;
  close_fd (&fds[0]);
  close_fd (&fds[1]);
  return result;
}

static gboolean
publish_consumer (MockDaemon *daemon,
                  int         control_fd)
{
  struct ctrl_msg response = { .type = CTRL_MSG_FDS_READY, .size = 0 };
  int ready = -1;
  int fence[2] = { -1, -1 };
  int data[2] = { -1, -1 };
  int audio[2] = { -1, -1 };
  int shm_fd = -1;
  int producer_fds[5];
  uint32_t selected = 1;
  gboolean result = FALSE;

  ready = eventfd (0, EFD_CLOEXEC);
  if (ready < 0 ||
      socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fence) < 0 ||
      socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data) < 0 ||
      socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, audio) < 0)
    goto out;
  shm_fd = new_memfd ("anland-device-index", sizeof (selected));
  if (shm_fd < 0 || pwrite (shm_fd, &selected, sizeof (selected), 0) !=
                   (ssize_t) sizeof (selected))
    goto out;

  /* Slot order the producer expects: { buf_ready, fence, data, shm, audio }. */
  producer_fds[0] = ready;
  producer_fds[1] = fence[0];
  producer_fds[2] = data[0];
  producer_fds[3] = shm_fd;
  producer_fds[4] = audio[0];
  if (send_fds (control_fd, &response, sizeof (response),
                producer_fds, G_N_ELEMENTS (producer_fds)) < 0)
    goto out;

  g_mutex_lock (&daemon->lock);
  daemon->peers = (MockPeers) {
    .ready = ready, .fence = fence[1], .data = data[1], .audio = audio[1],
  };
  g_mutex_unlock (&daemon->lock);
  ready = fence[1] = data[1] = audio[1] = -1;
  if (!send_buffers (get_peers (daemon).data))
    goto out;
  result = TRUE;

out:
  close_fd (&ready);
  close_fd (&fence[0]);
  close_fd (&fence[1]);
  close_fd (&data[0]);
  close_fd (&data[1]);
  close_fd (&audio[0]);
  close_fd (&audio[1]);
  close_fd (&shm_fd);
  if (!result)
    close_peers (daemon);
  return result;
}

static gpointer
mock_daemon_thread (gpointer user_data)
{
  MockDaemon *daemon = user_data;
  struct ctrl_msg request;
  int control_fd = -1;

  control_fd = accept4 (daemon->listen_fd, NULL, NULL, SOCK_CLOEXEC);
  if (control_fd < 0 ||
      recv_all (control_fd, &request, sizeof (request)) < 0 ||
      request.type != CTRL_MSG_PRODUCER_HELLO || request.size != 0 ||
      !send_screen_info (control_fd))
    goto failed;

  while (recv_all (control_fd, &request, sizeof (request)) == 0)
    {
      if (request.type != CTRL_MSG_PICKUP_FDS || request.size != 0)
        goto failed;
      if (daemon->delay_first_pickup)
        {
          /* Simulate "no consumer is up yet": the producer must stay in
           * fallback and report NO_CONSUMER rather than a dead daemon. */
          daemon->delay_first_pickup = FALSE;
          continue;
        }
      if (!publish_consumer (daemon, control_fd))
        goto failed;
    }

  close_fd (&control_fd);
  return NULL;

failed:
  daemon->failed = TRUE;
  close_fd (&control_fd);
  return NULL;
}

static int
create_listener (const char *path)
{
  struct sockaddr_un address = { 0 };
  int fd;

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  address.sun_family = AF_UNIX;
  if (strlen (path) >= sizeof (address.sun_path))
    goto failed;
  strcpy (address.sun_path, path);
  if (bind (fd, (struct sockaddr *) &address, sizeof (address)) < 0 ||
      listen (fd, 1) < 0)
    goto failed;

  return fd;

failed:
  close (fd);
  return -1;
}

static void
send_data_message (int         fd,
                   uint32_t    type,
                   const void *payload,
                   size_t      size)
{
  struct data_msg header = { .type = type, .size = size };

  g_assert_cmpint (send_all (fd, &header, sizeof (header)), ==, 0);
  if (size > 0)
    g_assert_cmpint (send_all (fd, payload, size), ==, 0);
}

static void
fallback_callback (void *user_data)
{
  int *count = user_data;

  (*count)++;
}

static void
test_device_and_reconnect (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp ("anland-device-XXXXXX", &error);
  g_autofree char *socket_path = NULL;
  MockDaemon daemon = {
    .listen_fd = -1,
    .peers = { -1, -1, -1, -1 },
    .delay_first_pickup = TRUE,
  };
  anland_device *device = NULL;
  anland_device_output_t output;
  anland_device_fb_t fb;
  anland_device_input_t event = { 0 };
  anland_device_input_t received_event;
  struct data_msg output_header;
  struct OutputEvent received_output;
  MockPeers peers;
  GThread *thread;
  char text[] = "Anland text";
  char audio_byte = 'A';
  char received_audio = 0;
  char received_text[sizeof (text)] = { 0 };
  char fence_byte = 1;
  int resource_fds[2] = { -1, -1 };
  int received_resource_fds[2] = { -1, -1 };
  int n_resource_fds = 0;
  int fence_fds[1] = { -1 };
  int n_fence_fds = 0;
  int pipe_fds[2] = { -1, -1 };
  int fallback_count = 0;
  eventfd_t ready_value = 5;
  eventfd_t received_ready = 0;

  g_assert_no_error (error);
  socket_path = g_build_filename (directory, "display.sock", NULL);
  daemon.listen_fd = create_listener (socket_path);
  if (daemon.listen_fd < 0)
    g_error ("failed to create mock Anland socket %s: %s",
             socket_path, g_strerror (errno));
  g_mutex_init (&daemon.lock);
  thread = g_thread_new ("anland-mock-daemon", mock_daemon_thread, &daemon);

  /* ---- open + connector ---- */
  device = anland_device_open (socket_path);
  g_assert_nonnull (device);
  g_assert_cmpstr (anland_device_socket_path (device), ==, socket_path);
  g_assert_cmpint (anland_device_get_outputs (device, &output, 1), ==, 1);
  g_assert_cmpuint (output.width, ==, 1920);
  g_assert_cmpuint (output.height, ==, 1080);
  g_assert_cmpuint (output.refresh_mhz, ==, 120000);
  g_assert_false (anland_device_is_connected (device));

  /* ---- consumer appears after the first attempt ---- */
  g_assert_cmpint (anland_device_connect_result (device), ==,
                   ANLAND_DEVICE_CONNECT_NO_CONSUMER);
  g_assert_false (anland_device_is_connected (device));
  g_assert_cmpint (anland_device_connect_result (device), ==,
                   ANLAND_DEVICE_CONNECT_READY);
  g_assert_true (anland_device_is_connected (device));

  /* ---- dmabuf set ---- */
  g_assert_cmpint (anland_device_fb_count (device), ==, 2);
  g_assert_cmpint (anland_device_current_fb_raw (device), ==, 1);
  g_assert_cmpint (anland_device_current_fb (device), ==, 1);
  g_assert_cmpint (anland_device_get_fb (device, 0, &fb), ==, 0);
  g_assert_cmpint (fb.fd, >=, 0);
  g_assert_cmpuint (fb.width, ==, 1280);
  g_assert_cmpuint (fb.height, ==, 720);
  g_assert_cmpuint (fb.stride, ==, 5120);
  g_assert_cmpuint (fb.format, ==, 1);
  g_assert_cmpuint (fb.modifier, ==, 0);
  close_fd (&fb.fd);
  g_assert_cmpint (anland_device_get_fb (device, 2, &fb), ==, -1);

  /* ---- buffer-ready eventfd + audio socketpair ---- */
  peers = get_peers (&daemon);
  g_assert_cmpint (eventfd_write (peers.ready, ready_value), ==, 0);
  g_assert_cmpint (eventfd_read (anland_device_buffer_ready_fd (device),
                                 &received_ready), ==, 0);
  g_assert_cmpuint (received_ready, ==, ready_value);
  g_assert_cmpint (send (peers.audio, &audio_byte, sizeof (audio_byte), 0), ==, 1);
  g_assert_cmpint (recv (anland_device_audio_fd (device),
                         &received_audio, sizeof (received_audio), 0), ==, 1);
  g_assert_cmpint (received_audio, ==, audio_byte);

  /* ---- variable-length text payload ---- */
  event.type = INPUT_TYPE_TEXT_INPUT;
  event.text_input.size = sizeof (text);
  send_data_message (peers.data, DATA_MSG_INPUT_EVENT, &event, sizeof (event));
  g_assert_cmpint (send_all (peers.data, text, sizeof (text)), ==, 0);
  g_assert_cmpint (anland_device_poll_input (device, &received_event, 100), ==, 1);
  g_assert_cmpuint (received_event.type, ==, ANLAND_DEVICE_IN_TEXT_INPUT);
  g_assert_cmpint (anland_device_read_input (device, received_text,
                                             sizeof (received_text), 100), ==, 1);
  g_assert_cmpmem (received_text, sizeof (received_text), text, sizeof (text));

  /* ---- resource fds (camera hand-off) ---- */
  g_assert_cmpint (pipe2 (resource_fds, O_CLOEXEC), ==, 0);
  event = (anland_device_input_t) {
    .type = INPUT_TYPE_RESOURCE,
    .resource = {
      .type = ANLAND_DEVICE_SERVICE_CAMERA,
      .fdnum = G_N_ELEMENTS (resource_fds),
    },
  };
  send_data_message (peers.data, DATA_MSG_INPUT_EVENT, &event, sizeof (event));
  output_header = (struct data_msg) {
    .type = DATA_MSG_INPUT_EXTEND_FDS,
    .size = 0,
  };
  g_assert_cmpint (send_fds (peers.data, &output_header, sizeof (output_header),
                             resource_fds, G_N_ELEMENTS (resource_fds)), ==, 0);
  close_fd (&resource_fds[0]);
  close_fd (&resource_fds[1]);
  g_assert_cmpint (anland_device_poll_input (device, &received_event, 100), ==, 1);
  g_assert_cmpuint (received_event.type, ==, ANLAND_DEVICE_IN_RESOURCE);
  g_assert_cmpuint (received_event.resource.type, ==, ANLAND_DEVICE_SERVICE_CAMERA);
  g_assert_cmpuint (received_event.resource.fdnum, ==,
                    G_N_ELEMENTS (received_resource_fds));
  g_assert_cmpint (anland_device_read_fds (device, received_resource_fds,
                                           G_N_ELEMENTS (received_resource_fds),
                                           &n_resource_fds, 100), ==, 1);
  g_assert_cmpint (n_resource_fds, ==, G_N_ELEMENTS (received_resource_fds));
  close_fd (&received_resource_fds[0]);
  close_fd (&received_resource_fds[1]);

  /* ---- clipboard payload from the consumer ---- */
  event = (anland_device_input_t) { 0 };
  event.type = INPUT_TYPE_CLIPBOARD;
  event.clipboard.size = sizeof (text);
  send_data_message (peers.data, DATA_MSG_INPUT_EVENT, &event, sizeof (event));
  g_assert_cmpint (send_all (peers.data, text, sizeof (text)), ==, 0);
  g_assert_cmpint (anland_device_poll_input (device, &received_event, 100), ==, 1);
  g_assert_cmpuint (received_event.type, ==, ANLAND_DEVICE_IN_CLIPBOARD);
  g_assert_cmpint (anland_device_read_input (device, received_text,
                                             sizeof (received_text), 100), ==, 1);
  g_assert_cmpmem (received_text, sizeof (received_text), text, sizeof (text));

  /* ---- control channel: resources request ---- */
  g_assert_cmpint (anland_device_request_resources (device,
                                                    ANLAND_DEVICE_SERVICE_CAMERA,
                                                    NULL), ==, 0);
  g_assert_cmpint (recv_all (peers.data, &output_header,
                             sizeof (output_header)), ==, 0);
  g_assert_cmpuint (output_header.type, ==, DATA_MSG_OUTPUT_EVENT);
  g_assert_cmpint (recv_all (peers.data, &received_output,
                             sizeof (received_output)), ==, 0);
  g_assert_cmpuint (received_output.type, ==, OUTPUT_TYPE_RESOURCES_REQUEST);
  g_assert_cmpuint (received_output.resources_request.type, ==,
                    SERVICE_TYPE_CAMERA);

  /* ---- input events are handed through verbatim ---- */
  event = (anland_device_input_t) {
    .type = INPUT_TYPE_POINTER_MOTION,
    .pointer_motion = {
      .x = 640.0f,
      .y = 360.0f,
      .dx = -2.5f,
      .dy = 1.5f,
    },
  };
  send_data_message (peers.data, DATA_MSG_INPUT_EVENT, &event, sizeof (event));
  g_assert_cmpint (anland_device_poll_input (device, &received_event, 100), ==, 1);
  g_assert_cmpuint (received_event.type, ==, ANLAND_DEVICE_IN_PTR_MOTION);
  g_assert_cmpfloat (received_event.pointer_motion.x, ==, 640.0f);
  g_assert_cmpfloat (received_event.pointer_motion.dy, ==, 1.5f);

  event = (anland_device_input_t) {
    .type = INPUT_TYPE_DISPLAY_REFRESH,
    .display = { .refresh_mhz = 90000 },
  };
  send_data_message (peers.data, DATA_MSG_INPUT_EVENT, &event, sizeof (event));
  g_assert_cmpint (anland_device_poll_input (device, &received_event, 100), ==, 1);
  g_assert_cmpuint (received_event.type, ==, ANLAND_DEVICE_IN_DISPLAY_REFRESH);
  g_assert_cmpuint (received_event.display.refresh_mhz, ==, 90000);

  /* ---- clipboard push (variable-length output event) ---- */
  g_assert_cmpint (anland_device_set_clipboard (device, text, sizeof (text)), ==, 0);
  g_assert_cmpint (recv_all (peers.data, &output_header,
                             sizeof (output_header)), ==, 0);
  g_assert_cmpuint (output_header.type, ==, DATA_MSG_OUTPUT_EVENT);
  g_assert_cmpint (recv_all (peers.data, &received_output,
                             sizeof (received_output)), ==, 0);
  g_assert_cmpuint (received_output.type, ==, OUTPUT_TYPE_CLIPBOARD);
  g_assert_cmpuint (output_header.size, ==, sizeof (received_output));
  g_assert_cmpint (recv_all (peers.data, received_text,
                             sizeof (received_text)), ==, 0);
  g_assert_cmpmem (received_text, sizeof (received_text), text, sizeof (text));

  /* ---- render fence rides with the frame-done message ---- */
  g_assert_cmpint (pipe2 (pipe_fds, O_CLOEXEC), ==, 0);
  anland_device_set_render_fence (device, pipe_fds[0]);
  pipe_fds[0] = -1;
  g_assert_true (anland_device_notify_frame_done (device));
  g_assert_cmpint (recv_fds (peers.fence, &fence_byte, sizeof (fence_byte),
                             fence_fds, 1, &n_fence_fds), ==, 1);
  g_assert_cmpint (fence_byte, ==, 0);
  g_assert_cmpint (n_fence_fds, ==, 1);
  close_fd (&fence_fds[0]);
  close_fd (&pipe_fds[1]);

  /* ---- consumer loss ---- */
  anland_device_set_fallback_cb (device, fallback_callback, &fallback_count);
  g_mutex_lock (&daemon.lock);
  close_fd (&daemon.peers.data);
  g_mutex_unlock (&daemon.lock);
  g_assert_cmpint (anland_device_poll_input (device, &event, 100), ==, -1);
  g_assert_false (anland_device_is_connected (device));
  g_assert_cmpint (fallback_count, ==, 1);
  /* Nothing reached the consumer, so the backend must NOT treat this frame as
   * presented (the Mutter stage view relies on the false result). */
  g_assert_false (anland_device_notify_frame_done (device));
  g_assert_cmpint (anland_device_buffer_ready_fd (device), ==, -1);
  g_assert_cmpint (anland_device_audio_fd (device), ==, -1);
  g_assert_cmpint (anland_device_fb_count (device), ==, 0);
  close_peers (&daemon);

  /* ---- the same handle re-establishes the session ---- */
  g_assert_cmpint (anland_device_connect_result (device), ==,
                   ANLAND_DEVICE_CONNECT_READY);
  g_assert_true (anland_device_is_connected (device));
  g_assert_cmpint (anland_device_fb_count (device), ==, 2);

  anland_device_close (device);
  g_thread_join (thread);
  g_assert_false (daemon.failed);
  close_fd (&daemon.listen_fd);
  close_peers (&daemon);
  g_assert_cmpint (g_unlink (socket_path), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
  g_mutex_clear (&daemon.lock);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/backends/anland/device-and-reconnect",
                   test_device_and_reconnect);
  return g_test_run ();
}