/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "clutter/clutter-mutter.h"
#include "backends/anland/libdisplay_producer/anland_hdr.h"

ClutterColorState * meta_anland_color_state_new (ClutterContext *context,
                                                gboolean        hdr,
                                                gboolean        output,
                                                float           sdr_white_nits);

MtkRegion * meta_anland_accumulate_copy_damage (MtkRegion       **slot_damage,
                                                unsigned int      n_slots,
                                                unsigned int      current,
                                                const MtkRegion  *scene_damage);
