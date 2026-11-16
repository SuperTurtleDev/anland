#!/usr/bin/env bash
# Fedora 43 ARM64: the same locked Denial/Flutter build, native RPM packaging.
# ANLAND_WITH_DEPS=1 JOBS=4 ANLAND_INSTALL=0 bash build.sh [--noconfirm]
# ANLAND_STAGE=check validates the recipe without compiling or touching services.
# Fedora build/package functions are local to this WM recipe.
fedora_die() { printf 'fedora-build: %s\n' "$*" >&2; exit 1; }

fedora_run() { "$@" || fedora_die "failed: $*"; }

fedora_preflight() {
    local ID VERSION_ID
    source /etc/os-release || fedora_die 'cannot identify distribution'
    [[ $ID == fedora && $VERSION_ID == 43 && $(uname -m) == aarch64 ]] ||
        fedora_die 'requires native Fedora 43 aarch64'
    [[ $(id -u) != 0 ]] || fedora_die 'compile as an ordinary user'
    JOBS=${JOBS:-4}
    [[ $JOBS =~ ^[1-9][0-9]*$ ]] || fedora_die 'JOBS must be a positive integer'
    local flag
    for flag in ANLAND_INSTALL ANLAND_WITH_DEPS ANLAND_OFFLINE; do
        printf -v "$flag" '%s' "${!flag:-0}"
        [[ ${!flag} =~ ^[01]$ ]] || fedora_die "$flag must be 0 or 1"
    done
    ANLAND_STAGE=${ANLAND_STAGE:-build}
    case $ANLAND_STAGE in check|prepare|build) ;; *) fedora_die 'ANLAND_STAGE must be check, prepare or build' ;; esac
    [[ $ANLAND_OFFLINE != 1 || $ANLAND_WITH_DEPS != 1 ]] || fedora_die 'offline dependency installation is not supported'
    [[ $ANLAND_STAGE == build || $ANLAND_INSTALL == 0 ]] || fedora_die 'installation requires the build stage'
}

fedora_reuse() {
    [[ -d $1 ]] || return 1
    [[ -f $1/.anland-source-identity && $(cat "$1/.anland-source-identity") == "$FEDORA_INPUT_ID" ]] ||
        fedora_die "stale/unmanaged source preserved: $1; refresh it explicitly"
}

fedora_seal() { printf '%s\n' "$FEDORA_INPUT_ID" > "$1/.anland-source-identity" || fedora_die 'cannot record source identity'; }

fedora_stage_rpm() {
    local name=$1 version=$2 license=$3 stage=$4 requires=${5:-} top spec marker file found=0
    [[ $name =~ ^[a-zA-Z0-9][a-zA-Z0-9+._-]*$ && $version =~ ^[a-zA-Z0-9][a-zA-Z0-9+._~]*$ ]] || fedora_die 'unsafe RPM identity'
    top="$WORKDIR/rpm/$name"
    fedora_run mkdir -p "$top"/{SPECS,SOURCES,BUILD,BUILDROOT,RPMS,SRPMS}
    fedora_run tar -cJf "$top/SOURCES/runtime.tar.xz" -C "$stage" .
    python3 - "$stage" "$top/SOURCES/files.list" <<'PY' || fedora_die 'RPM file list'
from pathlib import Path
import sys
stage, output = map(Path, sys.argv[1:])
standard = {'/', '/etc', '/etc/xdg', '/usr', '/usr/bin', '/usr/lib', '/usr/lib64', '/usr/share', '/usr/share/doc', '/usr/share/licenses', '/usr/share/applications', '/usr/share/wayland-sessions', '/usr/share/dbus-1', '/usr/share/dbus-1/services', '/usr/lib/systemd', '/usr/lib/systemd/user', '/usr/share/xdg-desktop-portal', '/usr/share/xdg-desktop-portal/portals'}
lines = []
for p in sorted(stage.rglob('*')):
    path = '/' + p.relative_to(stage).as_posix()
    if any(c in path for c in '\n\r%"'):
        raise SystemExit('unsafe RPM path: '+path)
    if p.is_symlink() or p.is_file():
        tag = '%config(noreplace) ' if path.startswith('/etc/') and not p.is_symlink() else ''
        if path.startswith('/usr/share/licenses/') and not p.is_symlink(): tag = '%license '
        lines.append(tag+'"'+path+'"')
    elif p.is_dir() and path not in standard:
        lines.append('%dir "'+path+'"')
if not lines: raise SystemExit('empty runtime stage')
output.write_text('\n'.join(lines)+'\n')
PY
    spec="$top/SPECS/$name.spec"
    # Package an already compiled DESTDIR. No binary conversion from .deb/pacman.
    cat > "$spec" <<EOF
%global debug_package %{nil}
%global _build_id_links none
%global __strip /usr/bin/true
%global __provides_exclude_from ^/usr/lib(64)?/(denial-anland|mango-anland|niri-anland|hyprland-anland)/.*$
Name: $name
Version: $version
Release: 1.anland%{?dist}
Summary: $name with the Anland v5 backend
License: $license
URL: https://github.com/sjszero/anland
Source0: runtime.tar.xz
Source1: files.list
ExclusiveArch: aarch64
BuildRequires: tar, xz, coreutils
EOF
    [[ -z $requires ]] || printf 'Requires: %s\n' "$requires" >> "$spec" || fedora_die 'RPM requirements'
    # Private SONAME dependencies are satisfied by this same package. Do not
    # export them globally, or require nonexistent Fedora packages for them.
    local sonames
    sonames=$(find "$stage" -type f -name '*.so*' -exec readelf -d {} \; 2>/dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p' | sort -u | sed 's/[.]/\\./g' | paste -sd '|') || fedora_die 'private SONAME list'
    [[ -z $sonames ]] || printf '%%global __requires_exclude ^(%s)(\\(.*\\))?$\n' "$sonames" >> "$spec" || fedora_die 'private RPM dependencies'
    cat >> "$spec" <<'EOF'
%description
Native Fedora 43 runtime built from current upstream sources. Installing this package
does not enable, stop or restart graphical sessions or desktop services.
%prep
%build
%install
mkdir -p %{buildroot}
tar -xJf %{SOURCE0} -C %{buildroot}
%files -f %{SOURCE1}
%defattr(-,root,root,-)
EOF
    marker=$(mktemp "$top/build-start.XXXXXX") || fedora_die 'RPM marker'
    fedora_run rpmbuild -bb --define "_topdir $top" --define "_smp_build_ncpus $JOBS" "$spec"
    while IFS= read -r file; do
        [[ $(rpm -qp --qf '%{NAME}|%{VERSION}|%{ARCH}' "$file") == "$name|$version|aarch64" ]] || fedora_die "RPM identity: $file"
        fedora_publish_rpm "$file"
        found=$((found + 1))
    done < <(find "$top/RPMS" -type f -name '*.rpm' ! -name '*debug*' -newer "$marker")
    [[ $found == 1 ]] || fedora_die "expected one current RPM: $name"
    fedora_run rm -f -- "$marker"
}

fedora_publish_rpm() {
    local file=$1 destination="$ARTIFACTS_DIR/${1##*/}" architecture
    architecture=$(rpm -qp --qf '%{ARCH}' "$file") || fedora_die 'RPM architecture'
    [[ $architecture == aarch64 || $architecture == noarch ]] || fedora_die "wrong architecture: $file"
    fedora_run rpm -K "$file"
    fedora_run rpm -qpl "$file" >/dev/null
    [[ ! -e $destination.sig && ! -L $destination.sig ]] || cmp -s "$file" "$destination" || fedora_die "existing signature preserved: $destination.sig"
    fedora_run cp -p -- "$file" "$destination"
    packages+=("$destination")
}

fedora_srpm_build() {
    local top=$FEDORA_SRPM_TOP spec=$FEDORA_SRPM_SPEC marker file found=0
    if [[ $ANLAND_WITH_DEPS == 1 ]]; then
        fedora_run sudo dnf --setopt=install_weak_deps=False builddep -y "$spec"
    fi
    marker=$(mktemp "$top/build-start.XXXXXX") || fedora_die 'SRPM build marker'
    # Keep Xwayland/Mutter isolated from the other WM's private SDK.
    fedora_run env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u CMAKE_PREFIX_PATH \
        -u LD_LIBRARY_PATH -u DESTDIR -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        rpmbuild -bb --define "_topdir $top" --define "_smp_build_ncpus $JOBS" "$spec"
    while IFS= read -r file; do fedora_publish_rpm "$file"; found=$((found + 1));
    done < <(find "$top/RPMS" -type f -name '*.rpm' ! -name '*debug*' ! -name '*-tests-*' -newer "$marker")
    [[ $found -gt 0 ]] || fedora_die 'no current SRPM build output'
    fedora_run rm -f -- "$marker"
}

fedora_xwayland() {
    # Every WM keeps its Xwayland inputs alongside its own distro recipe.
    fedora_srpm_prepare xorg-x11-server-Xwayland "$SCRIPT_DIR/xwayland.patch"
    [[ $ANLAND_STAGE != build ]] || fedora_srpm_build
}

fedora_finish() {
    [[ ${#packages[@]} -gt 0 ]] || fedora_die 'empty Fedora package set'
    printf '%s\n' "${packages[@]##*/}" > "$ARTIFACTS_DIR/packages.list" || fedora_die 'package list'
    (cd "$ARTIFACTS_DIR" && sha256sum -- "${packages[@]##*/}" > SHA256SUMS && sha256sum -c SHA256SUMS) || fedora_die 'package checksums'
    python3 - "$ROOT" "$FEDORA_INPUT_ID" "$ARTIFACTS_DIR/build-inputs.json" <<'PY' || fedora_die 'build record'
import json,subprocess,sys
root,identity,out=sys.argv[1:]
record={'distribution':'fedora43','architecture':'aarch64','input_sha256':identity,
 'anland_revision':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip(),
 'graphical_test':False}
open(out,'w').write(json.dumps(record,indent=2)+'\n')
PY
    if [[ $ANLAND_INSTALL == 1 ]]; then
        # Scoped only to this requested transaction; never rewrite dnf.conf.
        fedora_run sudo dnf --setopt=excludepkgs= install -y "${packages[@]}"
    fi
    printf '\nFedora RPMs: %s\nNo desktop/session was started or restarted. Graphical validation NOT performed.\n' "$ARTIFACTS_DIR"
}

fedora_denial_preflight() {
    fedora_preflight
    # The shared engine entry already owns WORKDIR/ARTIFACTS locks.
    ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd -P) || die 'repository root'
    source "$ROOT/producers/common/fedora-latest-srpm.sh"
    FEDORA_INPUT_ID=$(cat "$SCRIPT_DIR/sources.conf" "$SCRIPT_DIR/build.sh" \
        "$SCRIPT_DIR/xwayland.patch" "$ROOT/producers/common/fedora-latest-srpm.sh" \
        "$ROOT/producers/denial/Arch_v5/build.sh" | sha256sum) || die 'Fedora input identity'
    FEDORA_INPUT_ID=${FEDORA_INPUT_ID%% *}
    build_deps=(gcc gcc-c++ cmake meson ninja-build pkgconf-pkg-config ccache python3 \
        curl ca-certificates git-core rpm-build rpmdevtools dnf-plugins-core cpio patch tar xz unzip zip \
        file binutils rustup clang lld llvm jq gnupg2 dbus pipewire-devel pulseaudio-libs-devel \
        libglvnd-devel mesa-libgbm-devel libdrm-devel libinput-devel libseat-devel libxkbcommon-devel \
        wayland-devel wayland-protocols-devel systemd-devel pam-devel fontconfig-devel \
        freetype-devel glib2-devel libunwind-devel libffi-devel zlib-devel vulkan-loader-devel \
        xkeyboard-config)
    if [[ $ANLAND_WITH_DE == 1 ]]; then
        build_deps+=(polkit-devel xdg-utils zenity polkit xdg-desktop-portal \
                     xdg-desktop-portal-gtk xdg-desktop-portal-wlr)
    fi
}
fedora_denial_dependencies() {
    [[ $ANLAND_STAGE != check ]] || return 0
    if [[ $ANLAND_WITH_DEPS == 1 ]]; then
        fedora_run sudo dnf --setopt=install_weak_deps=False install -y "${build_deps[@]}"
    fi
    [[ $ANLAND_WITH_DE == 0 ]] || run pkg-config --print-errors --exists polkit-agent-1 polkit-gobject-1
}
prepare_fedora_rust() {
    export RUSTUP_HOME="$WORKDIR/rustup"
    export CARGO_HOME="${CARGO_HOME:-$WORKDIR/cargo-home}"
    export PATH="$CARGO_HOME/bin:$PATH"
    RUSTUP_TOOLCHAIN=$(python3 - "$SOURCE/rust-toolchain.toml" <<'PY'
import sys,tomllib
print(tomllib.load(open(sys.argv[1],'rb'))['toolchain']['channel'])
PY
    ) || die 'locked Rust toolchain'
    export RUSTUP_TOOLCHAIN
    # Fedora packages the installer, not pre-initialized Rust proxies.
    # --default-toolchain none is local-only; fetch the locked channel below.
    if [[ ! -x $CARGO_HOME/bin/rustup || ! -x $CARGO_HOME/bin/cargo || ! -x $CARGO_HOME/bin/rustc ]]; then
        command -v rustup-init >/dev/null || die 'rustup-init required (ANLAND_WITH_DEPS=1)'
        run mkdir -p "$CARGO_HOME" "$RUSTUP_HOME"
        run env RUSTUP_INIT_SKIP_PATH_CHECK=yes rustup-init -y --no-modify-path \
            --default-toolchain none --profile minimal
        hash -r
    fi
    [[ -x $CARGO_HOME/bin/rustup && -x $CARGO_HOME/bin/cargo && -x $CARGO_HOME/bin/rustc ]] ||
        die 'Rust proxy initialization failed'
    if ! rustup run "$RUSTUP_TOOLCHAIN" rustc --version >/dev/null 2>&1; then
        [[ $ANLAND_OFFLINE == 0 ]] || die 'offline locked Rust toolchain missing'
        run rustup toolchain install "$RUSTUP_TOOLCHAIN" --profile minimal --no-self-update
    fi
    run rustc --version
    run cargo --version
}
package_all() {
    log 'Package native Fedora Denial RPMs (no installation or service changes)'
    local pack="$WORKDIR/package-fedora" kind stage name version requires file app engine_root desktop_root
    local -a kinds=(engine wm)
    [[ $ANLAND_WITH_DE == 0 ]] || kinds+=(de)
    run mkdir -p "$pack"
    run cp "$ROOT/producers/denial/Arch_v5/PKGBUILD" "$SCRIPT_DIR/sources.conf" "$SCRIPT_DIR/startup.sh" "$pack/"
    printf '\nDENIAL_REV=%q\nDENIAL_VERSION=%q\nFLUTTER_REV=%q\nFLUTTER_VERSION=%q\n' \
        "$DENIAL_REV" "$DENIAL_VERSION" "$FLUTTER_REV" "$FLUTTER_VERSION" >> "$pack/sources.conf" || die 'selected source identity'
    printf 'ENGINE_VERSION=%q\nENGINE_SHA256=%q\nENGINE_ICU_SHA256=%q\n' \
        "$ENGINE_VERSION" "$ENGINE_SHA256" "$ENGINE_ICU_SHA256" >> "$pack/sources.conf" || die 'selected engine identity'
    export DENIAL_SOURCE_TREE="$SOURCE" DENIAL_BUILD_DIR="$BUILD" DENIAL_ENGINE_DIR="$ENGINE"
    packages=()
    for kind in "${kinds[@]}"; do
        stage="$pack/$kind"
        run rm -rf -- "$stage"
        run mkdir -p "$stage"
        in_dir "$pack" env DENIAL_PACKAGE_KIND="$kind" DENIAL_STAGE="$stage" bash -c '
            set -euo pipefail
            startdir=$PWD; pkgdir=$DENIAL_STAGE
            source "$startdir/PKGBUILD"
            package
        '
        case $kind in
            engine) name=denial-anland-engine; version=${ENGINE_VERSION//-/.}; requires='fontconfig' ;;
            wm) name=denial; version=${DENIAL_VERSION//-/.}
                requires="denial-anland-engine = ${ENGINE_VERSION//-/.}, bash, coreutils, util-linux, procps-ng, binutils, dbus, pulseaudio-libs, xkeyboard-config, xorg-x11-server-Xwayland" ;;
            de) name=denial-desktop; version=${DENIAL_VERSION//-/.}
                requires="denial = $version, denial-anland-engine = ${ENGINE_VERSION//-/.}, bash, git-core, unzip, xz, zlib, xdg-utils, zenity, polkit, xorg-x11-server-Xwayland, xdg-desktop-portal, xdg-desktop-portal-gtk, xdg-desktop-portal-wlr" ;;
        esac
        # Keep Denial's private /usr/lib layout: relative engine and bundle
        # symlinks are independent of Fedora's system /usr/lib64 convention.
        fedora_stage_rpm "$name" "$version" 'GPL-3.0-or-later AND BSD-3-Clause' "$stage" "$requires"
    done
    # Verify the installed layout rather than the three staging trees: the
    # bundles carry relative symlinks into ../flutter, so only a merged tree
    # resolves them. This mirrors the Arch and Debian checks, and running the
    # binaries without LD_LIBRARY_PATH is what proves the packaged RUNPATH works.
    run rm -rf -- "$BUILD/package-check"
    run mkdir -p "$BUILD/package-check"
    for kind in "${kinds[@]}"; do
        run cp -a -- "$pack/$kind/." "$BUILD/package-check/"
    done
    runtime="$BUILD/package-check/usr/lib/denial-anland"
    check_sha "$ENGINE_SHA256" "$runtime/flutter/lib/libflutter_engine.so"
    check_sha "$ENGINE_ICU_SHA256" "$runtime/flutter/data/icudtl.dat"
    check_elf "$runtime/flutter/lib/libflutter_engine.so"
    for file in "$runtime/wm/deniald" "$runtime/wm/denialctl" "$runtime/wm/"*.so "$runtime/wm/shell/lib/"*.so; do
        check_elf "$file"
    done
    run "$runtime/wm/deniald" --help
    reported=$("$runtime/wm/deniald" --version) || die 'version inspection failed'
    [[ $reported == *"$DENIAL_VERSION"* ]] || die "stale Denial binary: $reported"
    [[ $(cat "$BUILD/package-check/usr/share/denial/version") == "$DENIAL_VERSION" ]] || die 'wrong release identity file'
    if [[ $ANLAND_WITH_DE == 1 ]]; then
        for file in "$runtime/desktop/native/"*; do
            [[ $file == *.json ]] || check_elf "$file"
        done
        for app in settings:denial-settings polkit:denial-app plugins:denial-plugin-manager; do
            check_elf "$runtime/desktop/${app%%:*}/lib/libapp.so"
            run "$runtime/desktop/${app%%:*}/${app#*:}" --bundle "$runtime/desktop/${app%%:*}" \
                --engine "$runtime/flutter/lib/libflutter_engine.so" --check
        done
        run "$runtime/desktop/native/denial-plugins" --help
    fi
    fedora_xwayland
    fedora_finish
}
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    case ${1:-} in -h|--help) sed -n '2,4p' "$0"; exit 0 ;; esac
    HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
    export ANLAND_DENIAL_RECIPE_DIR="$HERE" ANLAND_DENIAL_DISTRO=fedora43
    export WORKDIR=${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/denial-anland-fedora43}
    export ARTIFACTS_DIR=${ARTIFACTS_DIR:-$HERE/artifacts}
    exec bash "$HERE/../Arch_v5/build.sh" "$@"
fi
