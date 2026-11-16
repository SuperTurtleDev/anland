#!/usr/bin/env python3
"""Run real SurfaceColor/helper/cache comparisons, then native GLSL on GLES.
Usage: test_hdr_gain.py /path/to/patched/pinned/denial
Output-color structs are type-only fixtures; no output behavior is simulated.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from hdr_shader_source import load_native_shaders

here=Path(__file__).resolve().parent
source=Path(sys.argv[1]).resolve()/'compositor/src/bin/deniald'
production=here.parent/'anland_backend/hdr.rs'
hdr=production.read_text()
gain=hdr[hdr.index('use std::sync::atomic'):hdr.index('pub(super) fn output(')]
gl, shaders=load_native_shaders(source/'flutter_runtime/renderer/gl.rs')
handler=(source/'flutter_runtime/renderer/handler.rs').read_text()
grade=handler[handler.index('#[derive(Clone)]\nstruct Grade'):handler.index('\nstruct GradedTexture')]
conversion=gl[gl.index('#[derive(Clone, Copy, Debug, Default, PartialEq)]\npub(super) struct Conversion'):gl.index('\n}',gl.index('pub(super) struct Conversion'))+2]

with tempfile.TemporaryDirectory(prefix='anland-denial-gain-') as d:
    d=Path(d)
    # Only unused HdrOutput data declarations are needed by the complete native
    # color_space.rs module. The gain function and Grade methods are verbatim.
    fixture='''#![allow(dead_code)]
mod output_color {
    #[derive(Clone, Copy)] pub enum SignalPrimaries { Bt2020, Srgb }
    #[derive(Clone, Copy)] pub struct Display { pub primaries: crate::color_space::Chromaticities, pub min_luminance:f64, pub max_luminance:f64, pub max_frame_average:f64 }
    #[derive(Clone, Copy)] pub struct HdrOutput { pub display:Display, pub primaries:SignalPrimaries, pub white:f64 }
    pub struct Lut;
}
'''
    fixture+=f'#[path={json.dumps(str(source/"color_space.rs"),ensure_ascii=False)}] mod color_space;\n'
    fixture+='mod anland_backend { pub mod hdr {\n'+gain+'\n}}\n'
    fixture+='mod grading { use std::sync::Arc;\n'+conversion+'\n'+grade+'''
    pub fn same(a:crate::color_space::SurfaceTransform,b:crate::color_space::SurfaceTransform)->bool {
        Grade{transform:Some(a),alpha:Default::default(),look:None}.same(&Grade{transform:Some(b),alpha:Default::default(),look:None})
    }
}
'''
    fixture+=(here/'hdr_gain_surface.rs').read_text()
    rust=d/'test.rs';rust.write_text(fixture)
    base=['rustc','--edition=2024','--cfg','feature="anland"',str(rust)]
    subprocess.run(base+['--test','-o',str(d/'rust-tests')],check=True)
    subprocess.run([str(d/'rust-tests'),'--test-threads=1'],check=True)
    subprocess.run(base+['-o',str(d/'cases')],check=True)
    cases=subprocess.check_output([str(d/'cases')],text=True)
    header='\n'.join(f'const char *{kind}=R"shader({shaders[name]})shader";' for kind,name in [('vertex','SCANOUT_VERTEX_SHADER'),('fragment','SCANOUT_FRAGMENT_SHADER')])
    cpp=d/'gpu.cpp';cpp.write_text(header+'\n'+cases+'\n'+(here/'native_hdr_gain.cpp').read_text())
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','egl','glesv2'],text=True).split()
    subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(d/'gpu'),*flags],check=True)
    subprocess.run([str(d/'gpu')],env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1'),check=True)
