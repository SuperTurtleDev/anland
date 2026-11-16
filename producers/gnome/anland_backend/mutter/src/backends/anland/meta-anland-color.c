/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backends/anland/meta-anland-color.h"

/* Only the Anland linear scene carries this key. In particular, a scene-to-PQ
 * output conversion must not apply the content gain a second time. */
#define CONTENT_GAIN_KEY "anland-hdr-content-gain-percent"
#define CONTENT_GAIN_CACHE_KEY "anland-hdr-content-render-state"

typedef struct
{
  unsigned int percent;
  ClutterColorState *state;
} ContentGainState;

static void
content_gain_state_free (gpointer data)
{
  ContentGainState *cached = data;
  g_object_unref (cached->state);
  g_free (cached);
}

ClutterColorState *
meta_anland_color_state_for_content (ClutterColorState *source,
                                     ClutterColorState *target)
{
  unsigned int percent = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (target),
                                                            CONTENT_GAIN_KEY));
  ClutterColorStateParams *params;
  const ClutterEOTF *eotf;
  const ClutterColorimetry *colorimetry;
  const ClutterLuminance *lum;
  ContentGainState *cached;
  g_autoptr (ClutterContext) context = NULL;

  if (percent == ANLAND_HDR_CONTENT_GAIN_DEFAULT_PERCENT ||
      percent < ANLAND_HDR_CONTENT_GAIN_MIN_PERCENT ||
      percent > ANLAND_HDR_CONTENT_GAIN_MAX_PERCENT ||
      !CLUTTER_IS_COLOR_STATE_PARAMS (source))
    return g_object_ref (source);

  params = CLUTTER_COLOR_STATE_PARAMS (source);
  eotf = clutter_color_state_params_get_eotf (params);
  /* PQ is the HDR input supported by these Mutter releases. Linear also
   * describes ordinary intermediate images; it must never imply HDR. Mutter
   * currently rejects Windows scRGB and has no HLG source decoder here. */
  if (eotf->type != CLUTTER_EOTF_TYPE_NAMED ||
      eotf->tf_name != CLUTTER_TRANSFER_FUNCTION_PQ)
    return g_object_ref (source);

  cached = g_object_get_data (G_OBJECT (source), CONTENT_GAIN_CACHE_KEY);
  if (cached && cached->percent == percent)
    return g_object_ref (cached->state);

  colorimetry = clutter_color_state_params_get_colorimetry (params);
  lum = clutter_color_state_params_get_luminance (params);
  g_object_get (source, "context", &context, NULL);
  cached = g_new0 (ContentGainState, 1);
  cached->percent = percent;
  /* Source reference appears in the denominator of native linear luminance
   * mapping. A render-local reference / gain scales only this source's light,
   * and native tone mapping sees the same increased peak/reference ratio.
   * Keep the PQ 0..10,000-nit EOTF and source mastering/black values intact. */
  cached->state = clutter_color_state_params_new_full (
    context,
    colorimetry->type == CLUTTER_COLORIMETRY_TYPE_COLORSPACE
      ? colorimetry->colorspace : CLUTTER_COLORSPACE_SRGB,
    CLUTTER_TRANSFER_FUNCTION_PQ,
    colorimetry->type == CLUTTER_COLORIMETRY_TYPE_PRIMARIES
      ? colorimetry->primaries : NULL,
    -1.0f, lum->min, lum->max,
    lum->ref * ANLAND_HDR_CONTENT_GAIN_DEFAULT_PERCENT / percent
#if defined(ANLAND_MUTTER_50) || defined(ANLAND_MUTTER_51)
    , lum->mastering_max
#elif defined(ANLAND_MUTTER_49)
    , FALSE
#endif
    );
  g_object_set_data_full (G_OBJECT (source), CONTENT_GAIN_CACHE_KEY, cached,
                          content_gain_state_free);
  return g_object_ref (cached->state);
}

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
                             gboolean        output,
                             float           sdr_white_nits,
                             unsigned int    hdr_content_gain_percent)
{
  ClutterColorState *state;
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
   * blending out of PQ. All content uses the selected SDR reference. */
  state = clutter_color_state_params_new_full (context, colorspace, transfer,
                                               NULL, -1.0f, 0.0f,
                                               output ? ANLAND_HDR_PQ_SCALE_NITS
                                                      : ANLAND_HDR_PEAK_NITS,
                                               sdr_white_nits
#if defined(ANLAND_MUTTER_50) || defined(ANLAND_MUTTER_51)
                                               , ANLAND_HDR_PEAK_NITS
#elif defined(ANLAND_MUTTER_49)
                                               , FALSE
#endif
                                               );
  if (!output)
    g_object_set_data (G_OBJECT (state), CONTENT_GAIN_KEY,
                      GUINT_TO_POINTER (hdr_content_gain_percent));
  return state;
}
