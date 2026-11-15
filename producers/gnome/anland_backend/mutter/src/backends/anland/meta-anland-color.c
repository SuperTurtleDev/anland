/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backends/anland/meta-anland-color.h"

MtkRegion *
meta_anland_accumulate_copy_damage (MtkRegion       **slot_damage,
                                    unsigned int      n_slots,
                                    unsigned int      current,
                                    const MtkRegion  *scene_damage)
{
  MtkRegion *copy_region;

  g_return_val_if_fail (current < n_slots, NULL);
  for (unsigned int i = 0; i < n_slots; i++)
    mtk_region_union (slot_damage[i], scene_damage);
  copy_region = slot_damage[current];
  slot_damage[current] = mtk_region_create ();
  return copy_region;
}

ClutterColorState *
meta_anland_color_state_new (ClutterContext *context,
                             gboolean        hdr,
                             gboolean        output)
{
  ClutterColorspace colorspace = hdr ? CLUTTER_COLORSPACE_BT2020 : CLUTTER_COLORSPACE_SRGB;
#if defined(ANLAND_MUTTER_50) || defined(ANLAND_MUTTER_51)
  ClutterTransferFunction srgb = CLUTTER_TRANSFER_FUNCTION_SRGB_PIECEWISE;
#else
  ClutterTransferFunction srgb = CLUTTER_TRANSFER_FUNCTION_SRGB;
#endif
  ClutterTransferFunction transfer = hdr ? (output ? CLUTTER_TRANSFER_FUNCTION_PQ
                                                  : CLUTTER_TRANSFER_FUNCTION_LINEAR)
                                         : srgb;

  if (!hdr)
    return clutter_color_state_params_new (context, colorspace, transfer);

  /* Scene code 1 is 1000 nits, while wire PQ code 1 remains 10000 nits.
   * Linear BT.2020 forces Clutter's real FP16 intermediate and keeps alpha
   * blending out of PQ. All content uses the same 203-nit SDR reference. */
  return clutter_color_state_params_new_full (context, colorspace, transfer,
                                               NULL, -1.0f, 0.0f,
                                               output ? ANLAND_HDR_PQ_SCALE_NITS
                                                      : ANLAND_HDR_PEAK_NITS,
                                               ANLAND_HDR_SDR_WHITE_NITS
#if defined(ANLAND_MUTTER_50) || defined(ANLAND_MUTTER_51)
                                               , ANLAND_HDR_PEAK_NITS
#elif defined(ANLAND_MUTTER_49)
                                               , FALSE
#endif
                                               );
}
