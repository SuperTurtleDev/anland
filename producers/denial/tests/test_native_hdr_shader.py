#!/usr/bin/env python3
"""Run the pinned Denial scanout shader on surfaceless GLES, without Flutter.

Usage: test_native_hdr_shader.py /path/to/patched/denial/source
The shader is extracted from upstream source, never reimplemented by the test.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from hdr_shader_source import load_native_shaders

source = Path(sys.argv[1]) / "compositor/src/bin/deniald/flutter_runtime/renderer/gl.rs"
_, shaders = load_native_shaders(source)

header = '\n'.join(
    f'const char *{kind} = R"shader({shaders[name]})shader";'
    for kind, name in (("vertex", "SCANOUT_VERTEX_SHADER"),
                       ("fragment", "SCANOUT_FRAGMENT_SHADER"))
)
test = Path(__file__).with_name("native_hdr_shader.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="anland-denial-hdr-gpu-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(header + '\n' + test)
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "egl", "glesv2"], text=True).split()
    subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary), *flags], check=True)
    environment = dict(os.environ, EGL_PLATFORM="surfaceless", LIBGL_ALWAYS_SOFTWARE="1")
    subprocess.run([str(binary)], env=environment, check=True)
