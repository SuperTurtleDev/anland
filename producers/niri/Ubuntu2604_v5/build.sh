#!/usr/bin/env bash
# Ubuntu 26.04 ARM64: current Niri/Smithay + xwayland-satellite + patched Xwayland.
# Usage: ANLAND_WITH_DEPS=1 JOBS=4 ANLAND_INSTALL=0 bash build.sh
# Default Niri D-Bus/systemd/PipeWire features are retained; no graphical startup.
set -uo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd -P) || exit 1
source "$ROOT/producers/common/latest-source.sh"
WORKDIR=$(realpath -m -- "${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-niri-ubuntu2604}") || exit 1
ARTIFACTS_DIR=$(realpath -m -- "${ARTIFACTS_DIR:-$SCRIPT_DIR/artifacts}") || exit 1
JOBS=${JOBS:-4}
ANLAND_INSTALL=${ANLAND_INSTALL:-0}
ANLAND_WITH_DEPS=${ANLAND_WITH_DEPS:-0}
ANLAND_OFFLINE=${ANLAND_OFFLINE:-0}
log() { printf '\n==> %s\n' "$*"; }
die() { printf 'niri-build: %s\n' "$*" >&2; exit 1; }
run() { "$@" || die "failed: $*"; }
export LANG=C.UTF-8 LC_ALL=C.UTF-8 GIT_TERMINAL_PROMPT=0
export CARGO_HOME="$WORKDIR/cargo-home" CARGO_TARGET_DIR="$WORKDIR/cargo-target"
export ANLAND_NIRI_BRIDGE_BUILD="$WORKDIR/bridge"
ubuntu_preflight() {
    local ID VERSION_ID
    source /etc/os-release || die 'cannot identify distribution'
    [[ $ID == ubuntu && $VERSION_ID == 26.04 && $(dpkg --print-architecture) == arm64 ]] || die 'requires Ubuntu 26.04 ARM64'
    [[ $(id -u) != 0 ]] || die 'compile as an ordinary user'
    [[ $JOBS =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
    for flag in ANLAND_INSTALL ANLAND_WITH_DEPS ANLAND_OFFLINE; do
        [[ ${!flag:-0} == 0 || ${!flag:-0} == 1 ]] || die "$flag must be 0 or 1"
    done
}
ubuntu_dependencies() {
    [[ ${ANLAND_WITH_DEPS:-0} == 1 ]] || return 0
    [[ ${ANLAND_OFFLINE:-0} == 0 ]] || die 'dependency installation requires network'
    if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
        run sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
    fi
    run sudo apt-get update
    run sudo apt-get install -y --no-install-recommends "${build_deps[@]}"
    run sudo apt-get build-dep -y --no-install-recommends xwayland
}

build_xwayland() {
    local out="$WORKDIR/xwayland" patch="$SCRIPT_DIR/xwayland.patch"
    local version tree result arch changes marker listing file name package_arch found=0
    run mkdir -p "$out" "$ARTIFACTS_DIR"
    result=$(anland_prepare_latest_apt_source xwayland "$out" "$patch" "$ANLAND_OFFLINE") || die 'cannot prepare latest Ubuntu Xwayland source'
    IFS=$'\t' read -r tree version <<< "$result"
    # dpkg-buildpackage writes packages beside the prepared source tree.
    out=$(dirname -- "$tree") || die 'cannot resolve Xwayland output directory'
    arch=$(dpkg --print-architecture) || die 'cannot read build architecture'
    changes="$out/xwayland_${version#*:}_${arch}.changes"
    marker=$(mktemp "$out/.build-start.XXXXXX") || die 'cannot create Xwayland marker'
    # The distro Xwayland must never inherit the WM's private SDK/library paths.
    (cd "$tree" && env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u CMAKE_PREFIX_PATH \
        -u LD_LIBRARY_PATH -u DESTDIR -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        PATH=/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        DEB_BUILD_OPTIONS="nocheck nodwz noautodbgsym parallel=$JOBS" \
        dpkg-buildpackage -b -uc -us) || die 'Xwayland build failed'
    [[ -s $changes && $changes -nt $marker ]] || die "missing or stale changes: $changes"
    listing=$(awk '/^Files:/ {in_files=1; next} in_files && /^[^ ]/ {exit} in_files && $5 ~ /\.deb$/ {print $5}' "$changes") || die 'cannot read Xwayland package list'
    xwayland_debs=()
    while IFS= read -r file; do
        [[ $file == *.deb && $file != */* ]] || die "invalid package filename: $file"
        file="$out/$file"
        [[ -s $file && $file -nt $marker ]] || die "missing or stale package: $file"
        name=$(dpkg-deb -f "$file" Package) || die "cannot read package name: $file"
        [[ -n $name ]] || die "missing package name: $file"
        case $name in *-debug|*-dbg|*-dbgsym) continue ;; esac
        package_arch=$(dpkg-deb -f "$file" Architecture) || die "cannot read package architecture: $file"
        [[ $package_arch == "$arch" || $package_arch == all ]] || die "wrong package architecture: $file"
        [[ $(dpkg-deb -f "$file" Version) == "$version" ]] || die "wrong package version: $file"
        run dpkg-deb --contents "$file" >/dev/null
        run cp -p "$file" "$ARTIFACTS_DIR/"
        xwayland_debs+=("$ARTIFACTS_DIR/${file##*/}")
        [[ $name != xwayland ]] || found=1
    done <<<"$listing"
    [[ $found == 1 ]] || die 'no Xwayland runtime package produced'
    run rm -f -- "$marker"
}

prepare_sources() {
    local name directory archive sha url file tree identity
    run mkdir -p "$WORKDIR/downloads" "$WORKDIR/src"
    identity=$(python3 - "$SCRIPT_DIR" "$ROOT" <<'PY_IDENTITY'
import hashlib, pathlib, sys
recipe, root = map(pathlib.Path, sys.argv[1:])
h = hashlib.sha256((recipe / 'sources.json').read_bytes())
for name in ('niri.patch', 'smithay.patch'):
    h.update((recipe / name).read_bytes())
for rel in ('producers/niri/anland_backend', 'libdisplay_producer', 'common'):
    folder = root / rel
    for p in sorted(folder.rglob('*')):
        if p.is_file():
            h.update(p.relative_to(root).as_posix().encode() + b'\0' + p.read_bytes())
print(h.hexdigest())
PY_IDENTITY
    ) || die 'source identity failed'
    for name in niri smithay satellite; do
        url=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]]["url"])' "$SCRIPT_DIR/sources.json" "$name") || die "source URL: $name"
        local patch=""
        if [[ $name == niri || $name == smithay ]]; then
            patch="$SCRIPT_DIR/$name.patch"
            local patchsha
            patchsha=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["patches"][sys.argv[2]+".patch"])' "$SCRIPT_DIR/sources.json" "$name") || die "patch checksum: $name"
            printf '%s  %s\n' "$patchsha" "$patch" | sha256sum -c --status - || die "patch checksum: $name"
        fi
        local result commit
        revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]].get("revision", ""))' "$SCRIPT_DIR/sources.json" "$name") || die "source revision: $name"
        result=$(anland_prepare_latest_git_source "$name" "$url" "$WORKDIR" "$patch" "$ANLAND_OFFLINE" "$identity" shallow "$revision") || die "cannot prepare $name source"
        IFS=$'\t' read -r tree commit <<< "$result"
        source_tree[$name]=$tree
        if [[ $name == niri && ! -f $tree/.anland-niri-overlay-id ]]; then
            run mkdir -p "$tree/src/backend/anland"
            run cp "$SCRIPT_DIR/niri/"*.rs "$tree/src/backend/anland/"
            printf '%s\n' "$identity" > "$tree/.anland-niri-overlay-id" || die 'cannot record Niri overlay'
        fi
    done
    [[ ! -e $WORKDIR/src/smithay-latest || -L $WORKDIR/src/smithay-latest ]] || die 'preserving non-managed Smithay path'
    run ln -sfn "${source_tree[smithay]}" "$WORKDIR/src/smithay-latest"
}
declare -A source_tree=()
cargo_package_version() {
    python3 - "$1" <<'PY_VERSION'
import sys, tomllib
with open(sys.argv[1], 'rb') as f: data = tomllib.load(f)
version = data.get('package', {}).get('version')
if not isinstance(version, str): version = data.get('workspace', {}).get('package', {}).get('version')
if not isinstance(version, str): raise SystemExit('Cargo package version is missing')
print(version)
PY_VERSION
}
package_runtime() {
    local name=$1 binary=$2 version=$3 tree=$4 stage="$WORKDIR/package/$1" file depends extra
    run rm -rf -- "$stage"
    run mkdir -p "$stage/usr/bin" "$stage/usr/share/doc/$name" "$stage/DEBIAN" "$WORKDIR/metadata/debian"
    run install -m755 "$binary" "$stage/usr/bin/$name"
    local -a shlibargs=("-e$stage/usr/bin/$name") libargs=()
    extra='xwayland, xkb-data'
    if [[ $name == niri-anland ]]; then
        run mkdir -p "$stage/usr/lib/niri-anland"
        run install -m755 "$WORKDIR/bridge/libanland_niri_bridge.so" "$WORKDIR/bridge/common/libdisplay_producer/libdisplay_producer.so" "$stage/usr/lib/niri-anland/"
        run install -m755 "$SCRIPT_DIR/startup.sh" "$stage/usr/bin/niri-anland-start"
        run install -m644 "$tree/resources/default-config.kdl" "$stage/usr/share/doc/$name/default-config.kdl"
        for file in "$stage/usr/lib/niri-anland/"*.so; do shlibargs+=("-e$file"); done
        libargs=("-l$stage/usr/lib/niri-anland")
        extra+=' , xwayland-satellite, bash, procps, util-linux, dbus'
    fi
    run install -m644 "$tree/LICENSE" "$stage/usr/share/doc/$name/copyright"
    if [[ $name == niri-anland ]]; then run cat "$ROOT/LICENSE" >> "$stage/usr/share/doc/$name/copyright"; fi
    printf 'Source: %s\nSection: x11\nPriority: optional\nMaintainer: Anland <build@localhost>\n\nPackage: %s\nArchitecture: arm64\nDescription: Anland Ubuntu runtime\n' "$name" "$name" > "$WORKDIR/metadata/debian/control" || die 'cannot write dependency metadata'
    depends=$(cd "$WORKDIR/metadata" && dpkg-shlibdeps --ignore-missing-info -O "${libargs[@]}" "${shlibargs[@]}") || die 'runtime dependency inference failed'
    depends=${depends#shlibs:Depends=}
    [[ -n $depends ]] || die 'empty runtime dependencies'
    printf 'Package: %s\nVersion: %s\nArchitecture: arm64\nSection: x11\nPriority: optional\nMaintainer: Anland <build@localhost>\nDepends: %s, %s\nDescription: %s for Anland on Ubuntu 26.04\n Native ARM64 build; no automatic service or graphical session changes.\n' "$name" "$version" "$depends" "$extra" "$name" > "$stage/DEBIAN/control" || die 'cannot write package control'
    if [[ $name == niri-anland ]]; then
        printf 'Recommends: xdg-desktop-portal-gnome\n' >> "$stage/DEBIAN/control" || die 'cannot write portal recommendation'
    fi
    local package="$ARTIFACTS_DIR/${name}_${version}_arm64.deb" marker
    marker=$(mktemp "$WORKDIR/.package-start.XXXXXX") || die 'cannot create package marker'
    run dpkg-deb --root-owner-group -Zxz --build "$stage" "$package"
    [[ -s $package && $package -nt $marker &&
       $(dpkg-deb -f "$package" Package) == "$name" &&
       $(dpkg-deb -f "$package" Version) == "$version" &&
       $(dpkg-deb -f "$package" Architecture) == arm64 ]] || die "invalid or stale runtime package: $package"
    run rm -f -- "$marker"
    packages+=("${package##*/}")
}
main() {
    [[ $# == 0 ]] || die 'use environment variables, not arguments'
    ubuntu_preflight
    [[ $WORKDIR != / && $WORKDIR != "$HOME" && $WORKDIR != "$ROOT" && $WORKDIR != "$ROOT/"* ]] || die 'WORKDIR must be a separate cache directory'
    run mkdir -p "$WORKDIR" "$ARTIFACTS_DIR"
    exec 8>"$WORKDIR/build.lock" || die 'cannot open build lock'
    flock -n 8 || die 'another builder owns WORKDIR'
    exec 9>"$ARTIFACTS_DIR/.build.lock" || die 'cannot open artifact lock'
    flock -n 9 || die 'another builder owns artifacts'
    ubuntu_dependencies
    prepare_sources
    local tree="${source_tree[niri]}" satellite="${source_tree[satellite]}"
    local -a fetch=(--locked --target aarch64-unknown-linux-gnu)
    [[ $ANLAND_OFFLINE == 0 ]] || fetch+=(--offline)
    for folder in "$tree" "$satellite"; do
        (cd "$folder" && cargo fetch "${fetch[@]}") || die 'locked Cargo fetch failed'
    done
    run cmake -S "$ROOT/producers/niri/anland_backend" -B "$WORKDIR/bridge" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN'
    run cmake --build "$WORKDIR/bridge" --target anland_niri_bridge --parallel "$JOBS"
    (cd "$tree" && RUSTFLAGS="${RUSTFLAGS:-} -C link-arg=-Wl,-rpath,\$ORIGIN/../lib/niri-anland" \
        cargo build --release --locked --offline --jobs "$JOBS") || die 'Niri compilation failed'
    (cd "$satellite" && cargo build --release --locked --offline --features systemd --jobs "$JOBS") || die 'xwayland-satellite compilation failed'
    run env LD_LIBRARY_PATH="$WORKDIR/bridge:$WORKDIR/bridge/common/libdisplay_producer" "$CARGO_TARGET_DIR/release/niri" --version
    local version satellite_version
    version="$(cargo_package_version "$tree/Cargo.toml")+anland5-1" || die 'Niri version missing'
    satellite_version="$(cargo_package_version "$satellite/Cargo.toml")+anland5-1" || die 'satellite version missing'
    package_runtime xwayland-satellite "$CARGO_TARGET_DIR/release/xwayland-satellite" "$satellite_version" "$satellite"
    package_runtime niri-anland "$CARGO_TARGET_DIR/release/niri" "$version" "$tree"
    build_xwayland
    local file
    for file in "${xwayland_debs[@]}"; do packages+=("${file##*/}"); done
    run mkdir -p "$WORKDIR/package-check"
    local package
    for package in "${packages[@]}"; do run dpkg-deb -x "$ARTIFACTS_DIR/$package" "$WORKDIR/package-check"; done
    local executable="$WORKDIR/package-check/usr/bin/niri-anland" linkage
    linkage=$(env -u LD_LIBRARY_PATH ldd "$executable") || die 'package ldd failed'
    [[ $linkage != *'not found'* ]] || die "packaged library missing: $linkage"
    run "$executable" --version
    (cd "$ARTIFACTS_DIR" && sha256sum "${packages[@]}") || die 'cannot hash packages'
    if [[ $ANLAND_INSTALL == 1 ]]; then
        local -a files=()
        for package in "${packages[@]}"; do files+=("$ARTIFACTS_DIR/$package"); done
        run sudo dpkg -i "${files[@]}"
    fi
    log "Done. Ubuntu .debs: $ARTIFACTS_DIR; no graphical test"
}
build_deps=(build-essential cmake meson ninja-build pkgconf ccache python3 curl ca-certificates git dpkg-dev debhelper fakeroot quilt patch xz-utils unzip zip file binutils libpipewire-0.3-dev cargo rustc clang libclang-dev libinput-dev libudev-dev libseat-dev libdisplay-info-dev libgbm-dev libegl-dev libgles-dev libxkbcommon-dev libwayland-dev libpango1.0-dev libcairo2-dev libpixman-1-dev libfontconfig-dev libxcb1-dev libxcb-composite0-dev libxcb-randr0-dev libxcb-res0-dev libxcb-cursor-dev libx11-xcb-dev libdrm-dev wayland-protocols xkb-data)
packages=()
xwayland_debs=()
main "$@"
