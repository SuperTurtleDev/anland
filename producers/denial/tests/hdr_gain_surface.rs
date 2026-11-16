// Test fixture uses the real native color_space module and the unchanged gain
// functions extracted from the production hdr.rs, not a copied implementation.
use color_space::{AlphaMode, DescriptionRecord, ImageDescription, RenderIntent, SceneEncoding, SurfaceColor, TransferFunction, tf};

fn surface(description: ImageDescription) -> SurfaceColor {
    SurfaceColor { description: Some((DescriptionRecord::new(description, false), RenderIntent::Relative)), alpha: AlphaMode::default() }
}
fn descriptions() -> Vec<ImageDescription> {
    let mut hlg=ImageDescription::windows_bt2100();
    hlg.tf=TransferFunction::Named(tf::HLG);
    hlg.luminances=hlg.tf.default_luminances();
    hlg.target_luminance=(hlg.luminances.min, hlg.luminances.max);
    vec![ImageDescription::windows_bt2100(),ImageDescription::windows_scrgb(),ImageDescription::srgb(),hlg]
}
fn scene() -> SceneEncoding { SceneEncoding { headroom: 1000.0/203.0, extended: true } }

#[test]
fn real_surface_transform_hdr_only_unity_reset_and_grade_cache() {
    use anland_backend::hdr::set_content_gain_percent;
    let scene=scene();
    let all=descriptions();
    set_content_gain_percent(100);
    let baseline:Vec<_>=all.iter().cloned().map(|d|surface(d).transform(scene).unwrap()).collect();
    // The hook at unity is bit-for-bit native, including decoder and tone map.
    for (i,d) in all.iter().enumerate() {
        assert_eq!(baseline[i],d.transform(RenderIntent::Relative,scene).unwrap());
    }
    for percent in [50,100,200,100] {
        set_content_gain_percent(percent);
        let gain=percent as f32/100.0;
        for (i,d) in all.iter().enumerate() {
            let changed=surface(d.clone()).transform(scene).unwrap();
            let old=baseline[i];
            if i==2 { assert_eq!(changed,old,"SDR remains native"); }
            else {
                for (a,b) in changed.matrix.iter().zip(old.matrix) { assert_eq!(*a,b*gain); }
                assert_eq!(changed.tone[0],old.tone[0]*gain);
                assert_eq!(&changed.tone[1..],&old.tone[1..]);
                assert_eq!(changed.decode,old.decode);
                assert_eq!(changed.params,old.params);
            }
            assert_eq!(grading::same(changed,old),percent==100 || i==2,"real Grade::same invalidates cached unchanged client buffers");
            assert_eq!(surface(d.clone()).transform(SceneEncoding::SDR),d.transform(RenderIntent::Relative,SceneEncoding::SDR),"SDR output receives no HDR gain");
        }
        let mut generic_linear=ImageDescription::srgb();
        generic_linear.tf=TransferFunction::Named(tf::EXT_LINEAR);
        assert_eq!(surface(generic_linear.clone()).transform(scene),generic_linear.transform(RenderIntent::Relative,scene),"generic SDR linear is not Windows scRGB");
        assert!(SurfaceColor{description:None,alpha:AlphaMode::default()}.transform(scene).is_none(),"untagged surface unchanged");
    }
    set_content_gain_percent(100);
}

fn arr(values:&[f32]) -> String { values.iter().map(|x|format!("{x:.9e}f")).collect::<Vec<_>>().join(",") }
fn main() {
    println!("struct NativeCase {{ int kind,percent,decode; float params[4],matrix[9],luma[3],tone[4]; }};\nconst NativeCase nativeCases[]={{");
    for percent in [50,100,200,100] {
        anland_backend::hdr::set_content_gain_percent(percent);
        for (kind,d) in descriptions().into_iter().enumerate() {
            let t=surface(d).transform(scene()).unwrap();
            println!("{{{kind},{percent},{},{{{}}},{{{}}},{{{}}},{{{}}}}},",t.decode,arr(&t.params),arr(&t.matrix),arr(&t.luma),arr(&t.tone));
        }
    }
    println!("}};");
}
