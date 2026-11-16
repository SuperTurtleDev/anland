#!/bin/bash
#
# build.sh — rebuild the patched Mutter and XWayland .deb packages and install them.
#
# Run this INSIDE the container (e.g. `droidspaces -n kde run` or a shell there).
# It uses sudo for the privileged steps (apt / dpkg), so it works whether you
# are root or an ordinary user with sudo rights.
#
# The Mutter patch enables the Anland backend, and the sibling 'mutter/'
# directory contains the backend files copied into the source tree. Mutter and
# XWayland both follow the latest source versions available to APT.
#
# You can override the patch locations with MUTTER_PATCH=... and
# XWAYLAND_PATCH=... ./build.sh.
#
set -u

# ---- sudo helper (no-op if already root) -----------------------------------
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
source "$ROOT/producers/common/latest-source.sh"
WORKDIR="${WORKDIR:-$HOME/anland-debbuild}"
JOBS="${JOBS:-$(nproc)}"

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
        if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
            $SUDO sed -i 's/^Types: deb$/Types: deb deb-src/' \
                /etc/apt/sources.list.d/ubuntu.sources
        elif [ -f /etc/apt/sources.list ]; then
            $SUDO sed -i 's/^deb \(.*\)$/deb \1\ndeb-src \1/' /etc/apt/sources.list
        fi
    fi
    $SUDO apt-get update -qq || warn "apt-get update reported issues"
}

# ---- build one source package with an optional overlay and patch ------------
build_pkg() {
    local src="$1" patch="$2" overlay_dir="${3:-}" sentinel="${4:-}"
    local result prepared version source_label source_identity build_root tree

    result=$(anland_prepare_latest_apt_source "$src" "$WORKDIR" "$patch" "${ANLAND_OFFLINE:-0}") \
        || die "cannot prepare latest $src source"
    IFS=$'\t' read -r prepared version <<< "$result"
    [ -d "$prepared" ] && [ -n "$version" ] || die "invalid latest $src source result"
    source_identity=$(cat "$prepared/.anland-source-identity") || die "cannot identify latest $src source"
    source_label="$version"

    log "Installing build dependencies for '$src' $source_label"
    $SUDO apt-get build-dep -y "$src=$version" \
        || warn "build-dep for $src had issues; continuing"

    build_root=$(mktemp -d "$WORKDIR/build-$src-${source_identity:0:12}.XXXXXX") || die "cannot create $src build workspace"
    tree="$build_root/$src"
    cp -a "$prepared" "$tree" || die "cannot copy latest $src source"

    if [ -n "$overlay_dir" ]; then
        [ -d "$overlay_dir" ] || die "overlay directory not found: $overlay_dir"
        log "Overlaying '$overlay_dir' -> $tree (overwrite-merge)"
        # Follow overlay symlinks so the staged source contains real backend files.
        # -L is required: the backend tree is reached through overlay links, and a
        # plain -a would stage dangling links instead of the shared sources.
        cp -aL "$overlay_dir/." "$tree/" \
            || die "failed to stage the backend overlay for $src"
    fi

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
    local built_version changes debs deb
    local files=()
    built_version="$(dpkg-parsechangelog -l "$tree/debian/changelog" -S Version)" \
        || die "cannot read built version for $src"
    built_version="${built_version#*:}" # Debian filenames omit the epoch.
    changes="$build_root/${src}_${built_version}_$(dpkg --print-architecture).changes"
    [ -f "$changes" ] || die "missing .changes for $src: $changes"
    debs="$(awk '/^Files:/ {in_files=1; next} in_files && /^[^ ]/ {exit} in_files && $5 ~ /\.deb$/ {print $5}' "$changes")"
    [ -n "$debs" ] || die "no .deb listed in $changes"
    while IFS= read -r deb; do
        [[ "$deb" == *.deb && "$deb" != */* ]] || die "unsafe package name in $changes"
        [ -f "$build_root/$deb" ] || die "missing package $deb"
        files+=("$build_root/$deb")
    done <<< "$debs"
    printf '%s\n' "${files[@]}"
    $SUDO dpkg --force-confdef --force-confold -i "${files[@]}" \
        || die "dpkg -i for $src failed"
}

# ---------------------------------------------------------------------------
main() {
    local mutter_patch xwayland_patch
    mutter_patch="$(find_patch mutter.patch "${MUTTER_PATCH:-}")" \
        || die "mutter.patch not found (set MUTTER_PATCH=... to override)"
    xwayland_patch="$(find_patch xwayland.patch "${XWAYLAND_PATCH:-}")" \
        || die "xwayland.patch not found (set XWAYLAND_PATCH=... to override)"

    log "mutter source  : latest available to APT"
    log "mutter.patch   : $mutter_patch"
    log "xwayland.patch : $xwayland_patch"
    log "xwayland source: latest available"
    log "work dir       : $WORKDIR"

    ensure_deb_src

    build_pkg mutter "$mutter_patch" "$SCRIPT_DIR/mutter" \
        'have_anland = get_option'
    build_pkg xwayland "$xwayland_patch" '' \
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
