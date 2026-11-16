"""Read native shaders from a Denial tree with the Anland precision patch."""
import codecs
from pathlib import Path
import re


def load_native_shaders(path: Path):
    rust = path.read_text()
    shaders = {}
    for name in ("SCANOUT_VERTEX_SHADER", "SCANOUT_FRAGMENT_SHADER"):
        match = re.search(rf'const {name}: &\[u8\] = b"(.*?)";', rust, re.S)
        if not match:
            raise SystemExit(f"{path}: missing native shader {name}")
        shaders[name] = codecs.decode(
            match[1].replace("\\\n", ""), "unicode_escape"
        ).rstrip("\0")
    if not re.search(r'\buniform\s+highp\s+sampler2D\s+source_texture\s*;',
                     shaders["SCANOUT_FRAGMENT_SHADER"]):
        raise SystemExit(
            f"{path}: Anland's highp source-sampler patch is missing. "
            "Apply the matching producers/denial/<distribution>/denial.patch "
            "before testing; raw upstream shaders are not the Anland renderer."
        )
    return rust, shaders
