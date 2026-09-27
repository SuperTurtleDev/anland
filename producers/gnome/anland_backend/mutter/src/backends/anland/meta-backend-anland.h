/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include "backends/native/meta-backend-native-private.h"

#define META_TYPE_BACKEND_ANLAND (meta_backend_anland_get_type ())
G_DECLARE_FINAL_TYPE (MetaBackendAnland,
                      meta_backend_anland,
                      META, BACKEND_ANLAND,
                      MetaBackendNative)

gboolean meta_backend_anland_setup (MetaBackendAnland  *backend,
                                    GError            **error);
