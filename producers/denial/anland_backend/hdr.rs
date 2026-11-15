//! The app's fixed RGB10/BT.2020/PQ contract, using Denial's native content CM.
use crate::color_space::BT2020;
use crate::output_color::{HdrCapability, HdrDisplay, HdrMode, HdrOutput, SignalPrimaries};
use denial_core::topology::OutputId;

pub(super) fn output() -> HdrOutput {
    HdrOutput::new(
        HdrCapability {
            primaries: BT2020,
            max_luminance: 1000.0,
            max_frame_average: 1000.0,
            min_luminance: 0.0,
        },
        HdrMode {
            white: 203.0,
            primaries: SignalPrimaries::Bt2020,
        },
    )
}

/// Called with the previous Flutter engine shut down. Engine startup reads
/// scene_encoding() to select its float root color format. No user HDR request
/// can turn an 8-bit consumer's SDR storage into an HDR signal.
pub(super) fn activate(id: OutputId, hdr: bool) {
    crate::output_color::clear_hdr_requests();
    let output = output();
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
        for _ in 0..8 {
            activate(id, true);
            let scene = crate::output_color::scene_encoding();
            assert!(scene.extended);
            assert!((scene.headroom - 1000.0 / 203.0).abs() < 1e-9);
            let hdr = crate::output_color::hdr_output(id).unwrap();
            let preferred = ImageDescription::hdr_output(hdr);
            assert_eq!(preferred.tf, TransferFunction::Named(tf::ST2084_PQ));
            assert_eq!(preferred.primaries, BT2020);
            assert_eq!(preferred.luminances.reference, 203.0);
            let client = ImageDescription::windows_bt2100()
                .transform(RenderIntent::Relative, scene)
                .unwrap();
            assert!(client.float);
            assert!(client.tone[2] > 4.9);
            assert_eq!(client.tone[3], 1.0);
            assert_eq!(hdr.output_color().pq_white, 203.0);

            // A retained HDR request cannot enable HDR in the app's 8-bit pool.
            crate::output_color::set_hdr_request("Anland", Some(hdr.mode()));
            activate(id, false);
            assert_eq!(crate::output_color::scene_encoding(), SceneEncoding::SDR);
            assert!(crate::output_color::hdr_output(id).is_none());
            assert!(crate::output_color::hdr_request("Anland").is_none());
        }
        crate::output_color::set_hdr_displays(Vec::new());
    }
}
