#!/bin/bash
#
# build.sh — rebuild patched kwin and Xwayland .rpm packages and install them.
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
# via a Source1 tarball declared in the spec.
#
set -u

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/anland-rpmbuild}"
JOBS="$(nproc)"
DNF_IGNORE_EXCLUDES_OPTS=(--setopt=exclude=)

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
    local src="$1" token="${src}*"

    if $SUDO sed -n 's/^exclude=//p' /etc/dnf/dnf.conf 2>/dev/null | tr ' ' '\n' | grep -Fxq "$token"; then
        return 0
    fi

    $SUDO grep -q '^exclude=' /etc/dnf/dnf.conf 2>/dev/null \
        && $SUDO sed -i "s/^exclude=.*/& ${token}/" /etc/dnf/dnf.conf \
        || echo "exclude=${token}" | $SUDO tee -a /etc/dnf/dnf.conf >/dev/null
}

build_pkg_rpm() {
    local src="$1" patch="$2" overlay_dir="$3"

    log "Installing build tools"
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" install -y --setopt=install_weak_deps=False \
        dnf-plugins-core rpmdevtools rpm-build patch tar xz 2>/dev/null

    log "Fetching source for '$src'"
    rm -rf "${WORKDIR:?}/$src"
    mkdir -p "$WORKDIR/$src"
    ( cd "$WORKDIR/$src" && dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" download --source "$src" ) \
        || die "dnf download --source $src failed"

    local srpm
    srpm="$(find "$WORKDIR/$src" -maxdepth 1 -name '*.src.rpm' -type f | head -1)"
    [ -n "$srpm" ] || die "no .src.rpm produced for $src"

    log "Installing build dependencies for '$src'"
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" builddep -y "$srpm" \
        || die "dnf builddep failed for $srpm"

    log "Unpacking SRPM: $srpm"
    rpmdev-setuptree
    rm -f ~/rpmbuild/SPECS/*.spec
    rm -rf ~/rpmbuild/RPMS/* ~/rpmbuild/SRPMS/*
    rpm -ivh --force "$srpm" \
        || die "rpm -ivh failed for $srpm"

    local spec
    spec="$(find ~/rpmbuild/SPECS -maxdepth 1 -name '*.spec' -type f | head -1)"
    [ -n "$spec" ] || die "could not find .spec in ~/rpmbuild/SPECS"
    log "Spec file: $spec"

    local patchbase
    patchbase="$(basename "$patch")"
    cp "$patch" ~/rpmbuild/SOURCES/

    if [ -d "$overlay_dir" ]; then
        log "Packing overlay '$overlay_dir' into Source2 tarball"
        local overlay_tar="anland-overlay.tar"
        # --dereference packs symlink targets as real files so repo-relative
        # links (libdisplay_producer/) don't dangle inside the source RPM.
        tar --dereference -cf "$WORKDIR/$src/$overlay_tar" -C "$overlay_dir" .
        cp "$WORKDIR/$src/$overlay_tar" ~/rpmbuild/SOURCES/

        sed -i "/^Source1:/a Source2: anland-overlay.tar" "$spec"
        sed -i '/^%autosetup/a mkdir -p %{_builddir}/%{name}-%{version}/src/backends/anland \&\& tar xf %{_sourcedir}/anland-overlay.tar -C %{_builddir}/%{name}-%{version}/src/backends/anland/' "$spec"
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

    log "Building '$src' (.rpm)"
    rpmbuild -bb "$spec" \
        || die "rpmbuild failed for $src"

    log "Collecting built .rpm(s) for '$src'"
    local rpms
    rpms="$(find ~/rpmbuild/RPMS -name '*.rpm' -type f ! -name '*debug*' 2>/dev/null)"
    [ -n "$rpms" ] || die "no .rpm produced for $src"
    printf '%s\n' "$rpms"
    find ~/rpmbuild/RPMS -name '*.rpm' -type f ! -name '*debug*' -exec cp {} "$WORKDIR/$src/" \;

    log "Installing built .rpm(s) for '$src'"
    # shellcheck disable=SC2086
    $SUDO dnf "${DNF_IGNORE_EXCLUDES_OPTS[@]}" install -y --allowerasing $rpms || warn "dnf install for $src reported issues"

    log "Locking '$src' against dnf updates"
    dnf_lock_pkg "$src"
}

main() {
    local kwin_patch xwl_patch
    kwin_patch="$(find_patch kwin.patch "${KWIN_PATCH:-}")" \
        || die "kwin.patch not found (set KWIN_PATCH=... to override)"
    xwl_patch="$(find_patch xwayland.patch "${XWAYLAND_PATCH:-}")" \
        || die "xwayland.patch not found (set XWAYLAND_PATCH=... to override)"

    log "kwin.patch     : $kwin_patch"
    log "xwayland.patch : $xwl_patch"
    log "work dir       : $WORKDIR"

    build_pkg_rpm kwin "$kwin_patch" "$SCRIPT_DIR/kwin/src/backends/anland"
    build_pkg_rpm xorg-x11-server-Xwayland "$xwl_patch" ""
    # CI workflow expects xwayland output under a fixed name; copy RPMs there
    mkdir -p "$WORKDIR/xwayland"
    find "$WORKDIR/xorg-x11-server-Xwayland" -maxdepth 1 -name '*.rpm' ! -name '*.src.*' -exec cp {} "$WORKDIR/xwayland/" \; 2>/dev/null || true

    sed -i '/PULSE_SERVER=unix:\/tmp\/.pulse-socket/d' /etc/environment

    $SUDO touch /etc/environment
    $SUDO sed -i '/^ANLAND_SKIP_IMPLICIT_SYNC_WAIT=/d' /etc/environment
    printf '%s\n' 'ANLAND_SKIP_IMPLICIT_SYNC_WAIT=1' | $SUDO tee -a /etc/environment >/dev/null
    
    log "Done. Patched kwin and Xwayland built and installed."
    echo "Built packages are under: $WORKDIR/{kwin,xorg-x11-server-Xwayland}/"
    echo "Restart the compositor session for the changes to take effect."
}

main "$@"
