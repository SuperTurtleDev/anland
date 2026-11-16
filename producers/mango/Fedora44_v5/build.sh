#!/usr/bin/env bash
# Fedora 44 aarch64: private Mango/wlroots/SceneFX + native patched Xwayland RPMs.
# ANLAND_STAGE=check|prepare|build ANLAND_WITH_DEPS=1 JOBS=4 ANLAND_INSTALL=0
set -uo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
source "$SCRIPT_DIR/../../common/latest-source.sh"
# Fedora build/package functions are local to this WM recipe.
fedora_die() { printf 'fedora-build: %s\n' "$*" >&2; exit 1; }

fedora_run() { "$@" || fedora_die "failed: $*"; }

fedora_preflight() {
    local ID VERSION_ID
    source /etc/os-release || fedora_die 'cannot identify distribution'
    [[ $ID == fedora && $VERSION_ID == 44 && $(uname -m) == aarch64 ]] ||
        fedora_die 'requires native Fedora 44 aarch64'
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

fedora_initialize() {
    fedora_preflight
    ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd -P) || fedora_die 'repository root'
    source "$ROOT/producers/common/fedora-latest-srpm.sh"
    WORKDIR=$(realpath -m -- "${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-$1-fedora44}") || fedora_die 'cache path'
    ARTIFACTS_DIR=$(realpath -m -- "${ARTIFACTS_DIR:-${ARTIFACTS:-$SCRIPT_DIR/artifacts}}") || fedora_die 'artifact path'
    [[ $WORKDIR != / && $WORKDIR != "$HOME" && $WORKDIR != "$ROOT" && $WORKDIR != "$ROOT/"* ]] || fedora_die 'WORKDIR must be a separate cache'
    [[ $ARTIFACTS_DIR != / && $ARTIFACTS_DIR != "$HOME" && $ARTIFACTS_DIR != "$ROOT" && $ARTIFACTS_DIR != "$WORKDIR" ]] || fedora_die 'unsafe artifact directory'
    [[ ! -L $ARTIFACTS_DIR ]] || fedora_die 'artifacts must not be a symlink'
    fedora_run mkdir -p "$WORKDIR" "$ARTIFACTS_DIR"
    exec 7>"$WORKDIR/fedora-build.lock" || fedora_die 'cache lock'
    flock -n 7 || fedora_die 'cache already in use'
    exec 6>"$ARTIFACTS_DIR/.fedora-build.lock" || fedora_die 'artifact lock'
    flock -n 6 || fedora_die 'artifacts already in use'
    export LANG=C.UTF-8 LC_ALL=C.UTF-8 GIT_TERMINAL_PROMPT=0 GIT_PAGER=cat
    export CCACHE_DIR="$WORKDIR/ccache" CCACHE_BASEDIR="$WORKDIR"
    # Do not inherit another WM's staged libraries or SDK.
    unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CMAKE_PREFIX_PATH LD_LIBRARY_PATH DESTDIR
    FEDORA_INPUT_ID=$(python3 - "$ROOT" "$SCRIPT_DIR" <<'PY'
import hashlib, pathlib, sys
root, recipe = map(pathlib.Path, sys.argv[1:])
h = hashlib.sha256()
paths = [recipe/'build.sh']
for folder in (recipe, recipe.parent/'anland_backend', root/'libdisplay_producer', root/'common'):
    paths += [p for p in folder.rglob('*') if p.is_file() and 'artifacts' not in p.parts]
for p in sorted(set(paths)):
    h.update(str(p.relative_to(root)).encode()+b'\0'+p.read_bytes())
print(h.hexdigest())
PY
    ) || fedora_die 'input fingerprint'
    packages=()
}

fedora_dependencies() {
    [[ $ANLAND_WITH_DEPS == 1 && $ANLAND_STAGE != check ]] || return 0
    local dnf_download_plugin=dnf-plugins-core
    if command -v dnf5 >/dev/null 2>&1; then dnf_download_plugin=dnf5-plugins; fi
    # Preserve Mesa/package holds. Do not enable COPR or change global DNF config.
    fedora_run sudo dnf --setopt=install_weak_deps=False install -y \
        gcc gcc-c++ make cmake meson ninja-build pkgconf-pkg-config ccache python3 \
        git-core curl ca-certificates patch tar xz unzip zip file binutils \
        rpm-build rpmdevtools "$dnf_download_plugin" cpio util-linux findutils diffutils "$@"
}

fedora_download() {
    local file=$1 sha=$2 url=$3
    [[ $sha =~ ^[0-9a-f]{64}$ ]] || fedora_die "invalid checksum: $file"
    if printf '%s  %s\n' "$sha" "$file" | sha256sum -c --status - 2>/dev/null; then return; fi
    [[ $ANLAND_OFFLINE == 0 ]] || fedora_die "offline source absent or corrupt: $file"
    fedora_run mkdir -p "$(dirname -- "$file")"
    fedora_run curl -fL --retry 3 --connect-timeout 30 -o "$file.part" "$url"
    printf '%s  %s\n' "$sha" "$file.part" | sha256sum -c - || fedora_die "source checksum: $file"
    fedora_run mv -- "$file.part" "$file"
}

fedora_reuse() {
    [[ -d $1 ]] || return 1
    [[ -f $1/.anland-source-identity && $(cat "$1/.anland-source-identity") == "$FEDORA_INPUT_ID" ]] ||
        fedora_die "stale/unmanaged source preserved: $1; refresh it explicitly"
}

fedora_seal() { printf '%s\n' "$FEDORA_INPUT_ID" > "$1/.anland-source-identity" || fedora_die 'cannot record source identity'; }

fedora_git() {
    local name=$1 url=$2 patch=$3 revision=${4:-} result tree commit
    result=$(anland_prepare_latest_git_source "$name" "$url" "$WORKDIR" "$patch" "$ANLAND_OFFLINE" "$FEDORA_INPUT_ID" shallow "$revision") ||
        fedora_die "cannot prepare latest $name source"
    IFS=$'\t' read -r tree commit <<< "$result"
    printf '%s\t%s\n' "$tree" "$commit"
}

fedora_patch() {
    printf '%s  %s\n' "$2" "$3" | sha256sum -c --status - || fedora_die "patch checksum: $3"
    fedora_run patch --batch --forward --fuzz=2 --no-backup-if-mismatch -p1 -d "$1" -i "$3"
}

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
Native Fedora 44 runtime built from current upstream sources. Installing this package
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
    printf '%s\n' "${packages[@]##*/}" > "$WORKDIR/packages.list" || fedora_die 'package list'
    (cd "$ARTIFACTS_DIR" && sha256sum -- "${packages[@]##*/}" > "$WORKDIR/SHA256SUMS" && sha256sum -c "$WORKDIR/SHA256SUMS") || fedora_die 'package checksums'
    python3 - "$ROOT" "$FEDORA_INPUT_ID" "$WORKDIR/build-inputs.json" <<'PY' || fedora_die 'build record'
import json,subprocess,sys
root,identity,out=sys.argv[1:]
record={'distribution':'fedora44','architecture':'aarch64','input_sha256':identity,
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

[[ $# == 0 ]] || fedora_die 'use environment variables, not arguments'
fedora_initialize mango
fedora_dependencies pipewire-devel libinput-devel systemd-devel libseat-devel \
    libdisplay-info-devel libglvnd-devel mesa-libgbm-devel libdrm-devel libxkbcommon-devel wayland-devel \
    wayland-protocols-devel pixman-devel libxcb-devel xcb-util-wm-devel xcb-util-errors-devel \
    cjson-devel pango-devel pcre2-devel hwdata-devel libX11-devel libXcursor-devel \
    xorg-x11-server-Xwayland-devel patchelf
# Fedora names and lib64 are handled here; the backend remains single-source.
if [[ $ANLAND_STAGE == check ]]; then
    printf 'FEDORA44_RECIPE_OK mango (no compilation or graphical test)\n'; exit 0
fi
fedora_run mkdir -p "$WORKDIR/src" "$WORKDIR/build"
declare -A source_tree=() build_dir=()
python3 - "$SCRIPT_DIR/sources.json" > "$WORKDIR/sources.tsv" <<'PY'
import json,sys
v=json.load(open(sys.argv[1]))
for name,e in v['sources'].items(): print(name,e['url'],v['patches'][name+'.patch'],e.get('revision',''),sep='\t')
PY
[[ $? == 0 ]] || fedora_die 'source lock'
while IFS=$'\t' read -r name url patchsha revision; do
    printf '%s  %s\n' "$patchsha" "$SCRIPT_DIR/$name.patch" | sha256sum -c --status - || fedora_die "patch checksum: $name"
    result=$(fedora_git "$name" "$url" "$SCRIPT_DIR/$name.patch" "$revision") || fedora_die "source: $name"
    IFS=$'\t' read -r tree commit <<< "$result"
    source_tree[$name]=$tree
    if [[ ! -f $tree/.anland-fedora-overlay-id ]]; then
        if [[ $name == mango ]]; then
            fedora_run mkdir -p "$tree/anland_backend"
            fedora_run cp -aL "$SCRIPT_DIR/mango/." "$tree/anland_backend/"
            fedora_run install -m755 "$SCRIPT_DIR/startup.sh" "$tree/scripts/mango-anland"
        fi
        printf '%s\n' "$FEDORA_INPUT_ID" > "$tree/.anland-fedora-overlay-id" || fedora_die 'cannot record source overlay'
    fi
done < "$WORKDIR/sources.tsv"
source_set=""
for name in mango wlroots scenefx; do source_set+="$(cat "${source_tree[$name]}/.anland-source-identity")\n"; done
FEDORA_INPUT_ID=$(printf '%s\n%s\n' "$FEDORA_INPUT_ID" "$(printf '%s' "$source_set" | sha256sum | awk '{print $1}')" | sha256sum | awk '{print $1}') || fedora_die 'source set identity'
if [[ $ANLAND_STAGE == prepare ]]; then fedora_xwayland; printf 'FEDORA44_PREPARED mango\n'; exit 0; fi
for requirement in 'wayland-server >= 1.24' 'wayland-protocols >= 1.47' \
    'libdrm >= 2.4.129' 'xkbcommon >= 1.11' 'pixman-1 >= 0.43'; do
    fedora_run pkg-config --print-errors --exists "$requirement"
done
PREFIX=/usr/lib/mango-anland
STAGE="$WORKDIR/stage"
fedora_run mkdir -p "$STAGE"
export PATH="$STAGE$PREFIX/bin:$PATH" PKG_CONFIG_PATH="$STAGE$PREFIX/lib/pkgconfig:$STAGE$PREFIX/share/pkgconfig"
export CMAKE_PREFIX_PATH="$STAGE$PREFIX" LD_LIBRARY_PATH="$STAGE$PREFIX/lib"
stage_pc() {
    local pc
    while IFS= read -r -d '' pc; do fedora_run sed -i "s|=$PREFIX|=$STAGE$PREFIX|g" "$pc";
    done < <(find "$STAGE$PREFIX" -name '*.pc' -print0)
}
fedora_run cmake -S "$ROOT" -B "$WORKDIR/build/public" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON "-DCMAKE_INSTALL_RPATH=$PREFIX/lib"
fedora_run cmake --build "$WORKDIR/build/public" --target display_producer --parallel "$JOBS"
fedora_run env DESTDIR="$STAGE" cmake --install "$WORKDIR/build/public/libdisplay_producer"
stage_pc
meson_component() {
    local name=$1 identity build; shift
    identity=$(cat "${source_tree[$name]}/.anland-source-identity") || fedora_die "source identity: $name"
    build="$WORKDIR/build/$name-${identity:0:16}"
    build_dir[$name]=$build
    local -a setup=()
    [[ ! -f $build/meson-private/coredata.dat ]] || setup+=(--reconfigure)
    fedora_run meson setup "${setup[@]}" "$build" "${source_tree[$name]}" \
        --prefix="$PREFIX" --libdir=lib --buildtype=release --wrap-mode=nodownload "$@"
    fedora_run meson compile -C "$build" -j "$JOBS"
    fedora_run env DESTDIR="$STAGE" meson install -C "$build" --no-rebuild
    stage_pc
}
meson_component wlroots -Dexamples=false -Dbackends=drm,libinput -Drenderers=gles2 \
    -Dallocators=gbm -Dsession=enabled -Dlibliftoff=disabled -Dxwayland=enabled
meson_component scenefx -Dexamples=false -Dcolor-management=disabled
meson_component mango --sysconfdir=/etc/mango-anland -Danland=enabled -Dxwayland=enabled
package="$WORKDIR/runtime"
fedora_run mkdir -p "$package"
fedora_run env DESTDIR="$package" cmake --install "$WORKDIR/build/public/libdisplay_producer"
for name in wlroots scenefx mango; do fedora_run env DESTDIR="$package" meson install -C "${build_dir[$name]}" --no-rebuild; done
fedora_run rm -rf -- "$package$PREFIX/lib/systemd" "$package$PREFIX/share/wayland-sessions"
fedora_run mv "$package/etc/mango-anland/mango/"* "$package/etc/mango-anland/"
fedora_run rmdir "$package/etc/mango-anland/mango"
fedora_run install -Dm755 "$SCRIPT_DIR/startup.sh" "$package/usr/bin/mango-anland"
printf '#!/bin/sh\nexport LD_LIBRARY_PATH=/usr/lib/mango-anland/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\nexec /usr/lib/mango-anland/bin/mmsg "$@"\n' > "$package/usr/bin/mmsg-anland" || fedora_die 'mmsg wrapper'
fedora_run chmod 755 "$package/usr/bin/mmsg-anland"
while IFS= read -r -d '' file; do
    [[ $(head -c4 "$file") == $'\x7fELF' ]] || continue
    fedora_run patchelf --set-rpath "$PREFIX/lib" "$file"
    linkage=$(env LD_LIBRARY_PATH="$package$PREFIX/lib" ldd "$file") || fedora_die 'ELF dependencies'
    [[ $linkage != *'not found'* ]] || fedora_die "$linkage"
done < <(find "$package$PREFIX" -type f -print0)
fedora_run install -Dm644 "$ROOT/LICENSE" "$package/usr/share/licenses/mango-anland/anland-LICENSE"
for name in mango wlroots scenefx; do
    while IFS= read -r -d '' file; do
        fedora_run install -Dm644 "$file" "$package/usr/share/licenses/mango-anland/$name/${file##*/}"
    done < <(find "${source_tree[$name]}" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \) -print0)
done
MANGO_VERSION=$(meson introspect --projectinfo "${build_dir[mango]}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])') || fedora_die 'cannot read Mango source version'
fedora_stage_rpm mango-anland "$MANGO_VERSION" 'GPL-3.0-only AND MIT' "$package" \
    'bash, procps-ng, util-linux, xorg-x11-server-Xwayland, xkeyboard-config, hwdata'
fedora_xwayland
fedora_finish
