#!/bin/bash
# Ubuntu 26.04 arm64: build current Hyprland + Aquamarine + Hyprgrass + patched Xwayland .debs.
# Usage: [ANLAND_INSTALL=1] [WORKDIR=...] [JOBS=4] bash build.sh
# Patch overrides: HYPRLAND_PATCH, AQUAMARINE_PATCH, HYPRGRASS_PATCH, XWAYLAND_PATCH.
# Intentional overrides require matching lock-file checksums.
# Prepared trees without .anland-source-identity need an explicit refresh; never auto-delete.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
source "$ROOT/producers/common/latest-source.sh"
BACKEND="$SCRIPT_DIR/../anland_backend"
WORKDIR="${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-hyprland-ubuntu2604}"
JOBS="${JOBS:-4}"
SYSROOT="$WORKDIR/sysroot"
ARTIFACTS_DIR=${ARTIFACTS_DIR:-$SCRIPT_DIR/artifacts}
ARTIFACTS="$ARTIFACTS_DIR"
[ "$(id -u)" -eq 0 ] && SUDO="" || SUDO="sudo"
export DEB_BUILD_OPTIONS="nocheck nodwz parallel=$JOBS"
export CCACHE_DIR="$WORKDIR/ccache" CCACHE_BASEDIR="$WORKDIR"
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CMAKE_PREFIX_PATH LD_LIBRARY_PATH DESTDIR
export PKG_CONFIG_DISABLE_UNINSTALLED=1

ubuntu_build_environment() {
    # Minimal Ubuntu images gain dpkg-architecture only when dpkg-dev is installed.
    # Resolve paths after ubuntu_dependencies, never cache an empty multiarch value.
    command -v dpkg-architecture >/dev/null || die 'dpkg-dev is required (ANLAND_WITH_DEPS=1)'
    ARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH) || die 'cannot determine Debian host multiarch'
    [[ $ARCH == aarch64-linux-gnu ]] || die "unexpected ARM64 multiarch: $ARCH"
    LIBDIR="$SYSROOT/usr/lib/$ARCH"
    export PKG_CONFIG_LIBDIR="/usr/lib/$ARCH/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
    # Aquamarine links OpenGL::OpenGL as well as EGL; libGL.so alone is insufficient.
    run build_env pkg-config --print-errors --exists pixman-1 opengl egl glesv2
    log "Build environment: multiarch=$ARCH; PKG_CONFIG_LIBDIR=$PKG_CONFIG_LIBDIR"
}

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
    # Only source packages come from Debian; every binary is built on Ubuntu.
    run sudo apt-get install -y --no-install-recommends debian-archive-keyring
    printf '%s\n' 'deb-src [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie-backports main' |
        sudo tee /etc/apt/sources.list.d/anland-hyprland-sources.list >/dev/null || die 'cannot configure compatible source packages'
    run sudo apt-get update
    run sudo apt-get build-dep -y --no-install-recommends xwayland
}

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'hyprland-build: %s\n' "$*" >&2; exit 1; }
run() { "$@" || die "failed: $*"; }
find_patch() {
    local name="$1" file="${2:-}"
    if [ -n "$file" ]; then
        [ -f "$file" ] || die "patch not found: $file"
        realpath "$file"; return
    fi
    for file in "$SCRIPT_DIR/$name.patch" "$PWD/$name.patch" "$SCRIPT_DIR/../$name.patch" \
        "$ROOT/producers/kde/Debian13_v5/$name.patch"; do
        [ ! -f "$file" ] || { realpath "$file"; return; }
    done
    die "$name.patch not found (set ${name^^}_PATCH)"
}
build_env() {
    env PKG_CONFIG_PATH="$LIBDIR/pkgconfig" CMAKE_PREFIX_PATH="$SYSROOT/usr" \
        LD_LIBRARY_PATH="$LIBDIR" "$@"
}

# Keep cached development trees intact. Refuse stale/incomplete preparation
# rather than silently ignoring changed patches or overwriting local edits.
source_identity() {
    python3 - "$SCRIPT_DIR/sources.json" "$ROOT" "$BACKEND" "$1" "${2:-}" <<'PY'
import hashlib, json, pathlib, sys
lock_path, root, backend, name, patch = sys.argv[1:]
lock = json.loads(pathlib.Path(lock_path).read_text())
h = hashlib.sha256()
entry = lock['plugins'] if name in lock['plugins'] else lock['sources']
h.update(json.dumps(entry[name] if name != 'xwayland' else lock['xwayland'], sort_keys=True).encode())
if name == 'hyprgrass':
    h.update(json.dumps(lock['plugins']['wf-touch'], sort_keys=True).encode())
if patch:
    h.update(pathlib.Path(patch).read_bytes())
folders = []
if name in ('aquamarine', 'hyprland'):
    folders.append(pathlib.Path(backend) / name)
if name == 'aquamarine':
    folders.extend(pathlib.Path(root) / p for p in ('libdisplay_producer', 'common'))
for folder in folders:
    for p in sorted(folder.rglob('*')):
        if p.is_file():
            h.update(str(p.relative_to(folder)).encode() + b'\0')
            h.update(p.read_bytes())
print(h.hexdigest())
PY
}
reuse_source() {
    local tree="$1" identity="$2"
    [ -f "$tree/.anland-source-identity" ] &&
        [ "$(cat "$tree/.anland-source-identity")" = "$identity" ] ||
        die "stale or untracked prepared source: $tree; refresh explicitly (local edits are preserved)"
}

declare -A directory version runtime source_tree plugin patch_file build_dir
declare -a PROJECTS=(hyprutils hyprlang hyprgraphics aquamarine hyprland) packages=()
fetch_sources() {
    local kind name a b result tree source_version
    run mkdir -p "$WORKDIR/downloads" "$WORKDIR/src" "$ARTIFACTS"
    python3 - "$SCRIPT_DIR/sources.json" > "$WORKDIR/sources.tsv" <<'PY'
import json, sys
with open(sys.argv[1]) as f: lock = json.load(f)
for name in lock['sources']:
    print('source', name, lock['runtime_packages'][name], sep='\t')
for name in ('hyprgrass', 'wf-touch'):
    e = lock['plugins'][name]
    print('plugin', name, e['url'], e.get('revision', ''), sep='\t')
PY
    [ $? -eq 0 ] || die 'cannot read sources.json'
    for name in aquamarine hyprland hyprgrass; do
        local var="${name^^}_PATCH"
        patch_file[$name]="$(find_patch "$name" "${!var:-}")" || die "cannot select $name patch"
        verify_patch "$name" "${patch_file[$name]}"
    done
    while IFS=$'\t' read -r kind name a b; do
        if [[ $kind == source ]]; then
            local extra requested_version
            extra=$(source_identity "$name" "${patch_file[$name]:-}") || die "source identity: $name"
            requested_version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sources"][sys.argv[2]].get("version", ""))' "$SCRIPT_DIR/sources.json" "$name") || die "source version: $name"
            result=$(anland_prepare_latest_apt_source "$name" "$WORKDIR" "" "${ANLAND_OFFLINE:-0}" "$extra" trixie-backports "$requested_version") || die "cannot fetch compatible source: $name"
            IFS=$'\t' read -r tree source_version <<< "$result"
            source_tree[$name]=$tree; version[$name]=$source_version; runtime[$name]=$a
        else
            local patch="" extra
            [[ $name != hyprgrass ]] || patch=${patch_file[hyprgrass]}
            extra=$(source_identity "$name" "$patch") || die "source identity: $name"
            result=$(anland_prepare_latest_git_source "$name" "$a" "$WORKDIR" "$patch" "${ANLAND_OFFLINE:-0}" "$extra" shallow "$b") || die "cannot fetch compatible Git source: $name"
            IFS=$'\t' read -r tree source_version <<< "$result"
            plugin[$name]=$tree
        fi
    done < "$WORKDIR/sources.tsv"
    local input_set="" source_path
    for name in "${PROJECTS[@]}" hyprgrass wf-touch; do
        if [[ $name == hyprgrass || $name == wf-touch ]]; then source_path=${plugin[$name]}; else source_path=${source_tree[$name]}; fi
        input_set+="$(cat "$source_path/.anland-source-identity")\n"
    done
    input_set=$(printf '%s' "$input_set" | sha256sum | awk '{print $1}') || die 'source set identity'
    SYSROOT="$WORKDIR/sysroot-$input_set"
    LIBDIR="$SYSROOT/usr/lib/$ARCH"
}
verify_patch() {
    local expected
    expected=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["patches"][sys.argv[2]+".patch"])' \
        "$SCRIPT_DIR/sources.json" "$1") || die 'cannot read patch checksum'
    printf '%s  %s\n' "$expected" "$2" | sha256sum -c --status - || die "checksum: $1.patch (update lock for an intentional override)"
}
prepare_source() {
    local name="$1" raw="${source_tree[$1]}" tree identity
    identity=$(source_identity "$name" "${patch_file[$name]:-}") || die "source identity: $name"
    identity=$(printf '%s\n%s\n' "$identity" "$(cat "$raw/.anland-source-identity")" | sha256sum | awk '{print $1}') || die "source identity: $name"
    tree="$WORKDIR/src/prepared-$name-${identity:0:16}"
    if [ -d "$tree" ]; then reuse_source "$tree" "$identity"; source_tree[$name]=$tree; log "Reuse $name"; return; fi
    log "Prepare $name"
    run mkdir -p "$tree"
    run cp -a "$raw/." "$tree/"
    run rm -f "$tree/.anland-source-identity"
    if [ "$name" = aquamarine ] || [ "$name" = hyprland ]; then
        run cp -aL "$BACKEND/$name/." "$tree/"
        if [ "$name" = aquamarine ]; then
            run mkdir -p "$tree/src/backend/anland/public"
            run cp -aL "$ROOT/libdisplay_producer" "$ROOT/common" "$tree/src/backend/anland/public/"
        fi
        run mkdir -p "$tree/debian/patches"
        run cp "${patch_file[$name]}" "$tree/debian/patches/anland-$name.patch"
        printf '\nanland-%s.patch\n' "$name" >> "$tree/debian/patches/series" || die 'cannot update patch series'
        (cd "$tree" && QUILT_PATCHES=debian/patches quilt push --fuzz=2 -a) || die "quilt: $name"
    fi
    { printf '%s (%s+anland5) UNRELEASED; urgency=medium\n\n  * Anland v5 backend.\n\n -- Anland <build@localhost>  Wed, 07 Oct 2026 00:00:00 +0000\n\n' "$name" "${version[$name]}"
      cat "$tree/debian/changelog"; } > "$tree/debian/changelog.new" || die 'cannot write changelog'
    run mv "$tree/debian/changelog.new" "$tree/debian/changelog"
    printf '%s\n' "$identity" > "$tree/.anland-source-identity" || die 'cannot record prepared source'
    source_tree[$name]=$tree
}
cmake_build() {
    local name="$1" identity folder module
    identity=$(cat "${source_tree[$name]}/.anland-source-identity") || die "source identity: $name"
    folder="$WORKDIR/build/$name-${identity:0:16}"
    build_dir[$name]=$folder
    log "Build $name"
    local -a args=(-U 'pkgcfg_lib_*' -U '__pkg_config_checked_*'
        -S "${source_tree[$name]}" -B "$folder" -G Ninja
        -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_FULLY_DISCONNECTED=ON
        -DCMAKE_INSTALL_PREFIX=/usr "-DCMAKE_INSTALL_LIBDIR=lib/$ARCH")
    if command -v ccache >/dev/null; then
        args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    fi
    if [ "$name" != hyprutils ]; then
        args+=("-DCMAKE_CXX_FLAGS=-I$SYSROOT/usr/include" "-Dpkgcfg_lib_deps_hyprutils=$LIBDIR/libhyprutils.so")
    fi
    if [ "$name" = hyprland ]; then
        args+=(-DBUILD_TESTING=OFF -DBUILD_HYPRTESTER=OFF -DNO_HYPRPM=OFF "-DCMAKE_INSTALL_INCLUDEDIR=include/$ARCH")
        for module in hyprutils_dep hyprctl_deps hyprpm_deps starthyprland_deps; do
            args+=("-Dpkgcfg_lib_${module}_hyprutils=$LIBDIR/libhyprutils.so")
        done
        for module in hyprlang hyprgraphics; do args+=("-Dpkgcfg_lib_${module}_dep_$module=$LIBDIR/lib$module.so"); done
    fi
    run build_env cmake "${args[@]}"
    run build_env cmake --build "$folder" --parallel "$JOBS"
    if [ "$name" != hyprland ]; then
        run build_env env DESTDIR="$SYSROOT" cmake --install "$folder"
        run sed -i -e "s|^prefix=/usr$|prefix=$SYSROOT/usr|" \
            -e 's|includedir=/usr/include|includedir=${prefix}/include|' \
            -e 's|libdir=/usr/lib/|libdir=${prefix}/lib/|' "$LIBDIR/pkgconfig/$name.pc"
    fi
}
build_hyprgrass() {
    local raw="${plugin[hyprgrass]}" touch="${plugin[wf-touch]}" tree folder pc="$WORKDIR/plugin-pkgconfig" identity
    identity=$(source_identity hyprgrass "${patch_file[hyprgrass]}") || die 'source identity: hyprgrass'
    identity=$(printf '%s\n%s\n' "$identity" "$(cat "$raw/.anland-source-identity")$(cat "$touch/.anland-source-identity")" | sha256sum | awk '{print $1}') || die 'plugin identity'
    tree="$WORKDIR/src/prepared-hyprgrass-${identity:0:16}"
    folder="$WORKDIR/build/hyprgrass-${identity:0:16}"
    build_dir[hyprgrass]=$folder
    plugin[hyprgrass-composed]=$tree
    if [ -d "$tree" ]; then
        reuse_source "$tree" "$identity"
    else
        run mkdir -p "$tree"
        run cp -a "$raw/." "$tree/"
        run rm -f "$tree/.anland-source-identity"
        run mkdir -p "$tree/subprojects/wf-touch"
        run cp -a "$touch/." "$tree/subprojects/wf-touch/"
        printf '%s\n' "$identity" > "$tree/.anland-source-identity" || die 'cannot record prepared plugin'
    fi
    log 'Build hyprgrass'
    run build_env env DESTDIR="$WORKDIR/plugin-sdk" cmake --install "${build_dir[hyprland]}"
    run mkdir -p "$pc"
    run sed "s|prefix=/usr/include/$ARCH|prefix=$WORKDIR/plugin-sdk/usr/include/$ARCH|" \
        "${build_dir[hyprland]}/hyprland.pc" > "$pc/hyprland.pc"
    local -a opts=()
    [ ! -f "$folder/meson-private/coredata.dat" ] || opts+=(--reconfigure)
    run build_env env PKG_CONFIG_PATH="$pc:$LIBDIR/pkgconfig" CXXFLAGS="-I$SYSROOT/usr/include" \
        meson setup "${opts[@]}" "$folder" "$tree" --prefix=/usr \
        "--libdir=lib/$ARCH/hyprland/plugins" --wrap-mode=nodownload -Dtests=disabled
    run build_env env PKG_CONFIG_PATH="$pc:$LIBDIR/pkgconfig" CXXFLAGS="-I$SYSROOT/usr/include" \
        meson compile -C "$folder" -j "$JOBS"
}
package_deb() {
    local name="$1" tree="${source_tree[$1]}" dep soname
    local package_version="${version[$name]}+anland5"
    local produced="$WORKDIR/src/${runtime[$name]}_${package_version#*:}_arm64.deb"
    log "Package $name"
    : > "$tree/debian/shlibs.local" || die 'cannot write shlibs.local'
    for dep in hyprutils hyprlang hyprgraphics aquamarine; do
        case "$dep" in hyprutils) soname=12 ;; hyprlang) soname=2 ;; hyprgraphics) soname=4 ;; aquamarine) soname=14 ;; esac
        printf 'lib%s %s %s (= %s+anland5)\n' "$dep" "$soname" "${runtime[$dep]}" "${version[$dep]}" \
            >> "$tree/debian/shlibs.local" || die 'cannot write shlibs.local'
    done
    cat > "$tree/debian/rules" <<EOF
#!/usr/bin/make -f
include /usr/share/dpkg/architecture.mk
export DH_OPTIONS = -p${runtime[$name]}
%:
	dh \$@ -p${runtime[$name]} --buildsystem=cmake --builddirectory=${build_dir[$name]}
override_dh_auto_configure override_dh_auto_build override_dh_auto_test override_dh_dwz override_dh_missing:
	:
override_dh_strip:
	dh_strip --no-automatic-dbgsym
override_dh_shlibdeps:
	dh_shlibdeps -l$LIBDIR
override_dh_auto_install:
	DESTDIR=\$(CURDIR)/debian/tmp cmake --install ${build_dir[$name]}
EOF
    [ $? -eq 0 ] || die 'cannot write Debian rules'
    if [ "$name" = hyprland ]; then
        cat >> "$tree/debian/rules" <<EOF
	install -m755 $SCRIPT_DIR/startup.sh debian/tmp/usr/bin/hyprland-anland
	DESTDIR=\$(CURDIR)/debian/tmp meson install -C ${build_dir[hyprgrass]} --no-rebuild
	rm -f debian/tmp/usr/lib/$ARCH/hyprland/plugins/libwftouch.a
override_dh_install:
	dh_install
	install -d debian/hyprland/usr/share/doc/hyprland/hyprgrass
	install -m644 ${plugin[hyprgrass-composed]}/LICENSE debian/hyprland/usr/share/doc/hyprland/hyprgrass/hyprgrass-LICENSE
	install -m644 ${plugin[hyprgrass-composed]}/LICENSE.aosp debian/hyprland/usr/share/doc/hyprland/hyprgrass/hyprgrass-LICENSE.aosp
	install -m644 ${plugin[hyprgrass-composed]}/subprojects/wf-touch/LICENSE debian/hyprland/usr/share/doc/hyprland/hyprgrass/wf-touch-LICENSE
override_dh_gencontrol:
	. debian/tmp/usr/lib/\${DEB_HOST_MULTIARCH}/hyprland/debian-abi-helper && dh_gencontrol -- "-Vhyprland:Provides=\$\${HYPRLAND_ABI}, \$\${HYPRLAND_API}"
EOF
        [ $? -eq 0 ] || die 'cannot append Debian rules'
    fi
    run chmod +x "$tree/debian/rules"
    local stamp
    stamp=$(mktemp "$tree/.package-start.XXXXXX") || die "cannot mark package build: $name"
    (cd "$tree" && build_env fakeroot debian/rules binary) || die "package: $name"
    [[ -s $produced && $produced -nt $stamp ]] || die "missing or stale package: $produced"
    [[ $(dpkg-deb -f "$produced" Package) == "${runtime[$name]}" &&
       $(dpkg-deb -f "$produced" Version) == "$package_version" &&
       $(dpkg-deb -f "$produced" Architecture) == arm64 ]] || die "unexpected package metadata: $produced"
    run dpkg-deb --contents "$produced" >/dev/null
    [[ $produced -ef $ARTIFACTS/${produced##*/} ]] || run cp -p "$produced" "$ARTIFACTS/"
    packages+=("${produced##*/}")
    run rm -f -- "$stamp"
}
# Follow the current .changes file, not every package left in the cache.
collect_xwayland_package() {
    local tree=$1 out=$2 expected_version=$3 stamp=$4
    local built_version changes rows sha size file produced count=0
    built_version=$(dpkg-parsechangelog -l "$tree/debian/changelog" -S Version) ||
        die 'cannot read built Xwayland version'
    [[ $built_version == "$expected_version" ]] || die 'Xwayland version differs from lock'
    changes="$out/xwayland_${built_version#*:}_arm64.changes"
    [[ -s $changes && $changes -nt $stamp ]] || die "missing or stale .changes: $changes"
    rows=$(awk '/^Checksums-Sha256:/ {in_files=1; next}
        in_files && /^[^ ]/ {exit}
        in_files && NF == 3 {printf "%s\t%s\t%s\n", $1, $2, $3}' "$changes") ||
        die 'cannot read Xwayland output list'
    while IFS=$'\t' read -r sha size file; do
        case $file in xwayland_*.deb) ;; *) continue ;; esac
        [[ $file != */* && $sha =~ ^[0-9a-f]{64}$ && $size =~ ^[0-9]+$ ]] ||
            die 'invalid package entry in .changes'
        produced="$out/$file"
        [[ -s $produced && $produced -nt $stamp ]] || die "missing or stale package: $produced"
        [[ $(stat -c %s -- "$produced") == "$size" ]] || die 'Xwayland package size mismatch'
        printf '%s  %s\n' "$sha" "$produced" | sha256sum -c --status - || die 'Xwayland package checksum mismatch'
        [[ $(dpkg-deb -f "$produced" Package) == xwayland &&
           $(dpkg-deb -f "$produced" Version) == "$built_version" &&
           $(dpkg-deb -f "$produced" Architecture) == arm64 ]] || die 'unexpected Xwayland package metadata'
        run dpkg-deb --contents "$produced" >/dev/null
        [[ $produced -ef $ARTIFACTS/$file ]] || run cp -p "$produced" "$ARTIFACTS/"
        packages+=("$file")
        count=$((count + 1))
    done <<< "$rows"
    [[ $count == 1 ]] || die 'expected one Xwayland runtime package in .changes'
}
build_xwayland() {
    local out="$WORKDIR/xwayland" patch="$SCRIPT_DIR/xwayland.patch" result
    local tree version stamp
    run mkdir -p "$out" "$ARTIFACTS_DIR"
    result=$(anland_prepare_latest_apt_source xwayland "$out" "$patch" "${ANLAND_OFFLINE:-0}") || die 'cannot prepare latest Ubuntu Xwayland source'
    IFS=$'\t' read -r tree version <<< "$result"
    # dpkg-buildpackage writes packages beside the prepared source tree.
    out=$(dirname -- "$tree") || die 'cannot resolve Xwayland output directory'
    stamp=$(mktemp "$out/.build-start.XXXXXX") || die 'cannot mark Xwayland build'
    # The distro Xwayland must never inherit the WM's private SDK/library paths.
    (cd "$tree" && env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u CMAKE_PREFIX_PATH \
        -u LD_LIBRARY_PATH -u DESTDIR -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        PATH=/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        DEB_BUILD_OPTIONS="nocheck nodwz noautodbgsym parallel=$JOBS" \
        dpkg-buildpackage -b -uc -us) || die 'Xwayland build failed'
    collect_xwayland_package "$tree" "$out" "$version" "$stamp"
    run rm -f -- "$stamp"
}

main() {
    [ $# -eq 0 ] || die 'use environment variables, not arguments'
    ubuntu_preflight
    ubuntu_dependencies
    ubuntu_build_environment
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
    run mkdir -p "$WORKDIR" "$ARTIFACTS"
    exec 8>"$WORKDIR/build.lock" || die 'cannot open build lock'
    flock -n 8 || die 'another build owns WORKDIR'
    exec 9>"$ARTIFACTS/.build.lock" || die 'cannot open artifact lock'
    flock -n 9 || die 'another build owns artifacts'
    fetch_sources
    local name file
    for name in "${PROJECTS[@]}"; do prepare_source "$name"; cmake_build "$name"; done
    build_hyprgrass
    for name in "${PROJECTS[@]}"; do package_deb "$name"; done
    build_xwayland
    (cd "$ARTIFACTS" && sha256sum "${packages[@]}") || die 'cannot hash packages'
    if [ "${ANLAND_INSTALL:-0}" = 1 ]; then
        local -a debs=()
        for file in "${packages[@]}"; do debs+=("$ARTIFACTS/$file"); done
        run $SUDO dpkg -i "${debs[@]}"
    fi
    log "Done. Packages: $ARTIFACTS"
}
build_deps=(build-essential cmake meson ninja-build pkgconf ccache python3 curl ca-certificates git dpkg-dev debhelper fakeroot quilt patch xz-utils unzip zip file binutils libpipewire-0.3-dev cpio gettext xxd hwdata hyprwayland-scanner hyprland-protocols hyprwire-scanner libhyprcursor-dev libhyprwire-dev libcairo2-dev libdrm-dev libxkbcommon-dev libegl-dev libopengl-dev libgbm-dev libgles-dev libglaze-dev glslang-dev glslang-tools libinput-dev liblcms2-dev liblua5.4-dev libmuparser-dev libpango1.0-dev libpixman-1-dev libre2-dev libseat-dev libtomlplusplus-dev libudev-dev libudis86-dev libwayland-dev libxcb1-dev libxcb-render0-dev libxcb-xfixes0-dev libxcb-icccm4-dev libxcb-composite0-dev libxcb-res0-dev libxcb-errors-dev libssl-dev libxcursor-dev libjpeg-dev libwebp-dev libmagic-dev libspng-dev librsvg2-dev libjxl-dev libheif-dev libdisplay-info-dev libglm-dev libevdev-dev wayland-protocols xwayland)
main "$@"
