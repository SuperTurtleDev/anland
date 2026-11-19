/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Included by Mutter's meta-wayland-keyboard.c. Use the keyboard's real focus
 * and modifier state without replacing the compositor-wide layout. */
#include "backends/anland/meta-anland-keyboard.h"

#if __has_include("core/meta-anonymous-file.h")
#include "core/meta-anonymous-file.h"
#else
#include "mtk/mtk.h"
#endif

MetaWaylandSurface *
meta_anland_keyboard_get_focus (MetaWaylandKeyboard *keyboard)
{
  if (wl_list_empty (&keyboard->focus_resource_list))
    return NULL;

  return keyboard->focus_surface;
}

struct xkb_keymap *
meta_anland_keyboard_get_keymap (MetaWaylandKeyboard *keyboard)
{
  return keyboard->xkb_info.keymap;
}

gboolean
meta_anland_keyboard_send_text_keys (MetaWaylandKeyboard *keyboard,
                                    struct wl_client    *client,
                                    const char          *keymap,
                                    const uint32_t      *keys,
                                    size_t               n_keys)
{
  MetaWaylandInputDevice *input_device = META_WAYLAND_INPUT_DEVICE (keyboard);
  struct wl_resource *resource;
  gboolean sent = FALSE;
  size_t size = strlen (keymap) + 1;
  uint32_t time = g_get_monotonic_time () / 1000;
#if __has_include("core/meta-anonymous-file.h")
  MetaAnonymousFile *file = meta_anonymous_file_new (size,
                                                    (const uint8_t *) keymap);
#else
  g_autoptr (MtkAnonymousFile) file =
    mtk_anonymous_file_new ("anland-text-keymap", size,
                            (const uint8_t *) keymap);
#endif

  if (!file)
    return FALSE;

  wl_resource_for_each (resource, &keyboard->focus_resource_list)
    {
      gboolean private_mapping = wl_resource_get_version (resource) >= 7;
      int fd;
      size_t i;

      if (wl_resource_get_client (resource) != client)
        continue;

#if __has_include("core/meta-anonymous-file.h")
      fd = meta_anonymous_file_open_fd (file,
                                       private_mapping ?
                                       META_ANONYMOUS_FILE_MAPMODE_PRIVATE :
                                       META_ANONYMOUS_FILE_MAPMODE_SHARED);
#else
      fd = mtk_anonymous_file_open_fd (file,
                                      private_mapping ?
                                      MTK_ANONYMOUS_FILE_MAPMODE_PRIVATE :
                                      MTK_ANONYMOUS_FILE_MAPMODE_SHARED);
#endif
      if (fd < 0)
        continue;

      wl_keyboard_send_keymap (resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                               fd, size);
#if __has_include("core/meta-anonymous-file.h")
      meta_anonymous_file_close_fd (fd);
#else
      mtk_anonymous_file_close_fd (fd);
#endif
      wl_keyboard_send_modifiers (resource,
                                  meta_wayland_input_device_next_serial (input_device),
                                  0, 0, 0, 0);
      for (i = 0; i < n_keys; i++)
        {
          wl_keyboard_send_key (resource,
                                meta_wayland_input_device_next_serial (input_device),
                                time, keys[i], WL_KEYBOARD_KEY_STATE_PRESSED);
          wl_keyboard_send_key (resource,
                                meta_wayland_input_device_next_serial (input_device),
                                time, keys[i], WL_KEYBOARD_KEY_STATE_RELEASED);
        }
      keyboard_send_modifiers (keyboard, resource,
                               meta_wayland_input_device_next_serial (input_device));
      sent = TRUE;
    }

#if __has_include("core/meta-anonymous-file.h")
  meta_anonymous_file_free (file);
#endif

  return sent;
}

void
meta_anland_keyboard_restore_keymap (MetaWaylandKeyboard *keyboard,
                                    struct wl_client    *client)
{
  MetaWaylandInputDevice *input_device = META_WAYLAND_INPUT_DEVICE (keyboard);
  struct wl_resource *resource;

  wl_resource_for_each (resource, &keyboard->resource_list)
    if (wl_resource_get_client (resource) == client)
      send_keymap (keyboard, resource);

  wl_resource_for_each (resource, &keyboard->focus_resource_list)
    {
      if (wl_resource_get_client (resource) != client)
        continue;
      send_keymap (keyboard, resource);
      keyboard_send_modifiers (keyboard, resource,
                               meta_wayland_input_device_next_serial (input_device));
    }
}
