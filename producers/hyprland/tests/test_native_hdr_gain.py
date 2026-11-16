#!/usr/bin/env python3
"""Compile upstream CM GLSL unmodified; run the real gain helper on GLES.
Usage: test_native_hdr_gain.py /path/to/native/hyprland
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1])
shaders = source / 'src/render/shaders/glsl'

def expand(path):
    text = path.read_text()
    return re.sub(r'^#include "([^"]+)".*$', lambda m: '' if m[1] == 'defines.h' else expand(shaders/m[1]), text, flags=re.M)

cm = expand(shaders/'cm_helpers.glsl')
values = (source/'src/helpers/cm/ColorManagement.hpp').read_text()
enum = re.search(r'enum eTransferFunction[^\{]*\{(.*?)\};', values, re.S)[1]
defs = '\n'.join(f'#define {name} {value}' for name,value in re.findall(r'(CM_TRANSFER_FUNCTION_\w+)\s*=\s*(\d+)',enum))
new = 'float linearNoise' in cm
args = ', 0.0' if new else ''
mode = ', uMode' if 'int tonemapMode' in cm else ''
fragment = f'''#version 300 es
precision highp float;
precision highp sampler3D;
#define ALLOW_INCLUDES 1
#define USE_TONEMAP 1
#define USE_SDR_MOD 0
#define USE_ICC 0
#define USE_MIRROR 0
#define USE_ALT_TONEMAP 0
{defs}
{cm}
uniform vec4 uPixel;
uniform mat3 uMatrix;
uniform int uTF;
uniform int uMode;
uniform vec2 uRange;
uniform float uMax;
uniform float uPeak;
out vec4 fragColor;
void main() {{
    fragColor = doColorManagement(uPixel, 1.0, uTF, CM_TRANSFER_FUNCTION_LINEAR, uMatrix,
                                 uRange, vec2(0.0,1000.0){args}, mat3(1.0), uMax, uPeak, 203.0, 203.0{mode});
}}
'''
vertex = '''#version 300 es
void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}
'''
here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='anland-hyprland-hdr-gain-') as d:
    d=Path(d)
    header='\n'.join(f'const char *{name}=R"shader({body})shader";' for name,body in [('vertex',vertex),('fragment',fragment)])
    cpp=d/'test.cpp'; binary=d/'test'
    tfs = dict(re.findall(r'(CM_TRANSFER_FUNCTION_\w+)\s*=\s*(\d+)', enum))
    ints = f"constexpr int pqTF={tfs['CM_TRANSFER_FUNCTION_ST2084_PQ']},scRGBTF={tfs['CM_TRANSFER_FUNCTION_EXT_LINEAR']},sdrTF={tfs['CM_TRANSFER_FUNCTION_SRGB']};\n"
    cpp.write_text(header+'\n'+ints+(here/'native_hdr_gain.cpp').read_text())
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','egl','glesv2'],text=True).split()
    subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+str(here),str(cpp),'-o',str(binary),*flags],check=True)
    subprocess.run([str(binary)],env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1'),check=True)
