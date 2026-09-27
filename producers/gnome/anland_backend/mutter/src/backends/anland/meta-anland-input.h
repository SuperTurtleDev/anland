/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include <glib.h>

#include "backends/anland/libdisplay_producer/anland_device.h"
#include "backends/meta-backend-types.h"

typedef struct _MetaAnlandInput MetaAnlandInput;

typedef enum
{
  META_ANLAND_INPUT_EVENT_HANDLED,
  META_ANLAND_INPUT_EVENT_NEEDS_PAYLOAD,
  META_ANLAND_INPUT_EVENT_NEEDS_RESOURCE_FDS,
  META_ANLAND_INPUT_EVENT_ERROR,
} MetaAnlandInputEventResult;

typedef void (* MetaAnlandInputRefreshFunc) (uint32_t refresh_mhz,
                                              gpointer user_data);

MetaAnlandInput * meta_anland_input_new (MetaBackend                *backend,
                                         MetaAnlandInputRefreshFunc   refresh_func,
                                         gpointer                     user_data,
                                         GError                     **error);
void meta_anland_input_free (MetaAnlandInput *input);

MetaAnlandInputEventResult
meta_anland_input_handle_event (MetaAnlandInput              *input,
                                const anland_device_input_t  *event,
                                uint32_t                      input_width,
                                uint32_t                      input_height);
gboolean meta_anland_input_inject_text (MetaAnlandInput *input,
                                        const char      *text);
void meta_anland_input_reset (MetaAnlandInput *input);