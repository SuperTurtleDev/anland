#!/bin/bash
#
# build.sh — rebuild patched kwin and Xwayland .deb packages and install them.
#
# Run this INSIDE the container (e.g. `droidspaces -n kde run` or a shell there).
# It uses sudo for the privileged steps (apt / dpkg), so it works whether you
# are root or an ordinary user with sudo rights.
#
# The two patches fix hardware acceleration / input on the kgsl(turnip) stack:
#   kwin.patch      -> src 'kwin'      (wayland backend coordinate scaling)
#   xwayland.patch  -> src 'xwayland'  (kgsl GBM: NULL main_dev fallback +
#                                       implicit-modifier wl_buffer creation)
#
# The official package version from debian/changelog is kept untouched, so the
# resulting .deb reinstalls cleanly over the distro package without confusing
# apt about versions.
#
# Patch files are located automatically: they may sit next to this script or in
# the current directory, under either name (kwin.patch / xwayland.patch). You
# can also point at them explicitly:  KWIN_PATCH=... XWAYLAND_PATCH=... ./build.sh
#
set -u

# ---- sudo helper (no-op if already root) -----------------------------------
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/anland-debbuild}"
JOBS="${JOBS:-$(nproc)}"

# ---- locate a patch file by name, regardless of where it lives -------------
find_patch() {
    # $1 = base name to look for (e.g. kwin.patch)
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
        if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
            $SUDO sed -i 's/^Types: deb$/Types: deb deb-src/' \
                /etc/apt/sources.list.d/ubuntu.sources
        elif [ -f /etc/apt/sources.list ]; then
            $SUDO sed -i 's/^deb \(.*\)$/deb \1\ndeb-src \1/' /etc/apt/sources.list
        fi
    fi
    $SUDO apt-get update -qq || warn "apt-get update reported issues"
}

# ---- build one source package with one patch -------------------------------
# $1 = source package name, $2 = patch file, $3 = sentinel grep to confirm patch
build_pkg() {
    local src="$1" patch="$2"

    local tree=""
    if [ "$src" = kwin ] && [ -n "${KWIN_SOURCE_TREE:-}" ]; then
        tree="$(realpath "$KWIN_SOURCE_TREE")"
        [ -f "$tree/debian/control" ] || die "KWIN_SOURCE_TREE lacks debian/control: $tree"
        mkdir -p "$WORKDIR/$src"
        log "Reusing prepared KWin source: $tree"
    else
        log "Installing build dependencies for '$src'"
        $SUDO apt-get build-dep -y "$src" || die "build-dep for $src failed"
        log "Fetching source for '$src'"
        mkdir -p "$WORKDIR/$src"
        ( cd "$WORKDIR/$src" && apt-get source "$src" ) \
            || die "apt-get source $src failed"
        tree="$(find "$WORKDIR/$src" -maxdepth 1 -type d -name "${src}-*" | head -1)"
        [ -n "$tree" ] || die "could not find unpacked source tree for $src"
    fi
    ( cd "$tree" && dpkg-checkbuilddeps ) || die "build dependencies missing for $src"

    # ---- overlay: copy local overrides into the source tree if present ------
    local overlay_dir="$SCRIPT_DIR/$src"
    if [ -d "$overlay_dir" ]; then
        log "Overlaying '$overlay_dir' -> $tree (overwrite-merge)"
        # -L dereferences the libdisplay_producer symlinks so the merged tree
        # is self-contained (relative links only resolve inside the checkout).
        cp -aL "$overlay_dir/." "$tree/" \
            || die "failed to stage the backend overlay for $src"
    fi

    log "Applying patch: $patch -> $tree"
    if [ "$src" = kwin ] && [ -n "${KWIN_SOURCE_TREE:-}" ]; then
        # Validate every hunk without changing the prepared source.
        ( cd "$tree" && patch --batch --force --dry-run -R -p1 < "$patch" ) \
            || die "prepared KWin source does not contain the complete kwin.patch"
        log "Prepared source already contains kwin.patch"
    else
        ( cd "$tree" && patch --batch -p1 --forward --reject-file=- < "$patch" ) \
            || die "patch did not apply cleanly for $src"
    fi

    log "Building '$src' (.deb, keeping official version)"
    # -d: don't re-check build-deps (already installed above)
    # -b -uc -us: binary only, unsigned. changelog untouched -> official version.
    ( cd "$tree" && DEB_BUILD_OPTIONS="nocheck parallel=$JOBS" \
        dpkg-buildpackage -b -uc -us -d -nc ) \
        || die "dpkg-buildpackage failed for $src"

    if [ "${ANLAND_INSTALL:-0}" != 1 ]; then
        log "Build only: not installing $src"
        return 0
    fi
    log "Installing built .deb(s) for '$src'"
    local debs
    local version="$(dpkg-parsechangelog -l "$tree/debian/changelog" -S Version)"
    version="${version#*:}" # Debian package filenames omit the epoch.
    local changes="$(dirname "$tree")/${src}_${version}_$(dpkg --print-architecture).changes"
    [ -f "$changes" ] || die "no current .changes file produced for $src: $changes"
    debs="$(awk '/^Files:/ {in_files=1; next} in_files && /^[^ ]/ {exit} in_files && $5 ~ /\.deb$/ {print $5}' "$changes")"
    [ -n "$debs" ] || die "no .deb listed in $changes"
    local files=() deb
    while IFS= read -r deb; do
        [[ "$deb" == *.deb && "$deb" != */* ]] || die "unsafe package name in $changes"
        [ -f "$(dirname "$tree")/$deb" ] || die "missing package $deb"
        files+=("$(dirname "$tree")/$deb")
    done <<< "$debs"
    printf '%s\n' "${files[@]}"
    $SUDO dpkg -i "${files[@]}" || die "dpkg -i for $src failed"
}

# ---------------------------------------------------------------------------
main() {
    local kwin_patch xwl_patch
    kwin_patch="$(find_patch kwin.patch "${KWIN_PATCH:-}")" \
        || die "kwin.patch not found (set KWIN_PATCH=... to override)"
    xwl_patch="$(find_patch xwayland.patch "${XWAYLAND_PATCH:-}")" \
        || die "xwayland.patch not found (set XWAYLAND_PATCH=... to override)"

    log "kwin.patch     : $kwin_patch"
    log "xwayland.patch : $xwl_patch"
    log "work dir       : $WORKDIR"

    if [ "${KWIN_ONLY:-0}" != 1 ] || [ -z "${KWIN_SOURCE_TREE:-}" ]; then
        ensure_deb_src
    fi

    # sentinels: a distinctive literal string introduced by each patch
    # (no regex metacharacters, so plain grep matches it verbatim).
    build_pkg kwin     "$kwin_patch"
    if [ "${KWIN_ONLY:-0}" != 1 ]; then
        build_pkg xwayland "$xwl_patch"
    fi
    
    if [ "${ANLAND_INSTALL:-0}" != 1 ]; then
        log "Done. Built packages only; global environment unchanged."
        return 0
    fi
    $SUDO sed -i '/PULSE_SERVER=unix:\/tmp\/.pulse-socket/d' /etc/environment \
        || die "failed to update /etc/environment"

    # plasma-wayland.service imports this file before starting the compositor.
    # Keep the fast path here as well as in startup.sh, which is used by the
    # manual launch path.
    $SUDO sed -i '/^ANLAND_SKIP_IMPLICIT_SYNC_WAIT=/d' /etc/environment \
        || die "failed to update /etc/environment"
    printf '%s\n' 'ANLAND_SKIP_IMPLICIT_SYNC_WAIT=1' | $SUDO tee -a /etc/environment > /dev/null \
        || die "failed to update /etc/environment"

    log "Done. Patched KWin${KWIN_ONLY:+ (KWIN_ONLY)} built and installed."
    echo "Built packages are under: ${KWIN_SOURCE_TREE:-$WORKDIR/kwin} parent directory."
    echo "Restart the compositor session for the changes to take effect."
}

main "$@"
