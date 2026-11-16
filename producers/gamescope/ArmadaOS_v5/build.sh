#!/usr/bin/env bash
# Offline native RPM build. Prepare pinned sources and dependencies separately.
# CACHE defaults to ~/.cache/anland-gamescope-armada; JOBS defaults to 4.
# Requires CACHE/{source,recipe/terra-gamescope.spec,sdk/usr/lib64/pkgconfig}.
# Never installs packages or starts a graphical session.
# RPM adaptation and session launchers are generated here, not stored separately.
# Documentation: ../../../doc/UserManual/Gamescope-ArmadaOS.md
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
CACHE=${CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-gamescope-armada}
ARMADA_ROOT=${ARMADA_ROOT:-$HOME/armada}
JOBS=${JOBS:-4}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid JOBS' >&2; exit 1; }
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || { echo 'Requires ordinary aarch64 user' >&2; exit 1; }
for tool in rpmbuild rpmspec git tar patch python3 meson ninja flock sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
mkdir -p "$CACHE"
CACHE=$(cd -- "$CACHE" && pwd -P)
exec 9>"$CACHE/build.lock"
flock -n 9 || { echo 'Build already active' >&2; exit 1; }
export LANG=C.UTF-8 LC_ALL=C.UTF-8
export PKG_CONFIG_PATH="$CACHE/sdk/usr/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --exists xwayland libpipewire-0.3 gbm
SOURCE="$CACHE/source"
[[ $(git -C "$SOURCE" rev-parse HEAD) == 4246b87f49d6116d5f863131dcc401fc52840644 ]] || { echo 'Unexpected Gamescope revision' >&2; exit 1; }
assert_clean_tree() {
    local tree=$1
    if ! git -C "$tree" diff --quiet --exit-code ||
       ! git -C "$tree" diff --cached --quiet --exit-code ||
       [[ -n $(git -C "$tree" status --porcelain --untracked-files=all) ]]; then
        echo "Dirty source tree: $tree" >&2
        return 1
    fi
}
assert_clean_tree "$SOURCE"
modules=(src/reshade subprojects/libdisplay-info subprojects/libliftoff subprojects/vkroots subprojects/wlroots thirdparty/SPIRV-Headers)
for module in "${modules[@]}"; do
    expected=$(git -C "$SOURCE" rev-parse "HEAD:$module")
    [[ $(git -C "$SOURCE/$module" rev-parse HEAD) == "$expected" ]] || { echo "Submodule mismatch: $module" >&2; exit 1; }
    assert_clean_tree "$SOURCE/$module"
done
TOP="$CACHE/rpmbuild"
mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,BUILDROOT,RPMS,SRPMS}
cp "$CACHE/recipe/"*.patch "$CACHE/recipe/stb.pc" "$TOP/SOURCES/"
cp "$ARMADA_ROOT/packages/gamescope/patches/"*.patch "$TOP/SOURCES/"
# Anland's own source changes ride in gamescope.patch below, which is applied after
# the native Armada/Terra set and the wrap overlays.
cp "$HERE/gamescope.patch" "$TOP/SOURCES/anland.patch"
cp "$HERE/startup.sh" "$TOP/SOURCES/anland-startup.sh"
tar -chf "$TOP/SOURCES/anland-overlay.tar" -C "$HERE/gamescope" .
# Preserve Git version identity; exclude the unused OpenVR download and its objects.
tar -cf "$TOP/SOURCES/gamescope-source.tar" --exclude='./subprojects/openvr' --exclude='./.git/modules/subprojects/openvr' -C "$SOURCE" .
python3 - "$HERE" "$CACHE" "$ARMADA_ROOT" <<'PY'
from pathlib import Path
import re,sys

SOURCE_FILES = ("anland.patch", "anland-overlay.tar", "anland-startup.sh",
                "armada-anland-game-mode", "armada-anland-game-mode.service",
                "armada-anland-game-mode.desktop")


def adapt_spec(text):
    if "# BEGIN ANLAND ARMADA" in text:
        raise ValueError("spec already adapted; supply the original native spec")
    sections = {}
    for section in ("prep", "install"):
        matches = list(re.finditer(r"^%" + section + r"\s*$", text, re.M))
        if len(matches) != 1:
            raise ValueError(f"expected one %{section} section")
        start = matches[0].end()
        end = re.search(r"^%(?:conf|build|install|check|files|changelog|clean|generate_buildrequires|package|description)(?:\s|$)", text[start:], re.M)
        if not end:
            raise ValueError(f"cannot locate end of %{section}")
        sections[section] = start + end.start()
    if len(re.findall(r"^Release\s*:", text, re.M)) != 1:
        raise ValueError("expected one literal Release field")
    if "%meson" not in text:
        raise ValueError("requires a Meson Gamescope RPM recipe")
    names = re.findall(r"^Name\s*:\s*(\S+)\s*$", text, re.M)
    if len(names) != 1 or names[0] not in ("gamescope", "terra-gamescope"):
        raise ValueError("expected gamescope or terra-gamescope source package")
    if re.search(r"^%package.*anland-session", text, re.M):
        raise ValueError("anland-session subpackage already exists")

    used = [int(n or 0) for n in re.findall(r"^\s*Source(\d*)\s*:", text, re.M)]
    first = max(used, default=0) + 1
    numbers = dict(zip(SOURCE_FILES, range(first, first + len(SOURCE_FILES))))
    declared = "# BEGIN ANLAND ARMADA\n" + "".join(
        f"Source{number}: {name}\n" for name, number in numbers.items())
    declared += "BuildRequires: patch, tar, pkgconfig(libpipewire-0.3)\n"
    declared += "# END ANLAND ARMADA\n\n"

    prep = (
        "# Apply only after ALL native Armada/Terra patches and wrap overlays.\n"
        "patch --batch --forward --fuzz=0 --no-backup-if-mismatch -p1 < %{SOURCE" + str(numbers["anland.patch"]) + "}\n"
        "test ! -e src/Backends/anland\n"
        "mkdir -p src/Backends/anland\n"
        "tar xf %{SOURCE" + str(numbers["anland-overlay.tar"]) + "} -C src/Backends/anland\n"
        "# The overlay already carries the connector-only PollState and the\n"
        "# writable-target repaint behaviour; anland.patch owns the steamcompmgr side.\n\n"
    )
    install = (
        "install -Dm755 %{SOURCE" + str(numbers["anland-startup.sh"]) + "} %{buildroot}%{_libexecdir}/gamescope-anland/startup.sh\n"
        "install -Dm755 %{SOURCE" + str(numbers["armada-anland-game-mode"]) + "} %{buildroot}%{_bindir}/armada-anland-game-mode\n"
        "install -Dm644 %{SOURCE" + str(numbers["armada-anland-game-mode.service"]) + "} %{buildroot}%{_userunitdir}/armada-anland-game-mode.service\n"
        "install -Dm644 %{SOURCE" + str(numbers["armada-anland-game-mode.desktop"]) + "} %{buildroot}%{_datadir}/applications/armada-anland-game-mode.desktop\n\n"
    )
    for index, content in sorted(((sections["prep"], prep), (sections["install"], install)), reverse=True):
        text = text[:index] + content + text[index:]
    # Native EVR and epoch are preserved; only append our release component.
    text = re.sub(r"^(Release\s*:\s*)([^\n]+)$", lambda m: m[1] + m[2] + ".anland", text, count=1, flags=re.M)
    # Never give the replacement binary ambient scheduling capabilities by default.
    text = re.sub(r"^%caps\([^\n]+\)\s+(%\{_bindir\}/gamescope)\s*$", r"\1", text, flags=re.M)
    position = re.search(r"^%description(?:\s|$)", text, re.M)
    if position is None:
        raise ValueError("missing main description")
    text = text[:position.start()] + declared + text[position.start():]
    subpackage = """
%package anland-session
Summary: Direct Anland v5 game mode entry for ArmadaOS
Requires: %{name}%{?_isa} = %{?epoch:%{epoch}:}%{version}-%{release}
Requires: bash, coreutils, util-linux, procps-ng, xorg-x11-server-Xwayland
Requires: pipewire, wireplumber, dbus-broker

%description anland-session
Standalone Anland game mode for Armada/Fedora 44. Uses the installed ARM64
Steam client; does not download Steam, activate services or replace desktop
configuration. Exit an existing graphical session explicitly before launching.

"""
    position = re.search(r"^%prep\s*$", text, re.M)
    text = text[:position.start()] + subpackage + text[position.start():]
    files = """
%files anland-session
%{_libexecdir}/gamescope-anland/
%{_bindir}/armada-anland-game-mode
%{_userunitdir}/armada-anland-game-mode.service
%{_datadir}/applications/armada-anland-game-mode.desktop

"""
    position = re.search(r"^%changelog(?:\s|$)", text, re.M)
    if position is None:
        raise ValueError("missing changelog section")
    text = text[:position.start()] + files + text[position.start():]
    return text

here,cache,armada=map(Path,sys.argv[1:])
session_sources = {
    'armada-anland-game-mode': """#!/usr/bin/env bash
# Explicit standalone game-mode entry. Never enable services or stop a desktop.
set -euo pipefail
exec /usr/libexec/gamescope-anland/startup.sh "$@"
""",
    'armada-anland-game-mode.service': """[Unit]
Description=ArmadaOS Steam Big Picture on Gamescope Anland v5
After=pipewire.service wireplumber.service

[Service]
Type=simple
ExecStart=/usr/bin/armada-anland-game-mode
Restart=no
TimeoutStopSec=30
StandardOutput=journal
StandardError=journal
# Intentionally no Install section: explicit start only, no desktop takeover.
""",
    'armada-anland-game-mode.desktop': """[Desktop Entry]
Type=Application
Name=Armada Anland Game Mode
Name[zh_CN]=Armada Anland 游戏模式
Comment=Start a standalone Anland game session after exiting your desktop
Exec=/usr/bin/armada-anland-game-mode
TryExec=/usr/bin/armada-anland-game-mode
Icon=steam
Terminal=true
Categories=Game;
StartupNotify=false""",
}
for name, content in session_sources.items():
    (cache/'rpmbuild/SOURCES'/name).write_text(content)
s=(cache/'recipe/terra-gamescope.spec').read_text()
# Terra's Fedora build macros provide %{evr}; define the equivalent explicitly
# for the native rpmbuild used by this offline Armada build.
s=s.replace('= %{evr}', '= %{?epoch:%{epoch}:}%{version}-%{release}')
patches=sorted((armada/'packages/gamescope/patches').glob('[0-9][0-9][0-9][0-9]-*.patch'))
assert len(patches)==31, f'Unexpected Armada patch count: {len(patches)}'
# Armada's set is pinned by count on purpose: a different number means the
# submodule/recipe assumptions in this script changed. Anland's own changes are
# not declared as Patch entries at all; they live inside %{SOURCE1000} and are
# applied by %prep after %autopatch, so this file needs no edit to carry them.
pos=s.index('BuildRequires:')
s=s[:pos]+''.join(f'Patch: {p.name}\n' for p in patches)+'Source999: gamescope-source.tar\n'+s[pos:]
s=s.replace('Release:        1%{?dist}', 'Release:        1%{?dist}.armada')
assert s.count('%git_clone %{url} %{ver}')==1
s=s.replace('%git_clone %{url} %{ver}', '%setup -q -T -c -n gamescope\ntar xf %{SOURCE999}')
# SDK metadata is extracted privately to preserve the installed Anland Xwayland.
s=s.replace('BuildRequires:  pkgconfig(xwayland)', '# pkgconfig(xwayland): checked by build.sh using private SDK')
s=s.replace('%meson \\\n', '%meson --wrap-mode=nodownload -Denable_openvr_support=false \\\n')
s=s.replace('export PKG_CONFIG_PATH=pkgconfig', 'export PKG_CONFIG_PATH=pkgconfig:${PKG_CONFIG_PATH:-}')
s=s.replace('%files\n', '%check\n%meson_test\n\n%files\n', 1)
s=adapt_spec(s)
(cache/'rpmbuild/SPECS/terra-gamescope.spec').write_text(s)
PY
SPEC="$TOP/SPECS/terra-gamescope.spec"
rpmspec -P "$SPEC" > "$CACHE/expanded.spec"
{
    git -C "$SOURCE" rev-parse HEAD
    git -C "$SOURCE" submodule status -- "${modules[@]}"
    git -C "$HERE" rev-parse HEAD
    rpm -q gcc gcc-c++ meson ninja-build rpm-build
    sha256sum "$TOP/SOURCES/"* "$SPEC"
} > "$CACHE/build-inputs.txt"
# RPM performs native Terra + Armada patches, then Anland, before configuration.
touch "$CACHE/build-start"
rpmbuild -ba --without lld --define "_topdir $TOP" --define "_smp_mflags -j$JOBS" --define "_smp_build_ncpus $JOBS" "$SPEC"
# A failed build never reaches artifact collection.
OUT=$(mktemp -d "$CACHE/artifacts.XXXXXX")
mapfile -d '' packages < <(find "$TOP/RPMS" -type f -name '*.rpm' -newer "$CACHE/build-start" -print0)
[[ ${#packages[@]} -gt 0 ]] || { echo 'No binary RPMs produced' >&2; exit 1; }
for package in "${packages[@]}"; do
    rpm -K "$package"
    rpm -qp --qf '%{NAME} %{EVR} %{ARCH}\n' "$package"
    cp -p "$package" "$OUT/"
done
cp "$CACHE/build-inputs.txt" "$OUT/"
(cd "$OUT" && sha256sum ./*.rpm > SHA256SUMS && sha256sum -c SHA256SUMS)
printf 'RPM_BUILD_OK artifacts=%s\n' "$OUT"