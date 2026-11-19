/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include <glib.h>
#include <wayland-server-core.h>

#include "wayland/meta-wayland-types.h"

struct xkb_keymap;

MetaWaylandSurface * meta_anland_keyboard_get_focus (MetaWaylandKeyboard *keyboard);

struct xkb_keymap * meta_anland_keyboard_get_keymap (MetaWaylandKeyboard *keyboard);

gboolean meta_anland_keyboard_send_text_keys (MetaWaylandKeyboard *keyboard,
                                             struct wl_client    *client,
                                             const char          *keymap,
                                             const uint32_t      *keys,
                                             size_t               n_keys);

void meta_anland_keyboard_restore_keymap (MetaWaylandKeyboard *keyboard,
                                          struct wl_client    *client);
