/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "clutter/clutter-mutter.h"
#include "backends/anland/libdisplay_producer/anland_hdr.h"

ClutterColorState * meta_anland_color_state_new (ClutterContext *context,
                                                gboolean        hdr,
                                                gboolean        output,
                                                float           sdr_white_nits,
                                                unsigned int    hdr_content_gain_percent);

/* Render-only source state for a client texture. The original protocol state,
 * SDR reference white and final output state are never changed. Full reference. */
ClutterColorState * meta_anland_color_state_for_content (ClutterColorState *source,
                                                        ClutterColorState *target);

MtkRegion * meta_anland_accumulate_copy_damage (MtkRegion       **slot_damage,
                                                unsigned int      n_slots,
                                                unsigned int      current,
                                                const MtkRegion  *scene_damage);
