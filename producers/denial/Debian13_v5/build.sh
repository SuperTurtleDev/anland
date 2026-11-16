#!/usr/bin/env bash
# Debian 13 ARM64: reuse the single Denial engine/AOT build, emit native .debs.
# Usage: ANLAND_WITH_DEPS=1 JOBS=4 ANLAND_INSTALL=0 bash build.sh [--noconfirm]
# No installation or graphical startup unless separately and explicitly requested.
debian_preflight() {
    local ID VERSION_ID
    source /etc/os-release || die 'cannot identify distribution'
    [[ $ID == debian && $VERSION_ID == 13 && $(dpkg --print-architecture) == arm64 ]] || die 'requires Debian 13 ARM64'
    [[ $(id -u) != 0 ]] || die 'compile as an ordinary user'
    [[ $JOBS =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
    case $ANLAND_STAGE in check|prepare|build) ;; *) die 'ANLAND_STAGE must be check, prepare or build' ;; esac
    for flag in ANLAND_INSTALL ANLAND_WITH_DEPS ANLAND_OFFLINE; do
        [[ ${!flag:-0} == 0 || ${!flag:-0} == 1 ]] || die "$flag must be 0 or 1"
    done
}
debian_dependencies() {
    if [[ ${ANLAND_WITH_DEPS:-0} == 1 ]]; then
        [[ ${ANLAND_OFFLINE:-0} == 0 ]] || die 'dependency installation requires network'
        # Follow kde/Debian13_v5: enable native deb-src, then use apt source/build-dep.
        if ! grep -rqsE '^Types:.*deb-src|^deb-src[[:space:]]' \
                /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
            if [[ -f /etc/apt/sources.list.d/debian.sources ]]; then
                run sudo sed -i 's/^Types:[[:space:]]*deb[[:space:]]*$/Types: deb deb-src/' \
                    /etc/apt/sources.list.d/debian.sources
            elif [[ -f /etc/apt/sources.list ]]; then
                run sudo sed -i 's/^deb \(.*\)$/deb \1\ndeb-src \1/' /etc/apt/sources.list
            else
                die 'enable Debian 13 deb-src repositories before installing build dependencies'
            fi
        fi
        run sudo apt-get update
        run sudo apt-get install -y --no-install-recommends "${build_deps[@]}"
        run sudo apt-get build-dep -y --no-install-recommends xwayland
    fi
    # polkitd supplies runtime .so.0 libraries, not the .so linker inputs.
    # Check before the engine/AOT builds, including offline/manual-dependency runs.
    if [[ ${ANLAND_WITH_DE:-1} == 1 ]]; then
        run pkg-config --print-errors --exists polkit-agent-1 polkit-gobject-1
    fi
}

XWAYLAND_TREE= XWAYLAND_VERSION=
prepare_xwayland() {
    local out="$WORKDIR/xwayland" patch="$SCRIPT_DIR/xwayland.patch" result
    run mkdir -p "$out" "$ARTIFACTS_DIR"
    result=$(anland_prepare_latest_apt_source xwayland "$out" "$patch" "$ANLAND_OFFLINE") || die 'cannot prepare latest Debian Xwayland source'
    IFS=$'\t' read -r XWAYLAND_TREE XWAYLAND_VERSION <<< "$result"
    [[ $(dpkg-parsechangelog -l "$XWAYLAND_TREE/debian/changelog" -SVersion) == "$XWAYLAND_VERSION" ]] || die 'Xwayland changelog differs from fetched source version'
    grep -Fq 'No usable linux-dmabuf main device' "$XWAYLAND_TREE/hw/xwayland/xwayland-glamor-gbm.c" || die 'Xwayland patch sentinel is missing'
}

build_xwayland() {
    prepare_xwayland
    local out="$WORKDIR/xwayland" version tree produced architecture changes marker
    version=$XWAYLAND_VERSION
    tree=$XWAYLAND_TREE
    # dpkg-buildpackage writes packages beside the prepared source tree.
    out=$(dirname -- "$tree") || die 'cannot resolve Xwayland output directory'
    architecture=$(dpkg --print-architecture) || die 'cannot identify package architecture'
    changes="$out/xwayland_${version#*:}_${architecture}.changes"
    marker=$(mktemp "$out/.package-start.XXXXXX") || die 'cannot mark Xwayland build start'
    # The distro Xwayland must never inherit the WM's private SDK/library paths.
    (cd "$tree" && env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u CMAKE_PREFIX_PATH \
        -u LD_LIBRARY_PATH -u DESTDIR -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        PATH=/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        DEB_BUILD_OPTIONS="nocheck nodwz noautodbgsym parallel=$JOBS" \
        dpkg-buildpackage -b -uc -us) || die 'Xwayland build failed'
    [[ -s $changes && $changes -nt $marker ]] || die 'no current Xwayland .changes produced'
    produced=$(python3 - "$changes" "$version" "$architecture" "$marker" <<'PY_XWAYLAND_OUTPUT'
import hashlib
from pathlib import Path
import re
import subprocess
import sys

changes, version, architecture, marker = sys.argv[1:]
changes = Path(changes)
fields = {}
key = None
for line in changes.read_text().splitlines():
    if not line:
        continue
    if line[:1].isspace():
        if key is None:
            raise SystemExit('invalid .changes continuation')
        fields[key] += '\n' + line.strip()
    else:
        key, separator, value = line.partition(':')
        if not separator or key in fields:
            raise SystemExit('invalid or duplicate .changes field')
        fields[key] = value.strip()
if (fields.get('Source', '').split() != ['xwayland']
        or fields.get('Version') != version
        or architecture not in fields.get('Architecture', '').split()):
    raise SystemExit('unexpected Xwayland .changes identity')
started = Path(marker).stat().st_mtime_ns
selected = []
for entry in fields.get('Checksums-Sha256', '').splitlines():
    if not entry.strip():
        continue
    columns = entry.split()
    if len(columns) != 3:
        raise SystemExit('invalid .changes SHA256 entry')
    digest, size, name = columns
    if (not re.fullmatch(r'[0-9a-f]{64}', digest) or not size.isdecimal()
            or Path(name).name != name or name in ('.', '..')):
        raise SystemExit('unsafe .changes package entry')
    if not name.endswith('.deb'):
        continue
    package = changes.parent / name
    metadata = [subprocess.check_output(
        ['dpkg-deb', '-f', str(package), field], text=True).strip()
        for field in ('Package', 'Version', 'Architecture')]
    if metadata[0] != 'xwayland':
        continue
    if metadata[1:] != [version, architecture]:
        raise SystemExit('unexpected Xwayland package version or architecture')
    status = package.stat()
    if status.st_mtime_ns <= started or status.st_size != int(size) or not status.st_size:
        raise SystemExit('stale or incomplete Xwayland package')
    actual = hashlib.sha256()
    with package.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            actual.update(chunk)
    if actual.hexdigest() != digest:
        raise SystemExit('Xwayland package checksum differs from .changes')
    selected.append(package)
if len(selected) != 1:
    raise SystemExit('expected exactly one current Xwayland package')
print(selected[0])
PY_XWAYLAND_OUTPUT
    ) || die 'cannot select verified Xwayland package from .changes'
    run dpkg-deb --contents "$produced" >/dev/null
    run rm -f -- "$marker"
    xwayland_deb="$ARTIFACTS_DIR/${produced##*/}"
    publish_native_package "$produced" "$xwayland_deb"
}


prepare_debian_rust() {
    # Read the current Denial source's Rust toolchain, not Debian's rustc 1.85.
    export RUSTUP_HOME="$WORKDIR/rustup"
    RUSTUP_TOOLCHAIN=$(python3 - "$SOURCE/rust-toolchain.toml" <<'PY_RUST'
import sys, tomllib
with open(sys.argv[1], 'rb') as f:
    print(tomllib.load(f)['toolchain']['channel'])
PY_RUST
    ) || die 'cannot read locked Rust toolchain'
    export RUSTUP_TOOLCHAIN
    command -v rustup >/dev/null || die 'rustup required (ANLAND_WITH_DEPS=1)'
    if ! rustup run "$RUSTUP_TOOLCHAIN" rustc --version >/dev/null 2>&1; then
        [[ $ANLAND_OFFLINE == 0 ]] || die 'offline locked Rust toolchain missing'
        run rustup toolchain install "$RUSTUP_TOOLCHAIN" --profile minimal --no-self-update
    fi
    local cargo_binary
    cargo_binary=$(rustup which --toolchain "$RUSTUP_TOOLCHAIN" cargo) || die 'locked cargo is missing'
    export PATH="${cargo_binary%/*}:$PATH"
    run rustc --version
    run cargo --version
}
package_all() {
    log 'Package Denial native Debian .debs (no installation)'
    local pack="$WORKDIR/package" kind stage pkg version extra depends file package runtime app marker native_packages destination
    local -a kinds=(engine wm) packages=()
    [[ $ANLAND_WITH_DE == 0 ]] || kinds+=(de)
    local wm_version="${DENIAL_VERSION/-/+}-$ANLAND_PKGREL"
    run mkdir -p "$pack"
    native_packages=$(mktemp -d "$WORKDIR/package-output.XXXXXX") || die 'cannot stage native packages'
    # Reuse the existing package layout, licenses and full desktop integration,
    # not makepkg/pacman or a second Flutter/backend implementation.
    run cp "$ROOT/producers/denial/Arch_v5/PKGBUILD" "$SCRIPT_DIR/sources.conf" "$SCRIPT_DIR/startup.sh" "$pack/"
    printf '\nDENIAL_REV=%q\nDENIAL_VERSION=%q\nFLUTTER_REV=%q\nFLUTTER_VERSION=%q\n' \
        "$DENIAL_REV" "$DENIAL_VERSION" "$FLUTTER_REV" "$FLUTTER_VERSION" >> "$pack/sources.conf" || die 'cannot record selected sources'
    printf 'ENGINE_VERSION=%q\nENGINE_SHA256=%q\nENGINE_ICU_SHA256=%q\n' \
        "$ENGINE_VERSION" "$ENGINE_SHA256" "$ENGINE_ICU_SHA256" >> "$pack/sources.conf" || die 'cannot record selected engine'
    export DENIAL_SOURCE_TREE="$SOURCE" DENIAL_BUILD_DIR="$BUILD" DENIAL_ENGINE_DIR="$ENGINE"
    build_xwayland
    packages+=("$xwayland_deb")
    for kind in "${kinds[@]}"; do
        stage="$pack/$kind"
        run rm -rf -- "$stage"
        run mkdir -p "$stage"
        in_dir "$pack" env DENIAL_PACKAGE_KIND="$kind" DENIAL_STAGE="$stage" bash -c '
            set -euo pipefail
            startdir=$PWD
            pkgdir=$DENIAL_STAGE
            source "$startdir/PKGBUILD"
            package
        '
        case "$kind" in
            engine) pkg=denial-anland-engine; version=$ENGINE_VERSION; extra='libfontconfig1' ;;
            wm) pkg=denial; version=$wm_version
                extra="denial-anland-engine (= $ENGINE_VERSION), bash, coreutils, util-linux, procps, binutils, dbus, libpulse0, xkb-data, xwayland" ;;
            de) pkg=denial-desktop; version="${DENIAL_VERSION/-/+}-$ANLAND_DE_PKGREL"
                extra="denial (= $wm_version), denial-anland-engine (= $ENGINE_VERSION), bash, git, unzip, xz-utils, zlib1g, xdg-utils, zenity, polkitd, xwayland, xdg-desktop-portal, xdg-desktop-portal-gtk, xdg-desktop-portal-wlr" ;;
        esac
        run mkdir -p "$stage/DEBIAN" "$pack/metadata/debian"
        printf 'Source: %s\nSection: x11\nPriority: optional\nMaintainer: Anland <build@localhost>\n\nPackage: %s\nArchitecture: arm64\nDescription: Denial Anland Debian runtime\n' "$pkg" "$pkg" > "$pack/metadata/debian/control" || die 'cannot write dependency metadata'
        local -a shlibargs=()
        while IFS= read -r -d '' file; do
            [[ $(head -c4 "$file") == $'\x7fELF' ]] || continue
            readelf -d "$file" | grep -q '(NEEDED)' || continue
            shlibargs+=("-e$file")
        done < <(find "$stage/usr" -type f -print0)
        local libargs=("-l$BUILD/native/usr/lib" "-l$ENGINE")
        [[ $kind != de ]] || libargs+=("-l$pack/engine/usr/lib/denial-anland/flutter/lib" "-l$pack/wm/usr/lib/denial-anland/wm")
        depends=$(cd "$pack/metadata" && dpkg-shlibdeps --ignore-missing-info -O "${libargs[@]}" "${shlibargs[@]}") || die "cannot infer Debian dependencies: $pkg"
        depends=${depends#shlibs:Depends=}
        [[ -n $depends ]] || die "empty Debian dependencies: $pkg"
        printf 'Package: %s\nVersion: %s\nArchitecture: arm64\nSection: x11\nPriority: optional\nMaintainer: Anland <build@localhost>\nDepends: %s, %s\nDescription: Denial Anland Debian runtime (%s)\n Built from the selected source revision; does not start or restart desktop services.\n' "$pkg" "$version" "$depends" "$extra" "$kind" > "$stage/DEBIAN/control" || die 'cannot write Debian control'
        if [[ $kind == de ]]; then
            printf '/etc/xdg/xdg-desktop-portal-wlr/Denial\n' > "$stage/DEBIAN/conffiles" || die 'cannot record configuration'
        fi
        package="$native_packages/${pkg}_${version#*:}_arm64.deb"
        marker=$(mktemp "$WORKDIR/.denial-package-start.XXXXXX") || die 'cannot mark package build start'
        run dpkg-deb --root-owner-group -Zxz --build "$stage" "$package"
        [[ -s $package && $package -nt $marker ]] || die "package was not produced by this build: $package"
        [[ $(dpkg-deb -f "$package" Package) == "$pkg" &&
           $(dpkg-deb -f "$package" Version) == "$version" &&
           $(dpkg-deb -f "$package" Architecture) == arm64 ]] || die "wrong package metadata: $package"
        run dpkg-deb --contents "$package" >/dev/null
        run rm -f -- "$marker"
        destination="$ARTIFACTS_DIR/${package##*/}"
        publish_native_package "$package" "$destination"
        packages+=("$destination")
    done
    run rm -rf -- "$BUILD/package-check"
    run mkdir -p "$BUILD/package-check"
    for package in "${packages[@]}"; do
        [[ $(dpkg-deb -f "$package" Architecture) == arm64 ]] || die 'wrong package architecture'
        run dpkg-deb -x "$package" "$BUILD/package-check"
    done
    runtime="$BUILD/package-check/usr/lib/denial-anland"
    check_sha "$ENGINE_SHA256" "$runtime/flutter/lib/libflutter_engine.so"
    check_sha "$ENGINE_ICU_SHA256" "$runtime/flutter/data/icudtl.dat"
    check_elf "$runtime/flutter/lib/libflutter_engine.so"
    for file in "$runtime/wm/deniald" "$runtime/wm/denialctl" "$runtime/wm/"*.so "$runtime/wm/shell/lib/"*.so; do check_elf "$file"; done
    run "$runtime/wm/deniald" --help
    local reported
    reported=$("$runtime/wm/deniald" --version) || die 'version inspection failed'
    [[ $reported == *"$DENIAL_VERSION"* ]] || die 'stale Denial binary'
    if [[ $ANLAND_WITH_DE == 1 ]]; then
        for app in settings:denial-settings polkit:denial-app plugins:denial-plugin-manager; do
            check_elf "$runtime/desktop/${app%%:*}/lib/libapp.so"
            run "$runtime/desktop/${app%%:*}/${app#*:}" --bundle "$runtime/desktop/${app%%:*}" --engine "$runtime/flutter/lib/libflutter_engine.so" --check
        done
        run "$runtime/desktop/native/denial-plugins" --help
    fi
    record_package_outputs
    if [[ $ANLAND_INSTALL == 1 ]]; then run sudo dpkg -i "${packages[@]}"; fi
    log "Done. Debian .debs: $ARTIFACTS_DIR; no graphical test"
}
build_deps=(build-essential cmake meson ninja-build pkgconf ccache python3 curl ca-certificates git dpkg-dev debhelper fakeroot quilt patch xz-utils unzip zip file binutils libpipewire-0.3-dev rustup clang lld llvm jq gnupg dbus libpulse-dev libegl-dev libgles-dev libgbm-dev libdrm-dev libinput-dev libseat-dev libxkbcommon-dev libwayland-dev libudev-dev libpam0g-dev libfontconfig-dev libfreetype-dev libglib2.0-dev libunwind-dev libffi-dev zlib1g-dev libvulkan-dev xkb-data wayland-protocols)
if [[ ${ANLAND_WITH_DE:-1} == 1 ]]; then
    build_deps+=(libpolkit-agent-1-dev libpolkit-gobject-1-dev
                 xdg-utils zenity polkitd xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr)
fi
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    case ${1:-} in -h|--help) sed -n '2,4p' "$0"; exit 0 ;; esac
    HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
    export ANLAND_DENIAL_RECIPE_DIR="$HERE" ANLAND_DENIAL_DISTRO=debian13
    export ANLAND_STAGE=${ANLAND_STAGE:-build}
    export WORKDIR=${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/denial-anland-debian13}
    export ARTIFACTS_DIR=${ARTIFACTS_DIR:-$HERE/artifacts}
    exec bash "$HERE/../Arch_v5/build.sh" "$@"
fi
