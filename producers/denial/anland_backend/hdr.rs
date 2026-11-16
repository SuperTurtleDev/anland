//! The app's fixed RGB10/BT.2020/PQ contract, using Denial's native content CM.
use crate::color_space::BT2020;
use crate::output_color::{HdrCapability, HdrDisplay, HdrMode, HdrOutput, SignalPrimaries};
use denial_core::topology::OutputId;

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
pub(super) fn activate(id: OutputId, hdr: bool, white: u32) {
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
            activate(id, true, white);
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
            activate(id, false, white);
            assert_eq!(crate::output_color::scene_encoding(), SceneEncoding::SDR);
            assert!(crate::output_color::hdr_output(id).is_none());
            assert!(crate::output_color::hdr_request("Anland").is_none());
        }
        crate::output_color::set_hdr_displays(Vec::new());
    }
}
