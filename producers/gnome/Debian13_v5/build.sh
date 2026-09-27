#!/bin/bash
#
# build.sh — rebuild the patched Mutter and XWayland .deb packages and install them.
#
# Run this INSIDE the container (e.g. `droidspaces -n kde run` or a shell there).
# It uses sudo for the privileged steps (apt / dpkg), so it works whether you
# are root or an ordinary user with sudo rights.
#
# The Mutter patch enables the Anland backend, and the sibling 'mutter/'
# directory contains the backend files copied into the source tree. The Mutter
# source version is pinned below because the patch targets that Ubuntu package
# revision. XWayland follows the latest source version available to apt.
#
# You can override the patch locations with MUTTER_PATCH=... and
# XWAYLAND_PATCH=... ./build.sh.
#
set -u

MUTTER_VERSION='48.7-0+deb13u1'

# ---- sudo helper (no-op if already root) -----------------------------------
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/anland-debbuild}"
JOBS="$(nproc)"

# ---- locate a patch file by name, regardless of where it lives -------------
find_patch() {
    # $1 = base name to look for (e.g. mutter.patch)
    local name="$1" explicit="${2:-}"
    if [ -n "$explicit" ] && [ -f "$explicit" ]; then
        printf '%s\n' "$explicit"; return 0
    fi
    local c
    for c in "$SCRIPT_DIR/$name" "./$name" "$SCRIPT_DIR/../$name"; do
        if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
    done
    # last resort: search a couple of likely roots
    local hit
    hit="$(find "$SCRIPT_DIR" "$PWD" -maxdepth 3 -name "$name" -type f 2>/dev/null | head -1)"
    if [ -n "$hit" ]; then printf '%s\n' "$hit"; return 0; fi
    return 1
}

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

# ---- ensure deb-src entries exist so `apt source` works --------------------
ensure_deb_src() {
    if ! $SUDO grep -rqsE '^Types:.*deb-src|^deb-src ' \
            /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
        log "Enabling deb-src repositories"
        if [ -f /etc/apt/sources.list.d/debian.sources ]; then
            $SUDO sed -i 's/^Types: deb$/Types: deb deb-src/' \
                /etc/apt/sources.list.d/debian.sources
        elif [ -f /etc/apt/sources.list ]; then
            $SUDO sed -i 's/^deb \(.*\)$/deb \1\ndeb-src \1/' /etc/apt/sources.list
        fi
    fi
    $SUDO apt-get update -qq || warn "apt-get update reported issues"
}

# ---- build one source package with an optional overlay and patch ------------
build_pkg() {
    local src="$1" patch="$2" version="${3:-}" overlay_dir="${4:-}" \
        sentinel="${5:-}" source_spec="$1" source_label

    if [ -n "$version" ]; then
        source_spec="$src=$version"
        source_label="$version"
    else
        source_label='latest available'
    fi

    log "Installing build dependencies for '$src' ($source_label)"
    $SUDO apt-get build-dep -y "$source_spec" \
        || warn "build-dep for $src had issues; continuing"

    log "Fetching source for '$src' ($source_label)"
    rm -rf "${WORKDIR:?}/$src"
    mkdir -p "$WORKDIR/$src"
    ( cd "$WORKDIR/$src" && apt-get source "$source_spec" ) \
        || die "apt-get source $src failed"

    local tree
    tree="$(find "$WORKDIR/$src" -maxdepth 1 -type d -name "${src}-*" | head -1)"
    [ -n "$tree" ] || die "could not find unpacked source tree for $src"

    if [ -n "$overlay_dir" ]; then
        [ -d "$overlay_dir" ] || die "overlay directory not found: $overlay_dir"
        log "Overlaying '$overlay_dir' -> $tree (overwrite-merge)"
        # Follow overlay symlinks so the staged source contains real backend
        # files: the overlay reaches the shared device layer and protocol header
        # through nested relative links, which would dangle once copied.
        cp -aL "$overlay_dir/." "$tree/" \
            || die "failed to stage the backend overlay for $src"
    fi

    log "Applying patch: $patch -> $tree"
    ( cd "$tree" && patch --batch -p1 --forward --reject-file=- < "$patch" ) \
        || die "patch did not apply cleanly for $src"

    log "Building '$src' $source_label (.deb)"
    # -d: don't re-check build-deps (already installed above)
    # -b -uc -us: binary only, unsigned. changelog untouched -> official version.
    ( cd "$tree" && DEB_BUILD_OPTIONS="nocheck parallel=$JOBS" \
        dpkg-buildpackage -b -uc -us -d ) \
        || die "dpkg-buildpackage failed for $src"

    if [ "${ANLAND_INSTALL:-0}" != 1 ]; then
        log "Build only: not installing $src"
        return 0
    fi
    log "Installing built .deb(s) for '$src'"
    local debs
    debs="$(find "$WORKDIR/$src" -maxdepth 1 -name '*.deb' -type f)"
    [ -n "$debs" ] || die "no .deb produced for $src"
    printf '%s\n' "$debs"
    # shellcheck disable=SC2086
    $SUDO dpkg --force-confdef --force-confold -i $debs \
        || die "dpkg -i for $src failed"
}

# ---------------------------------------------------------------------------
main() {
    local mutter_patch xwayland_patch
    mutter_patch="$(find_patch mutter.patch "${MUTTER_PATCH:-}")" \
        || die "mutter.patch not found (set MUTTER_PATCH=... to override)"
    xwayland_patch="$(find_patch xwayland.patch "${XWAYLAND_PATCH:-}")" \
        || die "xwayland.patch not found (set XWAYLAND_PATCH=... to override)"

    log "mutter version : $MUTTER_VERSION"
    log "mutter.patch   : $mutter_patch"
    log "xwayland.patch : $xwayland_patch"
    log "xwayland source: latest available"
    log "work dir       : $WORKDIR"

    ensure_deb_src

    build_pkg mutter "$mutter_patch" "$MUTTER_VERSION" "$SCRIPT_DIR/mutter" \
        'have_anland = get_option'
    build_pkg xwayland "$xwayland_patch" '' '' \
        'No usable linux-dmabuf main device'

    if [ "${ANLAND_INSTALL:-0}" != 1 ]; then
        log "Done. Built packages only; global environment unchanged."
        return 0
    fi
    $SUDO sed -i '/PULSE_SERVER=unix:\/tmp\/.pulse-socket/d' /etc/environment \
        || die "failed to update /etc/environment"

    log "Done. Patched Mutter and XWayland built and installed."
    echo "Built packages are under: $WORKDIR/{mutter,xwayland}/"
    echo "Restart the compositor session for the changes to take effect."
}

main "$@"
