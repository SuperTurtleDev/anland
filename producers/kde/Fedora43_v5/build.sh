#!/bin/bash
#
# build.sh — build patched KWin RPMs (Fedora 43 / KWin 6.7.5).
# BUILD_XWAYLAND=1 also rebuilds Xwayland; ANLAND_INSTALL=1 opts into installation.
# ANLAND_LOCK_PACKAGES=1 explicitly opts into DNF exclusions after installation.
# Default: JOBS=4, no package installation or compositor/session edits.
# Build tooling and builddep may still change installed build dependencies.
#
# Fedora 43 counterpart of the Debian/Ubuntu build.sh. Uses dnf download --source
# + rpmbuild instead of apt-get source + dpkg-buildpackage.
#
# Run this INSIDE a Fedora 43 container. It uses sudo for privileged steps.
#
# The two patches fix hardware acceleration / input on the kgsl(turnip) stack:
#   kwin.patch      -> src 'kwin'      (anland backend + --anland CLI option)
#   xwayland.patch  -> src 'xorg-x11-server-Xwayland'  (kgsl GBM fixes)
#
# The anland backend source is overlaid into the kwin source tree before build
# via a dynamically numbered Source tarball declared in the spec.
#
set -u

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/.cache/anland-fedora43-rpmbuild}"
JOBS="${JOBS:-4}"
# Build only by default; installation and Xwayland rebuilding are explicit.
ANLAND_INSTALL="${ANLAND_INSTALL:-0}"
ANLAND_LOCK_PACKAGES="${ANLAND_LOCK_PACKAGES:-0}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-0}"
KWIN_SOURCE="${KWIN_SOURCE:-kwin-6.7.5-1.fc43}"
DNF_IGNORE_EXCLUDES_OPTS=(--setopt=exclude= --disable-repo=google-chrome)

find_patch() {
    local name="$1" explicit="${2:-}"
    if [ -n "$explicit" ] && [ -f "$explicit" ]; then
        printf '%s\n' "$explicit"; return 0
    fi
    local c
    for c in "$SCRIPT_DIR/$name" "./$name" "$SCRIPT_DIR/../$name"; do
        if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
    done
    local hit
    hit="$(find "$SCRIPT_DIR" "$PWD" -maxdepth 3 -name "$name" -type f 2>/dev/null | head -1)"
    if [ -n "$hit" ]; then printf '%s\n' "$hit"; return 0; fi
    return 1
}

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

dnf_lock_pkg() {
    local src="$1"
    local token="${src}*"

    if $SUDO sed -n 's/^exclude=//p' /etc/dnf/dnf.conf 2>/dev/null | tr ' ' '\n' | grep -Fxq "$token"; then
        return 0
    fi

    $SUDO grep -q '^exclude=' /etc/dnf/dnf.conf 2>/dev/null \
        && $SUDO sed -i "s/^exclude=.*/& ${token}/" /etc/dnf/dnf.conf \
        || echo "exclude=${token}" | $SUDO tee -a /etc/dnf/dnf.conf >/dev/null
}

build_pkg_rpm() {
    local src="$1" patch="$2" overlay_dir="$3" source="${4:-$1}"
    local topdir="$WORKDIR/$src" download_dir
    download_dir="$(mktemp -d "$WORKDIR/${src}-source.XXXXXX")" || die "mktemp failed"

    log "Installing build tools"
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" install -y --setopt=install_weak_deps=False \
        dnf-plugins-core rpmdevtools rpm-build patch tar xz ninja-build ccache \
        || die "build tools installation failed"

    log "Fetching source for '$source'"
    local cached_srpm=""
    if [ "$src" = kwin ]; then
        cached_srpm="${KWIN_SRPM:-}"
    elif [ "$src" = xorg-x11-server-Xwayland ]; then
        cached_srpm="${XWAYLAND_SRPM:-}"
    fi
    if [ -n "$cached_srpm" ]; then
        [ -f "$cached_srpm" ] || die "cached SRPM does not exist: $cached_srpm"
        cp "$cached_srpm" "$download_dir/" || die "copy cached SRPM failed"
    else
        dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" download --source --destdir="$download_dir" "$source" \
            || die "dnf download --source $source failed"
    fi

    local srpm
    local -a sources
    mapfile -t sources < <(find "$download_dir" -maxdepth 1 -name '*.src.rpm' -type f)
    [ "${#sources[@]}" -eq 1 ] || die "expected exactly one SRPM for $source"
    srpm="${sources[0]}"
    rpm -K "$srpm" || die "SRPM verification failed"

    log "Installing build dependencies for '$src'"
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" builddep -y "$srpm" \
        || die "dnf builddep failed for $srpm"

    log "Unpacking SRPM: $srpm"
    mkdir -p "$topdir"/{SOURCES,SPECS,BUILD,BUILDROOT,RPMS,SRPMS} || die "mkdir failed"
    rpm -ivh --define "_topdir $topdir" "$srpm" \
        || die "rpm -ivh failed for $srpm"

    local spec="$topdir/SPECS/$src.spec"
    [ -f "$spec" ] || die "could not find $spec"
    log "Spec file: $spec"
    # Only accept the layout reviewed for Fedora 43. Fail on a changed SRPM.
    if [ "$src" = kwin ]; then
        [ "$(grep -c '^%autosetup -p1$' "$spec")" = 1 ] || die "unsupported KWin %prep layout"
    else
        grep -q '^%autosetup -S git_am -n ' "$spec" || die "unsupported Xwayland %prep layout"
    fi

    local patchbase
    patchbase="$(basename "$patch")"
    cp "$patch" "$topdir/SOURCES/" || die "copy patch failed"

    if [ -n "$overlay_dir" ]; then
        [ -f "$overlay_dir/CMakeLists.txt" ] || die "backend overlay missing"
        log "Packing self-contained backend overlay"
        local overlay_tar="anland-overlay.tar" source_num
        source_num="$(sed -nE 's/^Source([0-9]+):.*/\1/p' "$spec" | sort -n | tail -1)"
        source_num=$(( ${source_num:-0} + 1 ))
        tar chf "$topdir/SOURCES/$overlay_tar" -C "$overlay_dir" . || die "packing overlay failed"
        sed -i "/^Source0:/a Source${source_num}: $overlay_tar" "$spec" || die "Source insertion failed"
        # %autosetup already changed into the source root. RPM 6 adds another
        # build-subdirectory level; do not reconstruct it from %{_builddir}.
        sed -i '/^%autosetup -p1$/a mkdir -p src/backends/anland \&\& tar xf %{_sourcedir}/anland-overlay.tar -C src/backends/anland' "$spec" || die "overlay insertion failed"
    fi

    local first_source_line patch_num
    first_source_line=$(grep -n "^Source0:" "$spec" | head -1 | cut -d: -f1)
    if [ -n "$first_source_line" ]; then
        # Fedora specs can already define Patch0 (Fedora 44 does), so append
        # our patch at the next available numeric slot instead of colliding.
        patch_num="$(sed -nE 's/^[[:space:]]*Patch[[:space:]]*([0-9]+)[[:space:]]*:.*/\1/p' "$spec" | sort -n | tail -n1)"
        if [ -n "$patch_num" ]; then
            patch_num=$((patch_num + 1))
        else
            patch_num=0
        fi
        log "Adding $patchbase as Patch$patch_num"
        sed -i "${first_source_line}a Patch${patch_num}: $patchbase" "$spec"
    fi

    local rel_line
    rel_line="$(grep -m1 '^Release:' "$spec")"
    if ! echo "$rel_line" | grep -q 'anland'; then
        sed -i "s/^Release: \(.*\)%{?dist}/Release: \1.anland%{?dist}/" "$spec"
    fi
    log "Modified Release: $(grep '^Release:' "$spec")"

    log "Building '$src' (.rpm), jobs=$JOBS"
    local stamp
    stamp="$(mktemp "$topdir/build-start.XXXXXX")" || die "mktemp failed"
    rpmbuild --define "_topdir $topdir" --define "_smp_build_ncpus $JOBS" -bb "$spec" \
        || die "rpmbuild failed for $src"

    log "Collecting newly built .rpm(s) for '$src'"
    local -a rpms
    mapfile -t rpms < <(find "$topdir/RPMS" -name '*.rpm' -type f ! -name '*debug*' -newer "$stamp")
    [ "${#rpms[@]}" -gt 0 ] || die "no new .rpm produced for $src"
    printf '%s\n' "${rpms[@]}"
    for rpm_file in "${rpms[@]}"; do
        rpm -K "$rpm_file" || die "RPM validation failed"
        cp "$rpm_file" "$topdir/" || die "RPM collection failed"
    done
    sha256sum "${rpms[@]}" || die "hashing failed"

    if [ "$ANLAND_INSTALL" = 1 ]; then
        log "Installing built .rpm(s) for '$src' (explicitly requested)"
        # dnf install may skip an RPM with an already installed identical NEVRA.
        # Reinstall existing names so the newly built files are actually deployed.
        local -a installed_rpms=() fresh_rpms=()
        local rpm_file rpm_name
        for rpm_file in "${rpms[@]}"; do
            rpm_name="$(rpm -qp --qf '%{NAME}' "$rpm_file")" || die "cannot inspect $rpm_file"
            if [ "$(rpm -q --qf '%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}' "$rpm_name" 2>/dev/null)" = \
                 "$(rpm -qp --qf '%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}' "$rpm_file")" ]; then
                installed_rpms+=("$rpm_file")
            else
                fresh_rpms+=("$rpm_file")
            fi
        done
        if [ "${#fresh_rpms[@]}" -gt 0 ]; then
            # One transaction keeps interdependent subpackages consistent.
            $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" install -y "${rpms[@]}" \
                || die "dnf install failed for $src"
        fi
        if [ "${#installed_rpms[@]}" -gt 0 ]; then
            $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" reinstall -y "${installed_rpms[@]}" \
                || die "dnf reinstall failed for $src"
        fi
        if [ "$ANLAND_LOCK_PACKAGES" = 1 ]; then
            dnf_lock_pkg "$src" || die "package locking failed"
        fi
    else
        log "Build only: tools/build dependencies may change installed packages; DNF locks unchanged"
    fi
}

main() {
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
    mkdir -p "$WORKDIR" || die "cannot create WORKDIR"
    WORKDIR="$(cd "$WORKDIR" && pwd -P)" || die "cannot resolve WORKDIR"
    local kwin_patch xwl_patch
    kwin_patch="$(find_patch kwin.patch "${KWIN_PATCH:-}")" \
        || die "kwin.patch not found (set KWIN_PATCH=... to override)"

    log "kwin.patch     : $kwin_patch"
    log "work dir       : $WORKDIR"
    if [ "${SKIP_KWIN:-0}" != 1 ]; then
        build_pkg_rpm kwin "$kwin_patch" "$SCRIPT_DIR/kwin/src/backends/anland" "$KWIN_SOURCE"
    fi
    if [ "$BUILD_XWAYLAND" = 1 ]; then
        xwl_patch="$(find_patch xwayland.patch "${XWAYLAND_PATCH:-}")" \
            || die "xwayland.patch not found"
        build_pkg_rpm xorg-x11-server-Xwayland "$xwl_patch" ""
        # Keep the historical artifact directory for explicit Xwayland builds.
        mkdir -p "$WORKDIR/xwayland" || die "mkdir failed"
        find "$WORKDIR/xorg-x11-server-Xwayland" -maxdepth 1 -name '*.rpm' ! -name '*.src.*' -exec cp {} "$WORKDIR/xwayland/" \; || die "copy failed"
    fi

    log "Done. Packages are under $WORKDIR/kwin/ (ANLAND_INSTALL=$ANLAND_INSTALL)."
    echo "No compositor was restarted; /etc/environment was not modified."
}

main "$@"