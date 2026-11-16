#!/usr/bin/env bash
# Ubuntu 26.04 (resolute) ARM64 Denial producer for Anland v5.
#
# The integration targets Denial main's v0.6.0 release commit:
# denial.patch carries the Anland backend registration, the Flutter-side
# Impeller fix-ups live in flutter-engine.patch, and the Anland backend body
# (denial_backend/) is copied from the shared producer layer, never forked.
# Flutter, Skia and depot_tools stay pinned by Denial's SOURCE_LOCK.json.
#
# Usage:
#   bash build.sh
#   ANLAND_WITH_DEPS=1 JOBS=8 bash build.sh
#   ANLAND_INSTALL=1 bash build.sh
#   DENIAL_COMMIT=<sha> bash build.sh          reproduce one Denial revision
#   ANLAND_OFFLINE=1 bash build.sh             use only cached inputs
#
# Environment:
#   WORKDIR              source and toolchain cache (default ~/.cache/anland-denial-ubuntu2604)
#   ARTIFACTS_DIR        package output (default ./artifacts)
#   JOBS                 parallel build jobs (default: nproc)
#   ANLAND_WITH_DEPS     install build dependencies with apt (0/1, default 0)
#   ANLAND_INSTALL       install the produced .debs afterwards (0/1, default 0)
#   ANLAND_OFFLINE       forbid network access (0/1, default 0)
#   DENIAL_COMMIT        build this Denial commit instead of resolving DENIAL_BRANCH
#   DENIAL_BRANCH        branch resolved when DENIAL_COMMIT is empty (default main)
#   DENIAL_PACKAGE_VERSION  override the generated package version
#   GLIBC_BASELINE       libc6 minimum written into package metadata (default: host glibc)
#
# Nothing in this script starts a graphical session, installs packages or
# restarts services unless ANLAND_INSTALL=1 is set explicitly. Start the
# desktop with startup.sh after the package is installed.
set -euo pipefail
export LC_ALL=C
umask 022

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
BACKEND="$SCRIPT_DIR/denial"

# shellcheck source=sources.conf
if [[ -r "$SCRIPT_DIR/sources.conf" ]]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/sources.conf"
fi
: "${DENIAL_URL:=https://github.com/denialwm/denial.git}"
: "${DENIAL_BRANCH:=main}"
: "${DENIAL_COMMIT:=}"
: "${DENIAL_PC_TOOL:=tools/denial-pc}"
: "${DENIAL_FLUTTER_TOOL:=tools/denial-flutter-engine}"
: "${DENIAL_STAGE_TOOL:=tools/stage-denial-runtime}"
: "${DENIAL_DEB_TOOL:=tools/package-denial-deb}"
: "${UBUNTU_CODENAME:=resolute}"
: "${XWAYLAND_PACKAGE:=xwayland}"
: "${XWAYLAND_VERSION:=2:24.1.10-1}"
: "${XWAYLAND_ANLAND_REVISION:=anland1}"

WORKDIR="${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-denial-ubuntu2604}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$PWD/artifacts}"
JOBS="${JOBS:-$(nproc)}"
ANLAND_OFFLINE="${ANLAND_OFFLINE:-0}"
ANLAND_WITH_DEPS="${ANLAND_WITH_DEPS:-0}"
ANLAND_INSTALL="${ANLAND_INSTALL:-0}"
ANLAND_SKIP_PLATFORM_CHECK="${ANLAND_SKIP_PLATFORM_CHECK:-0}"
GLIBC_BASELINE="${GLIBC_BASELINE:-$(getconf GNU_LIBC_VERSION | awk '{print $2}')}"

SRC="$WORKDIR/src/denial"
BUILD="$WORKDIR/build"
ENGINE_CACHE="${DENIAL_FLUTTER_ENGINE_CACHE_ROOT:-$WORKDIR/engine-cache}"
PACKAGE_INPUT="$BUILD/package-input"
RUST_TOOLCHAIN=''
DENIAL_REV=''
FLUTTER_REV=''
SKIA_REV=''
ENGINE_SHA256=''

log() { printf '\n[denial-ubuntu2604] %s\n' "$*" >&2; }
die() { printf 'denial-ubuntu2604: %s\n' "$*" >&2; exit 1; }

preflight() {
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
    for flag in ANLAND_OFFLINE ANLAND_WITH_DEPS ANLAND_INSTALL ANLAND_SKIP_PLATFORM_CHECK; do
        [[ "${!flag}" == 0 || "${!flag}" == 1 ]] || die "$flag must be 0 or 1"
    done
    if [[ "$ANLAND_SKIP_PLATFORM_CHECK" == 0 ]]; then
        local ID VERSION_ID
        # shellcheck disable=SC1091
        source /etc/os-release
        [[ "${ID:-}" == ubuntu && "${VERSION_ID:-}" == 26.04 ]] \
            || die "requires Ubuntu 26.04, found ${ID:-?} ${VERSION_ID:-?}"
        [[ "$(dpkg --print-architecture)" == arm64 ]] \
            || die 'requires an arm64 userland'
        [[ "$(id -u)" != 0 ]] || die 'build as an ordinary user; only dependency installation uses sudo'
    fi
    [[ -L "$BACKEND" ]] || die "backend entry is not the shared symlink: $BACKEND"
    for file in CMakeLists.txt mod.rs session.c session.h; do
        [[ -f "$BACKEND/$file" ]] || die "backend overlay is incomplete: $BACKEND/$file"
    done
    [[ -f "$REPO_ROOT/libdisplay_producer/CMakeLists.txt" ]] \
        || die "missing public producer layer below $REPO_ROOT"
    [[ -f "$REPO_ROOT/common/protocol.h" ]] \
        || die "missing common protocol below $REPO_ROOT"
    [[ -f "$SCRIPT_DIR/denial.patch" && -f "$SCRIPT_DIR/flutter-engine.patch" \
        && -f "$SCRIPT_DIR/xwayland.patch" ]] || die 'recipe patches are missing'
    mkdir -p "$WORKDIR/src" "$WORKDIR/state" "$BUILD" "$ARTIFACTS_DIR"
}

# Host tools are checked after install_dependencies: jq, bsdtar and rsync are
# installed by this recipe, so a clean CI container has none of them yet at the
# point preflight inspects the source tree.
check_tools() {
    local file
    for file in git jq python3 cmake ninja rsync sha256sum install patch tar xz bsdtar \
        dpkg-deb dpkg-buildpackage dpkg-parsechangelog dpkg-source flock; do
        command -v "$file" >/dev/null 2>&1 || die "missing required command: $file"
    done
}

install_dependencies() {
    [[ "$ANLAND_WITH_DEPS" == 1 ]] || return 0
    [[ "$ANLAND_OFFLINE" == 0 ]] || die 'ANLAND_WITH_DEPS=1 needs network access'
    command -v sudo >/dev/null 2>&1 || die 'sudo is required to install build dependencies'

    if ! apt-cache showsrc "$XWAYLAND_PACKAGE" >/dev/null 2>&1; then
        log 'enabling deb-src entries for the Xwayland source build'
        for sources in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list; do
            [[ -f "$sources" ]] || continue
            if grep -Eq '^Types:[[:space:]]*deb([[:space:]]|$)' "$sources"; then
                sudo sed -i -E 's/^Types:([[:space:]]*)(deb)([[:space:]]|$)/Types:\1\2 deb-src\3/' "$sources"
            fi
        done
        sudo apt-get update
    fi

    local -a packages=(
        build-essential ca-certificates cmake dpkg-dev git jq ninja-build pkg-config
        python3 python3-venv rsync libarchive-tools
        clang lld llvm libclang-dev
        libdrm-dev libegl-dev libgles-dev libgbm-dev libseat-dev libinput-dev
        libudev-dev libxkbcommon-dev libwayland-dev wayland-protocols
        libpixman-1-dev libdisplay-info-dev libpipewire-0.3-dev
        libpam0g-dev libsystemd-dev libdbus-1-dev libfontconfig-dev libfreetype-dev
        libglib2.0-dev libgtk-3-dev libpcre2-dev libffi-dev libexpat1-dev libssl-dev
        zlib1g-dev liblzma-dev libzstd-dev libblkid-dev
        libx11-dev libxext-dev libxrandr-dev libxrender-dev libxfixes-dev libxi-dev
        libxcursor-dev libxinerama-dev libxkbcommon-x11-dev
        mesa-common-dev libgl1-mesa-dev libglu1-mesa-dev libpulse-dev
        libpolkit-agent-1-dev libpolkit-gobject-1-dev
    )
    log 'installing build dependencies'
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends "${packages[@]}"
    sudo apt-get install -y --no-install-recommends rustup \
        || log 'rustup package unavailable; using an existing rustup installation'
    sudo apt-get build-dep -y "$XWAYLAND_PACKAGE"
}

fetch_denial() {
    local origin rev
    if [[ ! -d "$SRC/.git" ]]; then
        rm -rf -- "$SRC"
        git init --quiet "$SRC"
    fi
    origin="$(git -C "$SRC" remote get-url origin 2>/dev/null || true)"
    if [[ -z "$origin" ]]; then
        git -C "$SRC" remote add origin "$DENIAL_URL"
    elif [[ "$origin" != "$DENIAL_URL" ]]; then
        die "Denial checkout origin is $origin, expected $DENIAL_URL"
    fi

    if [[ -n "${DENIAL_COMMIT:-}" ]]; then
        rev="$DENIAL_COMMIT"
    elif [[ "$ANLAND_OFFLINE" == 1 ]]; then
        rev="$(cat "$WORKDIR/state/denial.commit" 2>/dev/null || true)"
        [[ -n "$rev" ]] || die 'offline build has no recorded Denial commit'
    else
        rev="$(git ls-remote "$DENIAL_URL" "refs/heads/$DENIAL_BRANCH" | awk 'NR == 1 { print $1 }')"
        [[ -n "$rev" ]] || die "cannot resolve $DENIAL_URL $DENIAL_BRANCH"
    fi
    [[ "$rev" =~ ^[0-9a-f]{40}$ ]] || die "invalid Denial revision: $rev"

    if ! git -C "$SRC" cat-file -e "$rev^{commit}" 2>/dev/null; then
        [[ "$ANLAND_OFFLINE" == 0 ]] \
            || die "offline build has no cached Denial commit $rev"
        log "fetching Denial $DENIAL_BRANCH $rev"
        git -C "$SRC" fetch --quiet --no-tags --depth=1 origin "$rev"
    fi
    # Version numbering needs the release tags and their ancestry. A shallow,
    # tag-less clone cannot name the release this build extends and the
    # package version collapses to 0.0.0.r1. Fetch only when they are absent;
    # a complete history also keeps offline builds reproducible.
    if [[ "$ANLAND_OFFLINE" == 0 ]] \
        && { [[ -z "$(git -C "$SRC" tag --list 'v*' | head -n 1)" ]] \
             || [[ "$(git -C "$SRC" rev-parse --is-shallow-repository)" == true ]]; }; then
        log 'completing the Denial history and release tags for version numbering'
        git -C "$SRC" fetch --quiet --tags --unshallow origin 2>/dev/null \
            || git -C "$SRC" fetch --quiet --tags origin
    fi
    git -C "$SRC" checkout --quiet --force --detach "$rev"
    git -C "$SRC" clean --quiet -fd

    # The revision stamp lives outside the checkout: git clean would remove it.
    local last_checkout="$WORKDIR/state/denial.checkout"
    if [[ -f "$last_checkout" && "$(< "$last_checkout")" != "$rev" ]]; then
        log 'Denial revision changed; dropping ignored build output from the previous checkout'
        git -C "$SRC" clean --quiet -fdx
    fi
    printf '%s\n' "$rev" > "$last_checkout"
    printf '%s\n' "$rev" > "$WORKDIR/state/denial.commit"
    [[ "$(git -C "$SRC" rev-parse HEAD)" == "$rev" ]] || die 'Denial checkout did not reach the requested commit'
    DENIAL_REV="$rev"
    if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
        SOURCE_DATE_EPOCH="$(git -C "$SRC" log -1 --format=%ct)"
    fi
    [[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || die 'SOURCE_DATE_EPOCH must be a Unix timestamp'
    export SOURCE_DATE_EPOCH

    # Resolve the latest release reachable from this revision. A later tag on
    # main must not change the version of a build pinned to an older commit.
    DENIAL_VERSION_BASE_TAG="${DENIAL_VERSION_BASE_TAG:-}"
    if [[ -z "$DENIAL_VERSION_BASE_TAG" ]]; then
        local candidate
        while IFS= read -r candidate; do
            if [[ "$candidate" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                DENIAL_VERSION_BASE_TAG="$candidate"
                break
            fi
        done < <(git -C "$SRC" tag --merged HEAD --list 'v*' --sort=-v:refname)
    fi
    if [[ -n "$DENIAL_VERSION_BASE_TAG" ]]; then
        git -C "$SRC" rev-parse --verify --quiet \
            "${DENIAL_VERSION_BASE_TAG}^{commit}" >/dev/null \
            || die "DENIAL_VERSION_BASE_TAG is not a commit: $DENIAL_VERSION_BASE_TAG"
        git -C "$SRC" merge-base "$DENIAL_VERSION_BASE_TAG" HEAD >/dev/null 2>&1 \
            || die "DENIAL_VERSION_BASE_TAG $DENIAL_VERSION_BASE_TAG shares no history with $rev"
        log "version base: $DENIAL_VERSION_BASE_TAG"
    else
        log 'no release tag found; the package version will be 0.0.0-based'
    fi
    log "Denial revision: $DENIAL_REV ($DENIAL_BRANCH)"
}

apply_denial_patch() {
    local patch="$SCRIPT_DIR/denial.patch"
    if ! git -C "$SRC" apply --check "$patch" 2>/dev/null; then
        if git -C "$SRC" apply --reverse --check "$patch" 2>/dev/null; then
            log 'denial.patch is already applied'
            return
        fi
        die "denial.patch does not apply to $DENIAL_REV. Upstream drifted; refresh the Anland integration patch (do not fuzz it)."
    fi
    git -C "$SRC" apply "$patch"
    log 'applied denial.patch'
}

overlay_backend() {
    local destination="$SRC/compositor/src/bin/deniald/anland_backend"
    mkdir -p "$destination"
    rsync -a --delete "$BACKEND/" "$destination/"
    [[ -f "$destination/mod.rs" ]] || die 'backend overlay copy failed'
    log 'overlaid the shared anland_backend sources'
}

prepare_rust() {
    local channel
    channel="$(python3 - "$SRC/rust-toolchain.toml" <<'PY'
import sys
import tomllib

with open(sys.argv[1], 'rb') as handle:
    print(tomllib.load(handle)['toolchain']['channel'])
PY
    )" || die 'cannot read the pinned Rust toolchain'
    [[ -n "$channel" ]] || die 'the pinned Rust toolchain channel is empty'
    export RUSTUP_HOME="${RUSTUP_HOME:-$WORKDIR/rustup}"
    export CARGO_HOME="${CARGO_HOME:-$WORKDIR/cargo}"
    export RUSTUP_TOOLCHAIN="$channel"
    command -v rustup >/dev/null 2>&1 \
        || die 'rustup is required; rerun with ANLAND_WITH_DEPS=1 or install rustup'
    if ! rustup run "$channel" rustc --version >/dev/null 2>&1; then
        [[ "$ANLAND_OFFLINE" == 0 ]] || die "offline build lacks Rust toolchain $channel"
        log "installing Rust $channel"
        rustup toolchain install "$channel" --profile minimal --no-self-update
    fi
    RUST_TOOLCHAIN="$channel"
    rustup run "$channel" rustc --version >&2
}

build_bridge() {
    log 'building the Anland bridge and public producer'
    local prefix="$BUILD/native/usr"
    local backend_real
    # The recipe entry is a symlink. CMake does not resolve it, so "../.." from
    # the symlink path would point above the repository root. Build the real
    # path instead; its ../../.. is the repository that owns libdisplay_producer.
    backend_real="$(readlink -f -- "$BACKEND")" || die 'cannot resolve the backend path'
    [[ -f "$backend_real/CMakeLists.txt" ]] || die "backend CMakeLists is missing: $backend_real"
    rm -rf -- "$BUILD/bridge" "$BUILD/native"
    cmake -S "$backend_real" -B "$BUILD/bridge" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib >&2
    cmake --build "$BUILD/bridge" --parallel "$JOBS" >&2
    cmake --install "$BUILD/bridge" >&2
    local lib
    for lib in libanland_denial_bridge.so libdisplay_producer.so; do
        [[ -f "$prefix/lib/$lib" ]] || die "bridge build did not produce $lib"
    done
    export ANLAND_DENIAL_LIB_DIR="$prefix/lib"
    log "staged Anland libraries in $ANLAND_DENIAL_LIB_DIR"
}

prepare_flutter_sources() {
    local nested="$ENGINE_CACHE/checkout/engine/src/flutter/third_party/skia"
    local lock="$SRC/prebuilt/flutter-engine/SOURCE_LOCK.json"
    local tree="$ENGINE_CACHE/checkout"
    local depot="$tree/engine/src/flutter/third_party/depot_tools"
    local skia_tree

    if [[ ! -d "$depot" ]]; then
        [[ "$ANLAND_OFFLINE" == 0 ]] \
            || die "offline build lacks a prepared Flutter tree at $ENGINE_CACHE"
        log 'synchronizing the locked Flutter and Skia engine sources (first run only)'
        env -u DENIAL_FLUTTER_SOURCE_ROOT -u DENIAL_SKIA_SOURCE_ROOT \
            DENIAL_FLUTTER_ENGINE_CACHE_ROOT="$ENGINE_CACHE" \
            "$SRC/$DENIAL_FLUTTER_TOOL" prepare-graph >&2
    fi
    [[ -d "$tree/engine/src/flutter" ]] || die "prepared Flutter tree is incomplete: $tree"

    FLUTTER_REV="$(jq -er '.flutter.revision' "$lock")" || die 'SOURCE_LOCK.json has no Flutter revision'
    SKIA_REV="$(jq -er '.skia.revision' "$lock")" || die 'SOURCE_LOCK.json has no Skia revision'
    if [[ "$(git -C "$tree" rev-parse HEAD)" != "$FLUTTER_REV" ]]; then
        die "cached Flutter tree is at $(git -C "$tree" rev-parse --short HEAD), lock wants $FLUTTER_REV; remove $ENGINE_CACHE/checkout and rebuild"
    fi

    if [[ -L "$nested" ]]; then
        skia_tree="$(readlink -f -- "$nested")" || die 'cannot resolve the nested Skia link'
    else
        [[ -d "$nested" ]] || die "Flutter tree has no Skia checkout: $nested"
        rm -rf -- "$WORKDIR/src/skia"
        mv -- "$nested" "$WORKDIR/src/skia"
        ln -s "$WORKDIR/src/skia" "$nested"
        skia_tree="$WORKDIR/src/skia"
    fi
    [[ -d "$skia_tree/.git" ]] || die "Skia tree is not a checkout: $skia_tree"
    [[ "$(git -C "$skia_tree" rev-parse HEAD)" == "$SKIA_REV" ]] \
        || die "Skia tree is at $(git -C "$skia_tree" rev-parse --short HEAD), lock wants $SKIA_REV"

    local patch="$SCRIPT_DIR/flutter-engine.patch"
    if git -C "$tree" apply --check "$patch" 2>/dev/null; then
        git -C "$tree" apply "$patch"
        log 'applied flutter-engine.patch'
    elif git -C "$tree" apply --reverse --check "$patch" 2>/dev/null; then
        log 'flutter-engine.patch is already applied'
    else
        die "flutter-engine.patch does not apply to Flutter $FLUTTER_REV"
    fi

    export DENIAL_FLUTTER_SOURCE_ROOT="$tree"
    export DENIAL_SKIA_SOURCE_ROOT="$skia_tree"
    export DENIAL_FLUTTER_ENGINE_CACHE_ROOT="$ENGINE_CACHE"
    log "Flutter $FLUTTER_REV, Skia $SKIA_REV, engine cache $ENGINE_CACHE"
}

# `denial-pc build` prepares the plugin kit from prebuilt/flutter-engine/
# <platform>-release and copies the engine license sidecars from it. The
# committed prebuilts ship those files, but the arm64 release directory is
# produced locally by the engine build, so install the architecture-neutral
# license texts before the build stage reads them.
seed_engine_licenses() {
    local release="$SRC/prebuilt/flutter-engine/linux-arm64-release"
    local source="$SRC/prebuilt/flutter-engine/linux-x64-release"
    local name
    install -d -m 0755 "$release"
    for name in LICENSE.flutter LICENSE.third_party; do
        if [[ ! -f "$release/$name" ]]; then
            [[ -f "$source/$name" ]] || die "missing engine license: $source/$name"
            install -m 0644 "$source/$name" "$release/$name"
        fi
    done
}

# Flutter publishes no arm64 Linux SDK artifacts for the locked engine
# revision, so a fresh `flutter` cannot bootstrap its Dart SDK or GTK
# artifact cache from storage.googleapis.com. The engine build itself
# produces both (dart-sdk and icudtl.dat); build it first and seed the SDK
# cache, which makes update_dart_sdk.sh a no-op and lets bootstrap continue.
seed_arm64_flutter_sdk() {
    [[ "$(uname -m)" == aarch64 ]] || return 0
    local cache="$ENGINE_CACHE/checkout/bin/cache"
    local out="$ENGINE_CACHE/build/out/denial_host_release_arm64"
    local engine_version
    engine_version="$(tr -d '[:space:]' < "$ENGINE_CACHE/checkout/bin/internal/engine.version")"

    seed_engine_licenses

    if [[ -x "$cache/dart-sdk/bin/dart" \
        && -f "$cache/artifacts/engine/linux-arm64/icudtl.dat" \
        && -f "$cache/engine-dart-sdk.stamp" \
        && "$(< "$cache/engine-dart-sdk.stamp")" == "$engine_version" ]]; then
        log 'arm64 Flutter SDK cache is already seeded'
        return
    fi
    [[ "$ANLAND_OFFLINE" == 0 ]] || die 'offline build cannot build the arm64 Flutter SDK cache'
    command -v ninja >/dev/null 2>&1 || die 'ninja is required to build the Flutter engine'

    log 'building the arm64 engine and its Dart SDK (first run only)'
    "$SRC/$DENIAL_FLUTTER_TOOL" prepare-app-build >&2
    [[ -x "$out/dart-sdk/bin/dart" ]] || die "engine build produced no Dart SDK: $out/dart-sdk"
    [[ -f "$out/icudtl.dat" ]] || die "engine build produced no icudtl.dat: $out/icudtl.dat"

    install -d -m 0755 "$cache" "$cache/artifacts/engine/linux-arm64"
    rm -rf -- "$cache/dart-sdk"
    rsync -a "$out/dart-sdk/" "$cache/dart-sdk/"
    install -m 0644 "$out/icudtl.dat" "$cache/artifacts/engine/linux-arm64/icudtl.dat"
    printf '%s\n' "$engine_version" > "$cache/engine-dart-sdk.stamp"
    log 'seeded the arm64 Dart SDK and ICU cache from the engine build'
}

build_denial() {
    export DENIAL_PC_BUILD_ROOT="$BUILD/pc"
    export DENIAL_PC_PACKAGE_INPUT_ROOT="$PACKAGE_INPUT"
    export DENIAL_PC_BUNDLE="$PACKAGE_INPUT/bundle"
    export DENIAL_PC_BUNDLE_ASSEMBLY="$PACKAGE_INPUT/flutter-release"
    export DENIAL_PC_SETTINGS_BUNDLE="$SRC/settings_app/build/linux/arm64/release/bundle"
    export DENIAL_PC_PLUGIN_MANAGER_BUNDLE="$SRC/plugin_manager_app/build/linux/arm64/release/bundle"
    export DENIAL_PC_POLKIT_BUNDLE="$SRC/polkit_app/build/linux/arm64/release/bundle"
    export DENIAL_PC_XWAYLAND=1

    log 'bootstrapping the Flutter SDK and Rust dependencies'
    "$SRC/$DENIAL_PC_TOOL" bootstrap >&2
    log 'building engine, shell, desktop applications and compositor'
    "$SRC/$DENIAL_PC_TOOL" build >&2
}

populate_engine_metadata() {
    local meta="$SRC/prebuilt/flutter-engine/linux-arm64-release"
    local out="$ENGINE_CACHE/build/out/denial_host_release_arm64"
    local engine="$meta/libflutter_engine.so"
    [[ -f "$engine" ]] || die "prepare-app-build did not stage $engine"
    [[ -f "$out/args.gn" ]] || die "missing generated GN arguments: $out/args.gn"

    install -d -m 0755 "$meta"
    install -m 0644 "$out/args.gn" "$meta/args.gn"
    (
        cd -- "$meta"
        sha256sum libflutter_engine.so > libflutter_engine.so.sha256
    )
    tr -d '[:space:]' < "$ENGINE_CACHE/checkout/bin/internal/engine.version" > "$meta/ENGINE_REVISION"
    printf '%s\n' "$(jq -er '.flutter.upstream_revision' "$SRC/prebuilt/flutter-engine/SOURCE_LOCK.json")" \
        > "$meta/FLUTTER_REVISION"
    cat > "$meta/BUILD_INFO.md" <<EOF
# Flutter Engine — Linux arm64 release

\`libflutter_engine.so\` is Denial's raw Flutter Embedder built for Linux arm64
by the Anland Ubuntu 26.04 producer. It is built from the locked Denial
Flutter and Skia forks, with the Impeller fix-ups in \`flutter-engine.patch\`.

- Flutter fork: $FLUTTER_REV
- Skia fork: $SKIA_REV
- Engine: $(cat "$meta/ENGINE_REVISION")
- Rust toolchain: $RUST_TOOLCHAIN
EOF
    if [[ -f "$SRC/prebuilt/flutter-engine/linux-x64-release/LICENSE.flutter" \
        && -f "$SRC/prebuilt/flutter-engine/linux-x64-release/LICENSE.third_party" ]]; then
        install -m 0644 "$SRC/prebuilt/flutter-engine/linux-x64-release/LICENSE.flutter" "$meta/LICENSE.flutter"
        install -m 0644 "$SRC/prebuilt/flutter-engine/linux-x64-release/LICENSE.third_party" "$meta/LICENSE.third_party"
    else
        install -m 0644 "$ENGINE_CACHE/checkout/LICENSE" "$meta/LICENSE.flutter"
        printf 'Third-party licenses are listed in the Flutter engine source tree.\n' \
            > "$meta/LICENSE.third_party"
    fi
    ENGINE_SHA256="$(awk 'NR == 1 { print $1 }' "$meta/libflutter_engine.so.sha256")"
    log "engine metadata ready: $ENGINE_SHA256"
}

stage_packages() {
    local stage="$PACKAGE_INPUT/native"
    local root="$stage/denial"
    export DENIAL_PACKAGE_GLIBC_BASELINE="$GLIBC_BASELINE"
    export DENIAL_DEB_ARCH=arm64
    # The checkout is patched and overlaid by this producer, so it is always
    # dirty by the time the runtime is staged. The version already names the
    # revision the patches were built from; a ".dirty" suffix would only hide
    # which integration went in.
    export DENIAL_PACKAGE_ALLOW_DIRTY=1
    # The pinned release is identified by its immutable commit even when an
    # offline checkout has no tags. Other revisions retain upstream's
    # development version unless the caller supplies an explicit override.
    if [[ "$DENIAL_REV" == d65792d4309a93f2a4877bfc16bc96df6151506c \
        && ! ${DENIAL_PACKAGE_VERSION+x} ]]; then
        export DENIAL_PACKAGE_VERSION=0.6.0
    fi
    if [[ -n "${DENIAL_VERSION_BASE_TAG:-}" ]]; then
        export DENIAL_VERSION_BASE_TAG
    fi
    log 'staging the Denial runtime'
    "$SRC/$DENIAL_STAGE_TOOL" >&2

    log 'adding the Anland bridge, public producer and launcher'
    install -d -m 0755 "$root/usr/lib"
    install -m 0755 "$ANLAND_DENIAL_LIB_DIR/libanland_denial_bridge.so" "$root/usr/lib/"
    install -m 0755 "$ANLAND_DENIAL_LIB_DIR/libdisplay_producer.so" "$root/usr/lib/"
    install -m 0755 "$SCRIPT_DIR/startup.sh" "$root/usr/bin/denial-anland"

    # The payload inventory and checksum sidecars are written by the staging
    # tool before the Anland files are added. Rebuild them so the packaged
    # payload stays byte-verified against the staging tree.
    find "$root" -print0 \
        | xargs -0 touch --no-dereference --date="@$SOURCE_DATE_EPOCH"
    (
        cd -- "$root"
        find . -type f -print0 | sort -z | xargs -0 sha256sum > "$stage/denial.sha256"
        find . -mindepth 1 \
            \( -type l -printf '%m %y %p -> %l\n' \
               -o -printf '%m %y %p\n' \) \
            | sort > "$stage/denial.inventory"
    )

    log 'building the .debs'
    DENIAL_PACKAGE_SKIP_STAGE=1 DENIAL_PC_PACKAGE_ROOT="$ARTIFACTS_DIR" \
        "$SRC/$DENIAL_DEB_TOOL" >&2
}

build_xwayland() {
    local cache="$WORKDIR/xwayland/cache"
    local work="$WORKDIR/xwayland/work"
    local tree version new_version deb arch
    arch="$(dpkg --print-architecture)" || die 'cannot identify the package architecture'
    mkdir -p "$cache" "$work"

    if ! apt-cache showsrc "$XWAYLAND_PACKAGE" >/dev/null 2>&1; then
        die 'apt has no Xwayland source index; run with ANLAND_WITH_DEPS=1 to enable deb-src'
    fi
    if [[ "$ANLAND_OFFLINE" == 1 ]]; then
        local dsc
        dsc="$(find "$cache" -maxdepth 1 -name "${XWAYLAND_PACKAGE}_*.dsc" | sort | tail -n 1)"
        [[ -n "$dsc" ]] || die "offline build has no cached Xwayland source in $cache"
        rm -rf -- "$work"/xwayland-*
        (cd -- "$cache" && dpkg-source -x "$(basename -- "$dsc")") >&2
        mv -- "$cache"/xwayland-* "$work/" 2>/dev/null || true
    else
        rm -rf -- "$work"/xwayland-*
        (
            cd -- "$work"
            if [[ -n "${XWAYLAND_VERSION:-}" ]]; then
                apt-get source "$XWAYLAND_PACKAGE=$XWAYLAND_VERSION"
            else
                apt-get source "$XWAYLAND_PACKAGE"
            fi
        ) >&2
        cp -a -- "$work"/"${XWAYLAND_PACKAGE}"_* "$cache/" 2>/dev/null || true
    fi

    tree="$(find "$work" -maxdepth 1 -type d -name 'xwayland-*' | sort | head -n 1)"
    [[ -n "$tree" ]] || die 'apt-get source produced no Xwayland tree'
    version="$(dpkg-parsechangelog --file "$tree/debian/changelog" --show-field Version)"
    [[ -n "$version" ]] || die 'cannot read the Xwayland source version'
    if [[ -n "${XWAYLAND_VERSION:-}" && "$version" != "$XWAYLAND_VERSION" ]]; then
        die "apt produced Xwayland $version, expected $XWAYLAND_VERSION"
    fi

    log "patching Xwayland $version"
    (cd -- "$tree" && patch -p1 --forward --dry-run < "$SCRIPT_DIR/xwayland.patch") >&2 \
        || die "xwayland.patch does not apply to xwayland $version"
    (cd -- "$tree" && patch -p1 --forward < "$SCRIPT_DIR/xwayland.patch") >&2

    new_version="${version}${XWAYLAND_ANLAND_REVISION:?sources.conf defines no Xwayland revision}"
    {
        printf '%s (%s) %s; urgency=medium\n\n' "$XWAYLAND_PACKAGE" "$new_version" "$UBUNTU_CODENAME"
        printf '  * Anland: accept implicit-modifier dmabuf buffers, tolerate the\n'
        printf '    kgsl/freedreno linear-modifier rewrite, support linux-dmabuf v3\n'
        printf '    without wl_drm, and fall back to XWAYLAND_GBM_DEVICE when the\n'
        printf '    compositor exposes no render node.\n\n'
        printf ' -- Anland <build@localhost>  %s\n\n' "$(LC_ALL=C date -R -d "@$SOURCE_DATE_EPOCH")"
        cat -- "$tree/debian/changelog"
    } > "$tree/debian/changelog.new"
    mv -- "$tree/debian/changelog.new" "$tree/debian/changelog"

    log 'building the patched Xwayland .deb'
    (cd -- "$tree" && env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR -u CMAKE_PREFIX_PATH \
        -u LD_LIBRARY_PATH -u DESTDIR -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        DEB_BUILD_OPTIONS="nocheck nodwz noautodbgsym parallel=$JOBS" \
        dpkg-buildpackage -b -uc -us) >&2 || die 'Xwayland build failed'

    local upstream_version="${new_version#*:}"
    deb="$work/${XWAYLAND_PACKAGE}_${upstream_version}_${arch}.deb"
    [[ -f "$deb" ]] || deb="$(find "$work" -maxdepth 1 -name "${XWAYLAND_PACKAGE}_*_${arch}.deb" | sort | tail -n 1)"
    [[ -f "$deb" ]] || die 'Xwayland build produced no .deb'
    [[ "$(dpkg-deb --field "$deb" Package)" == "$XWAYLAND_PACKAGE" ]] || die 'unexpected Xwayland package name'
    [[ "$(dpkg-deb --field "$deb" Architecture)" == "$arch" ]] || die 'unexpected Xwayland architecture'
    [[ "$(dpkg-deb --field "$deb" Version)" == "$new_version" ]] || die 'unexpected Xwayland version'
    install -m 0644 "$deb" "$ARTIFACTS_DIR/"
    log "Xwayland package: $ARTIFACTS_DIR/$(basename -- "$deb")"
}

install_packages() {
    [[ "$ANLAND_INSTALL" == 1 ]] || return 0
    command -v sudo >/dev/null 2>&1 || die 'sudo is required for ANLAND_INSTALL=1'
    log 'installing the produced packages'
    local -a debs=()
    mapfile -t debs < <(find "$ARTIFACTS_DIR" -maxdepth 1 -name '*.deb' | sort)
    ((${#debs[@]})) || die 'no packages to install'
    sudo apt-get install -y "${debs[@]}"
}

write_build_info() {
    local info="$ARTIFACTS_DIR/build-info.txt"
    {
        printf 'denial_revision=%s\n' "$DENIAL_REV"
        printf 'denial_branch=%s\n' "${DENIAL_BRANCH:-}"
        printf 'flutter_revision=%s\n' "$FLUTTER_REV"
        printf 'skia_revision=%s\n' "$SKIA_REV"
        printf 'engine_sha256=%s\n' "$ENGINE_SHA256"
        printf 'rust_toolchain=%s\n' "$RUST_TOOLCHAIN"
        printf 'glibc_baseline=%s\n' "$GLIBC_BASELINE"
        printf 'denial_patch_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/denial.patch" | awk '{print $1}')"
        printf 'flutter_engine_patch_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/flutter-engine.patch" | awk '{print $1}')"
        printf 'xwayland_patch_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/xwayland.patch" | awk '{print $1}')"
    } > "$info"
    log "build record: $info"
}

main() {
    # The upstream tools default to `nproc - 4` and cap arm64 at 10. The
    # producer runs on dedicated build hardware, so hand them every core.
    export DENIAL_BUILD_JOBS="$JOBS"
    export DENIAL_FLUTTER_ENGINE_JOBS="$JOBS"
    export DENIAL_PC_DEDICATED_BUILDER=1
    export DENIAL_FLUTTER_ENGINE_DEDICATED_BUILDER=1

    preflight
    install_dependencies
    check_tools
    fetch_denial
    apply_denial_patch
    overlay_backend
    prepare_rust
    build_bridge
    prepare_flutter_sources
    seed_arm64_flutter_sdk
    build_denial
    populate_engine_metadata
    stage_packages
    build_xwayland
    install_packages
    write_build_info
    log 'done'
    printf 'Packages: %s\n' "$ARTIFACTS_DIR"
    printf 'Start:    sudo /usr/bin/denial-anland --check, then /usr/bin/denial-anland\n'
}

main "$@"
