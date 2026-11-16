//! The app's fixed RGB10/BT.2020/PQ contract, using Denial's native content CM.
use crate::color_space::BT2020;
use crate::output_color::{HdrCapability, HdrDisplay, HdrMode, HdrOutput, SignalPrimaries};
use denial_core::topology::OutputId;
use std::sync::atomic::{AtomicU32, Ordering};

// Published only at startup/rebind raster-idle boundaries or by the serial
// render operation that drains old frames before invalidating every slot.
static CONTENT_GAIN_PERCENT: AtomicU32 = AtomicU32::new(100);

pub(crate) fn set_content_gain_percent(percent: u32) {
    CONTENT_GAIN_PERCENT.store(percent.clamp(50, 200), Ordering::Relaxed);
}

pub(crate) fn apply_content_gain(
    description: &crate::color_space::ImageDescription,
    scene: crate::color_space::SceneEncoding,
    transform: &mut crate::color_space::SurfaceTransform,
) {
    use crate::color_space::{SRGB, TransferFunction, tf};
    let gain = CONTENT_GAIN_PERCENT.load(Ordering::Relaxed) as f32 / 100.0;
    if gain == 1.0 || !scene.extended {
        return;
    }
    let hdr = matches!(
        description.tf,
        TransferFunction::Named(tf::ST2084_PQ | tf::HLG)
    ) || (description.tf == TransferFunction::Named(tf::EXT_LINEAR)
        && description.primaries == SRGB
        && description.luminances.min == 0.0
        && description.luminances.max == 80.0
        && description.luminances.reference == 203.0);
    if !hdr {
        return;
    }
    // These uniforms are applied after source EOTF decoding and before the
    // existing tone mapper. Grade::same compares the complete transform, so
    // changing gain also invalidates converted copies of unchanged buffers.
    for value in &mut transform.matrix {
        *value *= gain;
    }
    transform.tone[0] *= gain;
}

pub(super) fn output(white: u32) -> HdrOutput {
    HdrOutput::new(
        HdrCapability {
            primaries: BT2020,
            max_luminance: 1000.0,
            max_frame_average: 1000.0,
            min_luminance: 0.0,
        },
        HdrMode {
            white: f64::from(white.clamp(80, 500)),
            primaries: SignalPrimaries::Bt2020,
        },
    )
}

/// Called at raster-idle startup/rebind boundaries. A change between SDR and
/// HDR first shuts down the old engine: startup reads scene_encoding() to
/// select its root color format. Live white changes use a raster operation.
pub(super) fn activate(id: OutputId, hdr: bool, white: u32, content_gain_percent: u32) {
    set_content_gain_percent(if hdr { content_gain_percent } else { 100 });
    crate::output_color::clear_hdr_requests();
    let output = output(white);
    crate::output_color::set_hdr_displays(vec![HdrDisplay {
        id,
        name: "Anland".into(),
        display: output.display,
        active: hdr.then(|| output.mode()),
    }]);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::color_space::{ImageDescription, RenderIntent, SceneEncoding, TransferFunction, tf};

    #[test]
    fn consumer_mode_drives_native_hdr_content_and_scene_precision() {
        let id = OutputId(7);
        for white in [203, 300, 400, 500, 80, 400, 203, 80] {
            activate(id, true, white, 100);
            let scene = crate::output_color::scene_encoding();
            assert!(scene.extended);
            assert!((scene.headroom - 1000.0 / f64::from(white)).abs() < 1e-9);
            let hdr = crate::output_color::hdr_output(id).unwrap();
            let preferred = ImageDescription::hdr_output(hdr);
            assert_eq!(preferred.tf, TransferFunction::Named(tf::ST2084_PQ));
            assert_eq!(preferred.primaries, BT2020);
            assert_eq!(preferred.luminances.reference, f64::from(white));
            let client = ImageDescription::windows_bt2100()
                .transform(RenderIntent::Relative, scene)
                .unwrap();
            assert!(client.float);
            assert!((f64::from(client.tone[2]) - scene.headroom).abs() < 1e-5);
            assert_eq!(client.tone[3], 1.0);
            assert_eq!(hdr.output_color().pq_white, white as f32);
            assert_eq!(hdr.display.max_luminance, 1000.0);

            // A retained HDR request cannot enable HDR in the app's 8-bit pool.
            crate::output_color::set_hdr_request("Anland", Some(hdr.mode()));
            activate(id, false, white, 100);
            assert_eq!(crate::output_color::scene_encoding(), SceneEncoding::SDR);
            assert!(crate::output_color::hdr_output(id).is_none());
            assert!(crate::output_color::hdr_request("Anland").is_none());
        }
        crate::output_color::set_hdr_displays(Vec::new());
    }
}
