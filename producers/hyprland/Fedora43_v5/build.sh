#!/usr/bin/env bash
# Fedora 43 aarch64: current upstream Hyprland stack and Hyprgrass, private RPM.
# ANLAND_STAGE=check|prepare|build ANLAND_WITH_DEPS=1 JOBS=4 ANLAND_INSTALL=0
set -uo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
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

fedora_initialize() {
    fedora_preflight
    ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd -P) || fedora_die 'repository root'
    source "$ROOT/producers/common/fedora-latest-srpm.sh"
    source "$ROOT/producers/common/latest-source.sh"
    WORKDIR=$(realpath -m -- "${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-$1-fedora43}") || fedora_die 'cache path'
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
paths = [recipe/'build.sh', root/'producers/common/fedora-latest-srpm.sh',
         root/'producers/common/latest-source.sh']
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

fedora_reuse() {
    [[ -d $1 ]] || return 1
    [[ -f $1/.anland-source-identity && $(cat "$1/.anland-source-identity") == "$FEDORA_INPUT_ID" ]] ||
        fedora_die "stale/unmanaged source preserved: $1; refresh it explicitly"
}

fedora_seal() { printf '%s\n' "$FEDORA_INPUT_ID" > "$1/.anland-source-identity" || fedora_die 'cannot record source identity'; }

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

[[ $# == 0 ]] || fedora_die 'use environment variables, not arguments'
fedora_initialize hyprland
fedora_dependencies pipewire-devel cairo-devel pango-devel libdrm-devel libxkbcommon-devel libuuid-devel \
    libglvnd-devel mesa-libgbm-devel libinput-devel libseat-devel libdisplay-info-devel systemd-devel \
    libxcb-devel xcb-util-wm-devel xcb-util-errors-devel libXcursor-devel libXcomposite-devel \
    libXfixes-devel libXrender-devel libX11-devel wayland-devel wayland-protocols-devel \
    libevdev-devel libei-devel lcms2-devel lua-devel muParser-devel pixman-devel re2-devel \
    tomlplusplus-devel openssl-devel udis86-devel hwdata-devel \
    glslang-devel glm-devel libjpeg-turbo-devel libwebp-devel file-devel libspng-devel \
    librsvg2-devel libjxl-devel libheif-devel libzip-devel patchelf pugixml-devel xxd
if [[ $ANLAND_STAGE == check ]]; then
    printf 'FEDORA43_RECIPE_OK hyprland (private current-source stack; no compilation)\n'; exit 0
fi
fedora_run mkdir -p "$WORKDIR/src" "$WORKDIR/downloads" "$WORKDIR/latest-sources"
python3 - "$SCRIPT_DIR/sources.json" > "$WORKDIR/sources.tsv" <<'PY'
import json,sys
v=json.load(open(sys.argv[1]))
for group in ('sources','plugins'):
    for name,e in v[group].items(): print(name,e['url'],e.get('revision',''),sep='\t')
PY
[[ $? == 0 ]] || fedora_die 'source URLs'
declare -A upstream_trees upstream_commits
source_records=()
while IFS=$'\t' read -r name url revision; do
    [[ -n $name && -n $url ]] || fedora_die 'invalid source URL record'
    source_result=$(anland_prepare_latest_git_source "$name" "$url" "$WORKDIR/latest-sources" '' "$ANLAND_OFFLINE" "$FEDORA_INPUT_ID" shallow "$revision") ||
        fedora_die "cannot fetch latest $name source"
    IFS=$'\t' read -r upstream_tree upstream_commit <<< "$source_result"
    [[ -d $upstream_tree && $upstream_commit =~ ^[0-9a-f]{40,64}$ ]] || fedora_die "invalid latest $name source identity"
    upstream_trees[$name]=$upstream_tree
    upstream_commits[$name]=$upstream_commit
    source_records+=("$name"$'\t'"$url"$'\t'"$upstream_commit")
done < "$WORKDIR/sources.tsv"
[[ ${#source_records[@]} == 12 ]] || fedora_die 'incomplete Hyprland source set'
source_set_id=$(printf '%s\n' "${source_records[@]}" | sha256sum | awk '{print $1}') || fedora_die 'source identity'
FEDORA_INPUT_ID=$(printf '%s\n%s\n' "$FEDORA_INPUT_ID" "$source_set_id" | sha256sum | awk '{print $1}') || fedora_die 'source input identity'
SOURCE_ROOT="$WORKDIR/src/$FEDORA_INPUT_ID"
BUILD_ROOT="$WORKDIR/build/$FEDORA_INPUT_ID"
if ! fedora_reuse "$SOURCE_ROOT"; then
    [[ ! -e $SOURCE_ROOT && ! -L $SOURCE_ROOT ]] || fedora_die "incomplete source tree preserved: $SOURCE_ROOT"
    fedora_run mkdir -p "$SOURCE_ROOT"
    for name in "${!upstream_trees[@]}"; do
        tree="$SOURCE_ROOT/$name"
        fedora_run mkdir -p "$tree"
        git -C "${upstream_trees[$name]}" archive --format=tar HEAD | tar -xf - -C "$tree" || fedora_die "cannot unpack current $name source"
        if [[ $name == aquamarine || $name == hyprland ]]; then
            fedora_run cp -aL "$SCRIPT_DIR/../anland_backend/$name/." "$tree/"
            if [[ $name == aquamarine ]]; then
                fedora_run mkdir -p "$tree/src/backend/anland/public"
                fedora_run cp -aL "$ROOT/common" "$ROOT/libdisplay_producer" "$tree/src/backend/anland/public/"
            fi
        fi
        if [[ -f $SCRIPT_DIR/$name.patch ]]; then
            patchsha=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["patches"][sys.argv[2]+".patch"])' "$SCRIPT_DIR/sources.json" "$name") || fedora_die 'patch hash'
            fedora_patch "$tree" "$patchsha" "$SCRIPT_DIR/$name.patch"
        fi
        fedora_seal "$tree"
    done
    fedora_run mkdir -p "$SOURCE_ROOT/hyprgrass/subprojects/wf-touch"
    fedora_run cp -a "$SOURCE_ROOT/wf-touch/." "$SOURCE_ROOT/hyprgrass/subprojects/wf-touch/"
    printf '%s\n' "$FEDORA_INPUT_ID" > "$SOURCE_ROOT/.anland-wf-touch" || fedora_die 'plugin source receipt'
    fedora_seal "$SOURCE_ROOT"
fi
fedora_run mkdir -p "$BUILD_ROOT"
if [[ $ANLAND_STAGE == prepare ]]; then fedora_xwayland; printf 'FEDORA43_PREPARED hyprland\n'; exit 0; fi
PREFIX=/usr/lib64/hyprland-anland
STAGE="$BUILD_ROOT/stage"
TOOLS="$BUILD_ROOT/tools"
fedora_run mkdir -p "$STAGE" "$TOOLS"
export PATH="$TOOLS/bin:$STAGE$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$TOOLS/lib/pkgconfig:$TOOLS/share/pkgconfig:$STAGE$PREFIX/lib/pkgconfig:$STAGE$PREFIX/share/pkgconfig"
export CMAKE_PREFIX_PATH="$TOOLS:$STAGE$PREFIX" LD_LIBRARY_PATH="$STAGE$PREFIX/lib"
# The source export has no .git directory, so pass the current upstream identity to CMake.
export GIT_COMMIT_HASH=${upstream_commits[hyprland]} GIT_TAG="git.${upstream_commits[hyprland]:0:12}"
# Build the current scanner as a private host tool, outside the runtime and SDK stages.
fedora_run cmake -S "$SOURCE_ROOT/hyprwayland-scanner" -B "$BUILD_ROOT/hyprwayland-scanner" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TOOLS" -DCMAKE_INSTALL_BINDIR=bin -DCMAKE_INSTALL_LIBDIR=lib
fedora_run cmake --build "$BUILD_ROOT/hyprwayland-scanner" --parallel "$JOBS"
fedora_run cmake --install "$BUILD_ROOT/hyprwayland-scanner"
[[ $(command -v hyprwayland-scanner) == "$TOOLS/bin/hyprwayland-scanner" ]] || fedora_die 'current scanner is not first in PATH'
scanner_version=$(hyprwayland-scanner --version) || fedora_die 'current scanner version query failed'
[[ -n $scanner_version ]] || fedora_die 'current scanner did not report its version'
printf 'FEDORA43_BUILD_TOOL hyprwayland-scanner %s (%s)\n' "$scanner_version" "$TOOLS/bin/hyprwayland-scanner"
# Keep protocol XML from the current upstream repository private, not in the runtime RPM.
fedora_run cmake -S "$SOURCE_ROOT/hyprland-protocols" -B "$BUILD_ROOT/hyprland-protocols" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$TOOLS" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_DATADIR=share
fedora_run cmake --build "$BUILD_ROOT/hyprland-protocols" --parallel "$JOBS"
fedora_run cmake --install "$BUILD_ROOT/hyprland-protocols"
fedora_run pkg-config --atleast-version=0.6.4 hyprland-protocols
[[ $(pkg-config --variable=pkgdatadir hyprland-protocols) -ef "$TOOLS/share/hyprland-protocols" ]] || fedora_die 'current protocol data is not first in pkg-config'
printf 'FEDORA43_BUILD_DATA hyprland-protocols %s (%s)\n' "$(pkg-config --modversion hyprland-protocols)" "$TOOLS/share/hyprland-protocols"
stage_pc() {
    local file
    while IFS= read -r -d '' file; do fedora_run sed -i "s|=$PREFIX|=$STAGE$PREFIX|g" "$file";
    done < <(find "$STAGE$PREFIX" -name '*.pc' -print0)
}
for name in hyprutils hyprlang hyprgraphics hyprwire hyprcursor glaze aquamarine hyprland; do
    args=(-S "$SOURCE_ROOT/$name" -B "$BUILD_ROOT/$name" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_INSTALL_INCLUDEDIR=include -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DBUILD_TESTING=OFF "-DCMAKE_INSTALL_RPATH=$PREFIX/lib" "-DCMAKE_BUILD_RPATH=$STAGE$PREFIX/lib")
    # Override a cached system-package location as well as the search prefix.
    if [[ $name == aquamarine || $name == hyprland ]]; then
        args+=("-Dhyprwayland-scanner_DIR=$TOOLS/lib/cmake/hyprwayland-scanner")
    fi
    [[ $name != glaze ]] || args+=(-Dglaze_BUILD_TESTS=OFF -Dglaze_BUILD_EXAMPLES=OFF)
    [[ $name != hyprland ]] || args+=(-DNO_HYPRPM=OFF -DBUILD_HYPRTESTER=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON)
    fedora_run cmake "${args[@]}"
    fedora_run cmake --build "$BUILD_ROOT/$name" --parallel "$JOBS"
    fedora_run env DESTDIR="$STAGE" cmake --install "$BUILD_ROOT/$name"
    stage_pc
done
setup=(); [[ ! -f $BUILD_ROOT/hyprgrass/meson-private/coredata.dat ]] || setup+=(--reconfigure)
fedora_run meson setup "${setup[@]}" "$BUILD_ROOT/hyprgrass" "$SOURCE_ROOT/hyprgrass" \
    --prefix="$PREFIX" --libdir=lib/hyprland/plugins --buildtype=release --wrap-mode=nodownload -Dtests=disabled
fedora_run meson compile -C "$BUILD_ROOT/hyprgrass" -j "$JOBS"
package="$BUILD_ROOT/runtime"
fedora_run mkdir -p "$package"
for name in hyprutils hyprlang hyprgraphics hyprwire hyprcursor glaze aquamarine hyprland; do
    fedora_run env DESTDIR="$package" cmake --install "$BUILD_ROOT/$name"
done
fedora_run env DESTDIR="$package" meson install -C "$BUILD_ROOT/hyprgrass" --no-rebuild
fedora_run rm -f -- "$package$PREFIX/lib/hyprland/plugins/libwftouch.a" "$package$PREFIX/lib/hyprland/plugins/libwf-touch.a"
fedora_run install -Dm755 "$SCRIPT_DIR/startup.sh" "$package/usr/bin/hyprland-anland"
for name in hyprctl hyprpm; do
    printf '#!/bin/sh\nexport LD_LIBRARY_PATH=/usr/lib64/hyprland-anland/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\nexec /usr/lib64/hyprland-anland/bin/%s "$@"\n' "$name" > "$package/usr/bin/$name-anland" || fedora_die 'private tool wrapper'
    fedora_run chmod 755 "$package/usr/bin/$name-anland"
done
for name in hyprutils hyprlang hyprgraphics hyprwire hyprcursor glaze aquamarine hyprland hyprgrass wf-touch; do
    while IFS= read -r -d '' file; do fedora_run install -Dm644 "$file" "$package/usr/share/licenses/hyprland-anland/$name/${file##*/}";
    done < <(find "$SOURCE_ROOT/$name" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \) -print0)
done
fedora_run install -Dm644 "$ROOT/LICENSE" "$package/usr/share/licenses/hyprland-anland/anland-LICENSE"
while IFS= read -r -d '' file; do
    [[ $(head -c4 "$file") == $'\x7fELF' ]] || continue
    fedora_run patchelf --set-rpath "$PREFIX/lib" "$file"
    linkage=$(env LD_LIBRARY_PATH="$package$PREFIX/lib" ldd "$file") || fedora_die 'Hyprland runtime linkage'
    [[ $linkage != *'not found'* ]] || fedora_die "$linkage"
done < <(find "$package$PREFIX" -type f -print0)
fedora_stage_rpm hyprland-anland "git.${upstream_commits[hyprland]:0:12}" 'BSD-3-Clause AND MIT AND GPL-3.0-or-later' "$package" \
    'bash, procps-ng, util-linux, dbus, xkeyboard-config, xorg-x11-server-Xwayland'
fedora_xwayland
fedora_finish
