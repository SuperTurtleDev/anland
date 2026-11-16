#!/bin/bash
#
# build.sh — build patched KWin RPMs from the current Fedora SRPM.
# BUILD_XWAYLAND=1 also rebuilds Xwayland; ANLAND_INSTALL=1 opts into installation.
# ANLAND_LOCK_PACKAGES=1 explicitly opts into DNF exclusions after installation.
# Default: JOBS=4, no package installation or compositor/session edits.
# Build tooling and builddep may still change installed build dependencies.
#
# Fedora 44 counterpart of the Debian/Ubuntu build.sh. Uses dnf download --source
# + rpmbuild instead of apt-get source + dpkg-buildpackage.
#
# Run this INSIDE a Fedora 44 container. It uses sudo for privileged steps.
#
# The two patches fix hardware acceleration / input on the kgsl(turnip) stack:
#   kwin.patch      -> src 'kwin'      (anland backend + --anland CLI option)
#   xwayland.patch  -> src 'xorg-x11-server-Xwayland'  (kgsl GBM fixes)
#
# The anland backend source is overlaid into the kwin source tree before build
# via a dynamically numbered Source tarball declared in the spec.
#
set -uo pipefail
source /etc/os-release || { printf 'Cannot identify distribution\n' >&2; exit 1; }
[[ $ID == fedora && $VERSION_ID == 44 && $(uname -m) == aarch64 && $(id -u) != 0 ]] || {
    printf 'Requires native Fedora 44 aarch64, ordinary user\n' >&2; exit 1;
}

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/.cache/anland-fedora44-rpmbuild}"
JOBS="${JOBS:-4}"
# Build only by default; installation and Xwayland rebuilding are explicit.
ANLAND_INSTALL="${ANLAND_INSTALL:-0}"
ANLAND_LOCK_PACKAGES="${ANLAND_LOCK_PACKAGES:-0}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-0}"
KWIN_SOURCE="${KWIN_SOURCE:-kwin}"
DNF_IGNORE_EXCLUDES_OPTS=() # Preserve installed driver/package holds during dependency setup.
# The official Fedora container may not define this optional third-party repo;
# DNF treats disabling an unknown repo ID as a fatal error.
if dnf repolist --all 2>/dev/null | awk '$1 == "google-chrome" { found=1 } END { exit !found }'; then
    DNF_IGNORE_EXCLUDES_OPTS+=(--disable-repo=google-chrome)
fi

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
    local download_dir topdir srpm source_name source_version source_release sourcepackage
    download_dir="$(mktemp -d "$WORKDIR/${src}-source.XXXXXX")" || die "mktemp failed"

    log "Installing build tools"
    local dnf_plugin=dnf-plugins-core
    command -v dnf5 >/dev/null 2>&1 && dnf_plugin=dnf5-plugins
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" install -y --setopt=install_weak_deps=False \
        "$dnf_plugin" rpmdevtools rpm-build patch tar xz ninja-build ccache \
        || die "build tools installation failed"

    log "Fetching the current source for '$source'"
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

    local -a sources
    mapfile -t sources < <(find "$download_dir" -maxdepth 1 -name '*.src.rpm' -type f)
    [ "${#sources[@]}" -eq 1 ] || die "expected exactly one SRPM for $source"
    srpm="${sources[0]}"
    rpm -K "$srpm" || die "SRPM verification failed"
    IFS='|' read -r source_name source_version source_release sourcepackage < <(
        rpm -qp --qf '%{NAME}|%{VERSION}|%{RELEASE}|%{SOURCEPACKAGE}\n' "$srpm"
    ) || die "cannot inspect $srpm"
    [ "$source_name" = "$src" ] && [ "$sourcepackage" = 1 ] \
        || die "unexpected source package identity: $source_name-$source_version-$source_release"
    LAST_BUILT_TOPDIR="$(mktemp -d "$WORKDIR/${src}-${source_version}-${source_release}.XXXXXX")" \
        || die "cannot create SRPM build directory"
    topdir="$LAST_BUILT_TOPDIR"

    log "Installing build dependencies for '$src'"
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" builddep -y "$srpm" \
        || die "dnf builddep failed for $srpm"

    log "Unpacking SRPM: $srpm"
    mkdir -p "$topdir"/{SOURCES,SPECS,BUILD,BUILDROOT,RPMS,SRPMS} || die "mkdir failed"
    rpm -ivh --define "_topdir $topdir" "$srpm" || die "rpm -ivh failed for $srpm"

    local spec="$topdir/SPECS/$src.spec"
    [ -f "$spec" ] || die "could not find $spec"
    log "Spec file: $spec"
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
        sed -i '/^%autosetup -p1$/a mkdir -p src/backends/anland \&\& tar xf %{_sourcedir}/anland-overlay.tar -C src/backends/anland' "$spec" || die "overlay insertion failed"
    fi

    # Keep the distro patch series in %autosetup; apply Anland with GNU patch
    # afterwards so git_am cannot require exact context for our patch.
    local patch_source
    patch_source="$(sed -nE 's/^[[:space:]]*Source([0-9]*):.*/\1/p' "$spec" | sort -n | tail -n1)"
    patch_source=$(( ${patch_source:-0} + 1 ))
    grep -q '^Source0:' "$spec" || die "missing Source0 in $spec"
    sed -i "/^Source0:/a Source${patch_source}: $patchbase" "$spec" || die "patch source insertion failed"
    sed -i "/^%autosetup/a patch --batch --forward --fuzz=2 --no-backup-if-mismatch -p1 < %{SOURCE${patch_source}}" "$spec" \
        || die "patch application insertion failed"

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
    # Keep the public artifact layout stable for the package workflow.  The
    # SRPM topdir has a random suffix and is an implementation detail; callers
    # collect KWin packages from $WORKDIR/kwin/.
    local artifact_dir="$WORKDIR/$src"
    mkdir -p "$artifact_dir" || die "cannot create artifact directory"
    for rpm_file in "${rpms[@]}"; do
        rpm -K "$rpm_file" || die "RPM validation failed"
        cp "$rpm_file" "$topdir/" || die "RPM collection failed"
        cp "$rpm_file" "$artifact_dir/" || die "RPM artifact collection failed"
    done
    sha256sum "${rpms[@]}" || die "hashing failed"

    if [ "$ANLAND_INSTALL" = 1 ]; then
        log "Installing built .rpm(s) for '$src' (explicitly requested)"
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
            $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" --setopt=excludepkgs= install -y "${rpms[@]}" \
                || die "dnf install failed for $src"
        fi
        if [ "${#installed_rpms[@]}" -gt 0 ]; then
            $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" --setopt=excludepkgs= reinstall -y "${installed_rpms[@]}" \
                || die "dnf reinstall failed for $src"
        fi
        if [ "$ANLAND_LOCK_PACKAGES" = 1 ]; then dnf_lock_pkg "$src" || die "package locking failed"; fi
    else
        log "Build only: tools/build dependencies may change installed packages; DNF locks unchanged"
    fi
}
main() {
    for flag in ANLAND_INSTALL ANLAND_LOCK_PACKAGES BUILD_XWAYLAND; do
        [[ ${!flag} =~ ^[01]$ ]] || die "$flag must be 0 or 1"
    done
    [[ $ANLAND_LOCK_PACKAGES == 0 || $ANLAND_INSTALL == 1 ]] || die 'package locks require explicit installation'
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
    mkdir -p "$WORKDIR" || die "cannot create WORKDIR"
    WORKDIR="$(cd "$WORKDIR" && pwd -P)" || die "cannot resolve WORKDIR"
    local repository_root
    repository_root=$(cd "$SCRIPT_DIR/../../.." && pwd -P) || die 'cannot resolve repository root'
    [[ $WORKDIR != / && $WORKDIR != "$HOME" && $WORKDIR != "$repository_root" && $WORKDIR != "$repository_root/"* ]] || die 'WORKDIR must be a separate persistent cache'
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
        build_pkg_rpm xorg-x11-server-Xwayland "$xwl_patch" "" xorg-x11-server-Xwayland
        # Keep the historical artifact directory for explicit Xwayland builds.
        mkdir -p "$WORKDIR/xwayland" || die "mkdir failed"
        find "$LAST_BUILT_TOPDIR" -maxdepth 1 -name '*.rpm' ! -name '*.src.*' -exec cp {} "$WORKDIR/xwayland/" \; || die "copy failed"
    fi

    log "Done. Packages are under $WORKDIR/kwin/ (ANLAND_INSTALL=$ANLAND_INSTALL)."
    echo "No compositor was restarted; /etc/environment was not modified."
}

main "$@"
