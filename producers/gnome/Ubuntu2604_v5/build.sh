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

MUTTER_VERSION='50.1-0ubuntu2.2'

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

# Ubuntu may remove an older source version from the current Sources index
# while the corresponding files remain available in the archive pool. Keep the
# build pinned to MUTTER_VERSION instead of silently switching to a newer
# package revision whose patch may no longer apply.
fetch_archived_source() {
    local src="$1" version="$2" dest="$3"
    local base="${MUTTER_SOURCE_POOL:-http://ports.ubuntu.com/ubuntu-ports/pool/main/m/mutter}"
    local dsc="${src}_${version}.dsc"
    local dsc_path="$dest/$dsc"
    local file

    log "Fetching archived source: $src $version"
    curl --fail --location --retry 2 --connect-timeout 15 --max-time 300 \
        -o "$dsc_path" "$base/$dsc" \
        || return 1

    # The .dsc is authoritative for the exact orig/debian archive names.
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        curl --fail --location --retry 2 --connect-timeout 15 --max-time 300 \
            -o "$dest/$file" "$base/$file" \
            || return 1
    done <<EOF
$(awk '
    /^Files:/ { in_files=1; next }
    in_files && /^Checksums-/ { exit }
    in_files && /^[[:space:]]*[0-9A-Fa-f]+[[:space:]]+[0-9]+[[:space:]]+[^[:space:]]+$/ { print $3 }
' "$dsc_path")
EOF

    ( cd "$dest" && dpkg-source -x "$dsc" )
}

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
    if ! ( cd "$WORKDIR/$src" && apt-get source "$source_spec" ); then
        if [ "$src" = mutter ] && [ -n "$version" ]; then
            warn "APT has no exact $source_spec; using the pinned Ubuntu archive source"
            fetch_archived_source "$src" "$version" "$WORKDIR/$src" \
                || die "could not fetch archived source $source_spec"
        else
            die "apt-get source $src failed"
        fi
    fi

    local tree
    tree="$(find "$WORKDIR/$src" -maxdepth 1 -type d -name "${src}-*" | head -1)"
    [ -n "$tree" ] || die "could not find unpacked source tree for $src"

    if [ -n "$overlay_dir" ]; then
        [ -d "$overlay_dir" ] || die "overlay directory not found: $overlay_dir"
        log "Overlaying '$overlay_dir' -> $tree (overwrite-merge)"
        # Follow overlay symlinks so the staged source contains real backend files.
        # -L is required: the backend tree is reached through overlay links, and a
        # plain -a would stage dangling links instead of the shared sources.
        cp -aL "$overlay_dir/." "$tree/" \
            || die "failed to stage the backend overlay for $src"
    fi

    log "Applying patch: $patch -> $tree"
    # The source was just unpacked, so the patch must apply completely. Accepting
    # a partial application (a sentinel string elsewhere in the tree) would build
    # a tree where later hunks never landed, which is far worse than failing here.
    if ! ( cd "$tree" && patch --batch -p1 --forward --reject-file=- < "$patch" ); then
        die "patch did not apply cleanly for $src"
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
    changes="$WORKDIR/$src/${src}_${built_version}_$(dpkg --print-architecture).changes"
    [ -f "$changes" ] || die "missing .changes for $src: $changes"
    debs="$(awk '/^Files:/ {in_files=1; next} in_files && /^[^ ]/ {exit} in_files && $5 ~ /\.deb$/ {print $5}' "$changes")"
    [ -n "$debs" ] || die "no .deb listed in $changes"
    while IFS= read -r deb; do
        [[ "$deb" == *.deb && "$deb" != */* ]] || die "unsafe package name in $changes"
        [ -f "$WORKDIR/$src/$deb" ] || die "missing package $deb"
        files+=("$WORKDIR/$src/$deb")
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