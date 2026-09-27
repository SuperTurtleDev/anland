/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include <gio/gio.h>

#include "backends/anland/libdisplay_producer/anland_device.h"

typedef struct _MetaAnlandClipboard MetaAnlandClipboard;
typedef struct _MetaBackend MetaBackend;

MetaAnlandClipboard * meta_anland_clipboard_new (MetaBackend *backend);
void meta_anland_clipboard_free (MetaAnlandClipboard *clipboard);

void meta_anland_clipboard_set_device (MetaAnlandClipboard *clipboard,
                                       anland_device       *device);

gboolean meta_anland_clipboard_set_from_consumer (MetaAnlandClipboard *clipboard,
                                                   GBytes              *contents);