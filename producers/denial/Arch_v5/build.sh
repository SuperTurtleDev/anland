#!/usr/bin/env bash
# Arch Linux ARM: one entry builds/reuses Flutter, then Denial and desktop packages.
# Usage: [ANLAND_WITH_DEPS=1] [ANLAND_ENGINE_MODE=auto|build|prepared] bash build.sh [--noconfirm]
# Cache: WORKDIR / DENIAL_CACHE; packages: ARTIFACTS_DIR (default $WORKDIR/packages).
# auto verifies prepared inputs or builds missing ones; build selects Impeller+Ganesh/Vulkan/no-LTO/no-GTK.
# prepared never compiles an engine; ANLAND_OFFLINE=1 requires synchronized source/dependency caches.
# ANLAND_WITH_DE=0 omits desktop; JOBS=2 limits builds; ANLAND_INSTALL=1 explicitly installs packages.
# Only finished native packages are emitted; no desktop is started/restarted.
if [[ ${BASH_SOURCE[0]} != "$0" ]]; then
    printf 'execute build.sh with bash; do not source it into an interactive shell\n' >&2
    return 1
fi
set -uo pipefail
BUILD_SCRIPT=$(realpath -- "${BASH_SOURCE[0]}") || exit 1

# Apply engine patches with fuzzy context and preserve unrelated source edits.
engine_patch_manager() {
    python3 - "$@" <<'PY_ENGINE_PATCH_MANAGER'
"""Apply engine patches with GNU patch fuzz=2; never discard unrelated edits.

A temporary copy of the affected files resolves fuzzy context. An alternate
Git index identifies the resulting diff without changing the real index.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'apply'))
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--patch', type=Path, required=True)
    parser.add_argument('--state', type=Path, required=True)
    args = parser.parse_args(argv)
    checkout = args.checkout.resolve(strict=True)
    patch = args.patch.resolve(strict=True)
    args.state.mkdir(parents=True, exist_ok=True)
    state = args.state.resolve(strict=True)
    if not patch.read_bytes():
        raise RuntimeError('empty engine patch')
    applied = state / 'engine-local.applied.patch'

    def git(*command, env=None, data=None):
        result = subprocess.run(['git', '-C', str(checkout), *command],
                                env=env, input=data, capture_output=True)
        if result.returncode != 0:
            raise RuntimeError('git ' + ' '.join(command) + ': ' +
                               result.stderr.decode(errors='replace').strip())
        return result.stdout

    if Path(git('rev-parse', '--show-toplevel').decode().strip()).resolve() != checkout:
        raise RuntimeError('checkout must name the engine Git root')
    git('diff', '--cached', '--quiet', '--ignore-submodules=all')
    # gclient quarantines conflicting dependency checkouts in the root _bad_scm/.
    # Preserve those backups without treating them as live source edits. Keep
    # rejecting other untracked files; do not exempt dependency or build trees.
    untracked = git('ls-files', '--others', '--exclude-standard',
                    '--exclude=/_bad_scm/')
    if untracked:
        raise RuntimeError('untracked engine changes preserved; refusing patch management:\n' +
                           untracked.decode(errors='replace').rstrip())

    actual = git('diff', '--no-ext-diff', '--binary', '--ignore-submodules=all', 'HEAD')

    def patch_index(path):
        fd, name = tempfile.mkstemp(prefix='engine-patch-index-', dir=state)
        os.close(fd)
        os.unlink(name)  # read-tree creates a valid index, not an empty file.
        env = dict(os.environ, GIT_INDEX_FILE=name)
        try:
            git('read-tree', 'HEAD', env=env)
            # --numstat only reads paths; GNU patch resolves the upstream context.
            paths = [os.fsdecode(entry.split(b'\t', 2)[2])
                     for entry in git('apply', '--numstat', '-z', str(path)).split(b'\0') if entry]
            if not paths or any(Path(p).is_absolute() or '..' in Path(p).parts for p in paths):
                raise RuntimeError('invalid engine patch paths')
            with tempfile.TemporaryDirectory(prefix='engine-patch-files-', dir=state) as temporary:
                for entry in git('ls-files', '--stage', '-z', '--', *paths, env=env).split(b'\0'):
                    if not entry:
                        continue
                    metadata, relative = entry.split(b'\t', 1)
                    mode, blob, stage = metadata.split()
                    if mode not in (b'100644', b'100755') or stage != b'0':
                        raise RuntimeError('unsupported engine patch file mode')
                    target = Path(temporary) / os.fsdecode(relative)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(git('cat-file', 'blob', blob.decode()))
                    target.chmod(int(mode, 8) & 0o777)
                result = subprocess.run(['patch', '--batch', '--forward', '--fuzz=2',
                                         '--no-backup-if-mismatch', '-p1', '-d', temporary,
                                         '-i', str(path)], capture_output=True)
                if result.returncode:
                    raise RuntimeError('engine patch failed with fuzz=2:\n' +
                                       (result.stdout + result.stderr).decode(errors='replace'))
                sys.stdout.buffer.write(result.stdout + result.stderr)
                git('--work-tree=' + temporary, 'add', '--all', '--force', '--', *paths, env=env)
            expected = git('diff', '--cached', '--no-ext-diff', '--binary',
                           '--ignore-submodules=all', 'HEAD', env=env)
            transition = git('diff', '--no-ext-diff', '--binary',
                             '--ignore-submodules=all', env=env)
            return expected, transition
        finally:
            Path(name).unlink(missing_ok=True)
            Path(name + '.lock').unlink(missing_ok=True)

    expected, transition = patch_index(patch)
    if actual and actual != expected:
        previous = patch_index(applied)[0] if applied.is_file() else None
        if previous != actual:
            raise RuntimeError('engine diff is not the exact current or recorded patch; unrelated edits preserved')
    if args.action == 'check':
        print('engine patch check: ' + ('managed' if actual else 'pristine'))
        return
    if actual != expected:
        # transition describes new-index -> working-tree; reverse it to update
        # the entire known old state atomically to the new desired patch.
        # This diff was generated above from the resolved files, not upstream context.
        git('apply', '--reverse', '--whitespace=nowarn', '-', data=transition)
        if git('diff', '--no-ext-diff', '--binary', '--ignore-submodules=all', 'HEAD') != expected:
            raise RuntimeError('engine patch post-apply diff mismatch')
    saved = state / 'engine-local.applied.patch.new'
    saved.write_bytes(patch.read_bytes())
    os.replace(saved, applied)
    print('engine patch apply: ' + ('already current (mtime preserved)' if actual == expected else 'updated'))


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (OSError, RuntimeError) as error:
        raise SystemExit('engine-patch: ' + str(error))
PY_ENGINE_PATCH_MANAGER
}
SCRIPT_DIR="$(cd -- "${ANLAND_DENIAL_RECIPE_DIR:-$(dirname -- "${BASH_SOURCE[0]}")}" && pwd -P)" || exit 1
ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd -P)" || exit 1
source "$ROOT/producers/common/latest-source.sh"
log() { printf '\n==> %s\n' "$*"; }
die() { printf 'denial-build: %s\n' "$*" >&2; exit 1; }
run() { "$@" || die "failed: $*"; }
in_dir() { local dir=$1; shift; (cd -- "$dir" && "$@") || die "failed in $dir: $*"; }
for argument in "$@"; do
    case $argument in
        -h|--help) sed -n '2,8p' "$BUILD_SCRIPT"; exit 0 ;;
        --noconfirm) ;; # Same calling convention as the other Arch_v5 entries.
        *) die "unknown argument: $argument (see --help)" ;;
    esac
done
source "$SCRIPT_DIR/sources.conf" || die 'cannot read sources.conf'
JOBS=${JOBS:-2}
ANLAND_PKGREL=${ANLAND_PKGREL:-2}
ANLAND_DE_PKGREL=${ANLAND_DE_PKGREL:-2}
ANLAND_WITH_DE=${ANLAND_WITH_DE:-1}
ANLAND_WITH_DEPS=${ANLAND_WITH_DEPS:-0}
ANLAND_OFFLINE=${ANLAND_OFFLINE:-0}
ANLAND_INSTALL=${ANLAND_INSTALL:-0}
ANLAND_ENGINE_MODE=${ANLAND_ENGINE_MODE:-auto}
case $ANLAND_ENGINE_MODE in auto|build|prepared) ;; *) die 'ANLAND_ENGINE_MODE must be auto, build or prepared' ;; esac
ENGINE_POLICY='release-arm64-impeller-ganesh-vulkan-nolto-no-gtk-shadow-opacity-v3'
for flag in ANLAND_WITH_DE ANLAND_WITH_DEPS ANLAND_OFFLINE ANLAND_INSTALL; do
    [[ ${!flag} == 0 || ${!flag} == 1 ]] || die "$flag must be 0 or 1"
done
for number in JOBS ANLAND_PKGREL ANLAND_DE_PKGREL; do
    [[ ${!number} =~ ^[1-9][0-9]*$ ]] || die "$number must be a positive integer"
done
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || die 'run as a normal aarch64 user'
ANLAND_DENIAL_DISTRO=${ANLAND_DENIAL_DISTRO:-arch}
case $ANLAND_DENIAL_DISTRO in arch|debian13|ubuntu2604|fedora43|fedora44) ;; *) die 'unsupported Denial distribution' ;; esac
WORKDIR=$(realpath -m -- "${WORKDIR:-${DENIAL_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/denial-anland}}") || die 'invalid WORKDIR'
ARTIFACTS_DIR=$(realpath -m -- "${ARTIFACTS_DIR:-$WORKDIR/packages}") || die 'invalid ARTIFACTS_DIR'
[[ $WORKDIR != / && $WORKDIR != "$HOME" && $WORKDIR != "$ROOT" && $WORKDIR != "$ROOT/"* ]] || die 'WORKDIR must be a separate cache directory'
SOURCE="$WORKDIR/src/denial"
ENGINE_PATCH="$WORKDIR/engine.pending.patch"
BUILD="$WORKDIR/build"
PUBLIC="$WORKDIR/public-overlay/anland"
FLUTTER=$(realpath -m -- "${DENIAL_FLUTTER_ROOT:-$WORKDIR/engine/checkout}") || die 'invalid Flutter path'
ENGINE=$(realpath -m -- "${DENIAL_ENGINE_DIR:-$WORKDIR/engine/build/out/denial_host_release_arm64}") || die 'invalid engine path'
[[ $ARTIFACTS_DIR != "$SOURCE" && $ARTIFACTS_DIR != "$SOURCE/"* ]] || die 'packages must be outside the source tree'
export LANG=C.UTF-8 LC_ALL=C.UTF-8 GIT_TERMINAL_PROMPT=0 GIT_PAGER=cat
export CARGO_HOME="$WORKDIR/cargo-home" CARGO_TARGET_DIR="$WORKDIR/cargo-target"
export PUB_CACHE="$WORKDIR/pub-cache" FLUTTER_ROOT="$FLUTTER" FLUTTER_GIT_URL="$FLUTTER_URL"
export CI=true FLUTTER_SUPPRESS_ANALYTICS=true DART_SUPPRESS_ANALYTICS=true
export ANLAND_DENIAL_LIB_DIR="$BUILD/native/usr/lib"
export ANLAND_PKGREL ANLAND_DE_PKGREL
# All native binaries report the release identity without changing upstream locks.
export DENIAL_BUILD_VERSION="$DENIAL_VERSION"
# Release policy belongs to the recipe, not upstream Cargo manifests.
export CARGO_PROFILE_RELEASE_LTO="${CARGO_PROFILE_RELEASE_LTO:-false}"
run mkdir -p "$WORKDIR" "$BUILD" "$ARTIFACTS_DIR"
exec 9>"$WORKDIR/build.lock" || die 'cannot open build lock'
flock -n 9 || die 'another build owns WORKDIR'
exec 8>"$ARTIFACTS_DIR/.build.lock" || die 'cannot open package lock'
flock -n 8 || die 'another build owns ARTIFACTS_DIR'
network=()
[[ $ANLAND_OFFLINE == 0 ]] || network=(--offline)

checkout() {
    local tree=$1 url=$2 revision=$3 seed=${4:-}
    if [[ ! -e $tree/.git ]]; then
        [[ ! -e $tree ]] || die "untracked source preserved: $tree"
        if [[ -n $seed && -e $seed/.git ]] && git -C "$seed" cat-file -e "$revision^{commit}" 2>/dev/null; then
            run git clone --no-hardlinks --no-checkout "$seed" "$tree"
        else
            [[ $ANLAND_OFFLINE == 0 ]] || die "offline source missing: $tree"
            run git init -q "$tree"
            run git -C "$tree" fetch --depth=1 "$url" "$revision"
        fi
        run git -C "$tree" checkout --detach "$revision"
    fi
    [[ $(git -C "$tree" rev-parse HEAD) == "$revision" ]] ||
        die "cached revision differs: $tree; move it aside before changing upstream version"
}

resolve_flutter_dependencies() {
    # Read only the selected DEPS expressions; never execute upstream DEPS code.
    python3 - "$1/DEPS" "$FLUTTER_URL" "$FLUTTER_REV" <<'PY_FLUTTER_SOURCES'
import ast, hashlib, json, re, sys
from pathlib import Path
path, repository, revision = sys.argv[1:]
raw = Path(path).read_bytes()
assignments = {}
for node in ast.parse(raw).body:
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
        assignments[node.targets[0].id] = node.value

def entries(name):
    node = assignments.get(name)
    if not isinstance(node, ast.Dict):
        raise SystemExit('unsupported Flutter DEPS: missing ' + name)
    return {ast.literal_eval(key): value for key, value in zip(node.keys, node.values)}

variables = entries('vars')
dependencies = entries('deps')

def value(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return value(node.left) + value(node.right)
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and
            node.func.id == 'Var' and len(node.args) == 1 and not node.keywords):
        return value(variables[ast.literal_eval(node.args[0])])
    raise SystemExit('unsupported Flutter DEPS expression; adapt the source resolver')

record = {'schema_version': 1, 'flutter': {'repository': repository, 'revision': revision},
          'deps_sha256': hashlib.sha256(raw).hexdigest()}
for name in ('skia', 'depot_tools'):
    node = dependencies['engine/src/flutter/third_party/' + name]
    if isinstance(node, ast.Dict):
        node = next(v for k, v in zip(node.keys, node.values) if ast.literal_eval(k) == 'url')
    url, commit = value(node).rsplit('@', 1)
    if not url.startswith('https://') or not re.fullmatch('[0-9a-f]{40}', commit):
        raise SystemExit('invalid Flutter DEPS Git source: ' + name)
    record[name] = {'repository': url, 'revision': commit}
print(json.dumps(record, indent=2))
PY_FLUTTER_SOURCES
}

prepare_sources() {
    log 'Prepare Denial and Anland sources'
    local latest_result latest_tree flutter_tree
    latest_result=$(anland_prepare_latest_git_source denial "$DENIAL_URL" "$WORKDIR/latest-sources" '' "$ANLAND_OFFLINE" '' shallow "$DENIAL_REVISION") ||
        die 'cannot resolve the Denial revision supported by the backend patch'
    IFS=$'\t' read -r latest_tree DENIAL_REV <<< "$latest_result"
    [[ -d $latest_tree && $DENIAL_REV =~ ^[0-9a-f]{40,64}$ ]] || die 'invalid latest Denial identity'
    [[ $DENIAL_REV == "$DENIAL_REVISION" ]] || die 'cached Denial revision differs from sources.conf'
    SOURCE="$WORKDIR/src/denial-$DENIAL_REV"
    [[ $ARTIFACTS_DIR != "$SOURCE" && $ARTIFACTS_DIR != "$SOURCE/"* ]] || die 'packages must be outside the source tree'
    checkout "$SOURCE" "$DENIAL_URL" "$DENIAL_REV" "${DENIAL_SOURCE:-$latest_tree}"
    # Denial's source lock identifies the matching Flutter SDK branch and
    # revision, independently of the fork's default branch.
    local flutter_lock="$SOURCE/prebuilt/flutter-engine/SOURCE_LOCK.json" flutter_revision
    flutter_revision=$(jq -er '.flutter.revision' "$flutter_lock") || die 'missing Denial Flutter revision'
    FLUTTER_URL=$(jq -er '.flutter.repository' "$flutter_lock") || die 'missing Denial Flutter repository'
    FLUTTER_VERSION=$(jq -er '.flutter.branch | capture("^denial/(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)$").version' "$flutter_lock") ||
        die 'cannot derive Flutter SDK version from Denial source lock'
    latest_result=$(anland_prepare_latest_git_source flutter "$FLUTTER_URL" "$WORKDIR/latest-sources" '' "$ANLAND_OFFLINE" '' shallow "$flutter_revision") ||
        die 'cannot resolve the Flutter revision selected by Denial'
    IFS=$'\t' read -r flutter_tree FLUTTER_REV <<< "$latest_result"
    [[ -d $flutter_tree && $FLUTTER_REV =~ ^[0-9a-f]{40}$ ]] || die 'invalid latest Flutter identity'
    [[ $FLUTTER_REV == "$flutter_revision" ]] || die 'cached Flutter revision differs from Denial source lock'
    ENGINE_SOURCES="$WORKDIR/engine/sources/$FLUTTER_REV.json"
    run mkdir -p "$(dirname -- "$ENGINE_SOURCES")"
    resolve_flutter_dependencies "$flutter_tree" > "$ENGINE_SOURCES.pending" || die 'cannot resolve current Flutter DEPS; adapt the source resolver'
    run mv -- "$ENGINE_SOURCES.pending" "$ENGINE_SOURCES"
    export DENIAL_ENGINE_SOURCES="$ENGINE_SOURCES"
    [[ $FLUTTER_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][A-Za-z0-9.+-]+)?$ ]] || die 'unrecognized Flutter SDK version; adapt the version resolver'
    FLUTTER=$(realpath -m -- "${DENIAL_FLUTTER_ROOT:-$WORKDIR/engine/checkout-$FLUTTER_REV}") || die 'invalid Flutter checkout path'
    if [[ -z ${DENIAL_ENGINE_DIR:-} ]]; then
        ENGINE=$(realpath -m -- "$WORKDIR/engine/build/$FLUTTER_REV/out/denial_host_release_arm64_anland_ganesh") ||
            die 'invalid Flutter engine output path'
    fi
    # The patch and release version belong to the exact commit in sources.conf.
    # Avoid deriving a release from unrelated tags or missing shallow history.
    [[ $DENIAL_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die 'invalid pinned Denial release version'
    export DENIAL_BUILD_VERSION="$DENIAL_VERSION" FLUTTER_GIT_URL="$FLUTTER_URL" FLUTTER_ROOT="$FLUTTER"
    local patch="$WORKDIR/denial.pending.patch" previous="$WORKDIR/denial.$DENIAL_REV.applied.patch" file target
    # Validate exact context for the selected release; do not fuzz upstream drift.
    [[ -s $SCRIPT_DIR/denial.patch ]] || die 'missing component patch: denial.patch'
    cat -- "$SCRIPT_DIR/denial.patch" > "$patch" || die 'cannot read denial.patch'
    [[ -s $SCRIPT_DIR/flutter-engine.patch ]] || die 'missing component patch: flutter-engine.patch'
    cp -- "$SCRIPT_DIR/flutter-engine.patch" "$ENGINE_PATCH" || die 'cannot read flutter-engine.patch'
    if git -C "$SOURCE" apply --check "$patch"; then
        run git -C "$SOURCE" apply "$patch"
    elif git -C "$SOURCE" apply --reverse --check "$patch" 2>/dev/null; then
        log 'Denial integration patch is already applied'
    else
        [[ -f $previous ]] || die 'source has conflicting edits; refusing to overwrite them'
        # Prove the transition on a copy before changing the managed checkout.
        local probe
        probe=$(mktemp -d "$WORKDIR/.patch-check.XXXXXX") || die 'cannot create patch check tree'
        run cp -a --reflink=auto "$SOURCE/." "$probe/"
        if ! git -C "$probe" apply --reverse --check "$previous" ||
           ! git -C "$probe" apply --reverse "$previous" ||
           ! git -C "$probe" apply --check "$patch"; then
            run rm -rf -- "$probe"
            die 'Denial patch transition failed; source changes preserved'
        fi
        run rm -rf -- "$probe"
        run git -C "$SOURCE" apply --reverse "$previous"
        run git -C "$SOURCE" apply "$patch"
    fi
    run cp "$patch" "$previous"
    target="$SOURCE/compositor/src/bin/deniald/anland_backend"
    run mkdir -p "$target"
    for file in "$SCRIPT_DIR/denial/"*.rs; do
        # Changed overlays get a fresh mtime so Cargo cannot reuse stale objects.
        local relative=${file#"$SCRIPT_DIR/denial/"}
        cmp -s "$file" "$target/$relative" || run install -m644 "$file" "$target/$relative"
    done
    # Use the canonical public library, not a Denial-specific copy or patch.
    run rm -rf -- "$PUBLIC/common" "$PUBLIC/libdisplay_producer" "$PUBLIC/producers"
    run mkdir -p "$PUBLIC/producers/denial"
    run cp -aL "$ROOT/common" "$ROOT/libdisplay_producer" "$PUBLIC/"
    run cp -aL "$SCRIPT_DIR/denial" "$PUBLIC/producers/denial/anland_backend"
}
check_sha() {
    printf '%s  %s\n' "$1" "$2" | sha256sum -c - || die "artifact checksum: $2"
}
link_cache() {
    local source previous managed
    source=$(realpath -e -- "$1") || die "missing SDK input: $1"
    if [[ -L $2 ]]; then
        previous=$(realpath -m -- "$2") || die "cannot inspect SDK link: $2"
        [[ $previous != "$source" ]] || return 0
        managed=$(realpath -m -- "$WORKDIR/engine/build") || die 'invalid managed engine cache'
        [[ $previous == "$managed/"* ]] || die "unmanaged SDK link preserved: $2"
        # Retarget only our existing engine-cache link; never remove SDK directories.
        run ln -sfnT -- "$source" "$2"
    elif [[ -e $2 ]]; then
        [[ $(realpath -e -- "$2") == "$source" ]] || die "unmanaged SDK directory preserved: $2"
    else
        run ln -s -- "$source" "$2"
    fi
}
prepare_engine_sources() (
    # Isolated options/functions; no second build entry is sourced or executed.
    set -euo pipefail
    local SOURCE_LOCK="$ENGINE_SOURCES"
    local CACHE_ROOT="$WORKDIR/engine" DEPOT_TOOLS="$WORKDIR/engine/depot_tools-$FLUTTER_REV"
    local STATE_ROOT="$WORKDIR/engine/state/$FLUTTER_REV" VPYTHON_CACHE_ROOT="$WORKDIR/engine/vpython"
    local CHECKOUT="$FLUTTER" SKIA_CHECKOUT="$FLUTTER/engine/src/flutter/third_party/skia"
    local DENIAL_ARCH=aarch64 DENIAL_FLUTTER_ENGINE_OFFLINE="$ANLAND_OFFLINE"
    fail() { die "engine source preparation: $*"; }
    lock_value() { jq -er "$1" "$SOURCE_LOCK"; }
    file_sha256() { sha256sum "$1" | awk '{print $1}'; }
    resolve_source_roots() { CHECKOUT="$FLUTTER"; SKIA_CHECKOUT="$CHECKOUT/engine/src/flutter/third_party/skia"; }
    require_clean_checkout() {
      local label="$1"
      local checkout="$2"
      local status

      [[ "$(git -C "$checkout" rev-parse --is-inside-work-tree 2>/dev/null || true)" \
        == true ]] \
        || fail "$label source is not a Git checkout: $checkout"
      if [[ $label == Flutter ]]; then
        engine_patch_manager check \
          --checkout "$checkout" --patch "$ENGINE_PATCH" \
          --state "$STATE_ROOT" || fail 'unmanaged Flutter source changes preserved'
        return
      fi
      git -C "$checkout" diff --quiet --ignore-submodules=all \
        || fail "$label source has tracked modifications: $checkout"
      git -C "$checkout" diff --cached --quiet --ignore-submodules=all \
        || fail "$label source has staged modifications: $checkout"
      status="$(
        git -C "$checkout" status \
          --porcelain=v1 \
          --untracked-files=normal \
          --ignore-submodules=all
      )"
      [[ -z "$status" ]] \
        || fail "$label source has untracked modifications: $checkout"
    }

    ensure_repository() {
      local label="$1"
      local repository="$2"
      local revision="$3"
      local destination="$4"
      local actual_origin checkout_root head_ref ref_status entries
      local initial_checkout=0

      if [[ ! -e "$destination" ]]; then
        install -d -m 0700 "$(dirname -- "$destination")"
        git init --quiet "$destination"
        git -C "$destination" remote add origin "$repository"
      fi
      checkout_root="$(git -C "$destination" rev-parse --show-toplevel)" \
        || fail "$label source is not a Git worktree: $destination"
      [[ "$checkout_root" == "$(realpath -e -- "$destination")" ]] \
        || fail "$label cache must name the Git worktree root: $destination"
      actual_origin="$(git -C "$destination" remote get-url origin)" \
        || fail "$label cache has no origin: $destination"
      [[ "$actual_origin" == "$repository" ]] \
        || fail "$label cache origin is $actual_origin, expected $repository"

      if git -C "$destination" rev-parse --verify --quiet 'HEAD^{commit}' >/dev/null; then
        # Existing worktrees must be checked before any checkout can change them.
        require_clean_checkout "$label" "$destination"
        if [[ $label == Flutter ]] && ! git -C "$destination" diff --quiet; then
          [[ $(git -C "$destination" rev-parse HEAD) == "$revision" ]] \
            || fail 'managed Flutter patch cannot be moved to a different source lock'
          return
        fi
      else
        # git init (or an interrupted first fetch) has no HEAD to diff against.
        # Only an empty unborn worktree may bootstrap; never repair broken refs.
        head_ref="$(git -C "$destination" symbolic-ref --quiet --no-recurse HEAD)" \
          || fail "$label cache has invalid HEAD; preserved: $destination"
        [[ $head_ref == refs/heads/* ]] \
          || fail "$label cache HEAD is not a branch; preserved: $destination"
        ref_status=0
        # Git >= 2.46: only status 2 means missing; malformed refs are errors.
        git -C "$destination" show-ref --exists "$head_ref" 2>/dev/null || ref_status=$?
        [[ $ref_status == 2 ]] \
          || fail "$label cache is not verifiably unborn (Git >= 2.46 required); preserved: $destination"
        entries="$(git -C "$destination" ls-files --stage)" \
          || fail "$label cache index cannot be read; preserved: $destination"
        [[ -z "$entries" ]] \
          || fail "$label unborn cache has index entries; use a new cache directory: $destination"
        # Include ignored files, hidden files and empty directories, unlike status.
        entries="$(find "$checkout_root" -mindepth 1 -maxdepth 1 ! -name .git -printf x -quit)" \
          || fail "$label cache cannot be inspected; preserved: $destination"
        [[ -z "$entries" ]] \
          || fail "$label unborn cache is not empty; use a new cache directory: $destination"
        initial_checkout=1
      fi
      if ! git -C "$destination" cat-file -e "$revision^{commit}" 2>/dev/null; then
        printf 'Fetching %s %s\n' "$label" "$revision"
        git -C "$destination" fetch \
          --no-tags \
          --depth=1 \
          --filter=blob:none \
          origin \
          "$revision"
      fi
      git -C "$destination" checkout --quiet --detach "$revision"
      [[ "$(git -C "$destination" rev-parse HEAD)" == "$revision" ]] \
        || fail "$label checkout did not reach $revision"
      if [[ $initial_checkout == 1 ]]; then
        require_clean_checkout "$label" "$destination"
      fi
    }

    write_gclient_config() {
      local repository="$1"
      local custom_vars='{}'

      if [[ "$DENIAL_ARCH" == aarch64 ]]; then
        custom_vars='{ "download_fuchsia_deps": False }'
      fi

      {
        printf '%s\n' \
          'solutions = [' \
          '  {' \
          '    "custom_deps": {},' \
          '    "deps_file": "DEPS",' \
          '    "managed": False,' \
          '    "name": ".",' \
          '    "safesync_url": "",'
        printf '    "custom_vars": %s,\n' "$custom_vars"
        printf '    "url": "%s",\n' "$repository"
        printf '%s\n' \
          '  },' \
          ']'
      } >"$CHECKOUT/.gclient"
    }

    sync_managed_checkout() (
      local filtered_deps

      cd "$CHECKOUT"
      if [[ "$DENIAL_ARCH" != aarch64 ]]; then
        PATH="$DEPOT_TOOLS/.cipd_bin:$DEPOT_TOOLS:$PATH" \
          DEPOT_TOOLS_UPDATE=0 \
          VPYTHON_VIRTUALENV_ROOT="$VPYTHON_CACHE_ROOT" \
          gclient sync --no-history --nohooks
        return
      fi

      # Flutter publishes version 21 of its formatting/Android JDK only for
      # linux-amd64. The desktop engine build does not use it, but gclient tries to
      # resolve it unconditionally. Filter that single dependency while syncing,
      # then restore the exact locked DEPS file before hooks and validation run.
      git -c core.hooksPath=/dev/null checkout --quiet -- DEPS
      filtered_deps="$(mktemp -- "$CHECKOUT/.DEPS.arm64.XXXXXX")"
      trap 'git -c core.hooksPath=/dev/null checkout --quiet -- DEPS; rm -f -- "$filtered_deps"' \
        EXIT HUP INT TERM
      if ! awk '
          BEGIN { skipping = 0; blocks = 0 }
          $0 == "  '\''engine/src/flutter/third_party/java/openjdk'\'': {" {
            skipping = 1
            blocks++
            next
          }
          skipping && $0 == "   }," { skipping = 0; next }
          !skipping { print }
          END { if (skipping || blocks != 1) exit 1 }
        ' DEPS >"$filtered_deps"; then
        fail 'could not isolate the Flutter OpenJDK dependency for ARM64 sync'
      fi
      chmod --reference=DEPS "$filtered_deps"
      mv -- "$filtered_deps" DEPS
      PATH="$DEPOT_TOOLS/.cipd_bin:$DEPOT_TOOLS:$PATH" \
        DEPOT_TOOLS_UPDATE=0 \
        VPYTHON_VIRTUALENV_ROOT="$VPYTHON_CACHE_ROOT" \
        gclient sync --no-history --nohooks
    )

    checkout_matches_lock() {
      local flutter_revision="$1"
      local skia_revision="$2"
      local checkout_depot_revision="$3"
      local flutter_repository
      local skia_repository
      local skia_checkout="$SKIA_CHECKOUT"
      local checkout_depot="$CHECKOUT/engine/src/flutter/third_party/depot_tools"

      flutter_repository="$(lock_value '.flutter.repository')"
      skia_repository="$(lock_value '.skia.repository')"
      [[ "$(git -C "$CHECKOUT" rev-parse --is-inside-work-tree 2>/dev/null || true)" \
        == true ]] || return 1
      [[ "$(git -C "$CHECKOUT" rev-parse HEAD 2>/dev/null || true)" \
        == "$flutter_revision" ]] || return 1
      [[ "$(git -C "$CHECKOUT" remote get-url origin 2>/dev/null || true)" \
        == "$flutter_repository" ]] || return 1
      require_clean_checkout 'Flutter' "$CHECKOUT"
      [[ "$(git -C "$skia_checkout" rev-parse --is-inside-work-tree 2>/dev/null || true)" \
        == true ]] || return 1
      [[ "$(git -C "$skia_checkout" rev-parse HEAD 2>/dev/null || true)" \
        == "$skia_revision" ]] || return 1
      [[ "$(git -C "$skia_checkout" remote get-url origin 2>/dev/null || true)" \
        == "$skia_repository" ]] || return 1
      require_clean_checkout 'Skia' "$skia_checkout"
      [[ "$(git -C "$checkout_depot" rev-parse --is-inside-work-tree 2>/dev/null || true)" \
        == true ]] || return 1
      [[ "$(git -C "$checkout_depot" rev-parse HEAD 2>/dev/null || true)" \
        == "$checkout_depot_revision" ]] || return 1
    }

    prepare_checkout() {
      local source_state="$1"
      local flutter_repository
      local flutter_revision
      local skia_revision
      local depot_repository
      local depot_revision
      local checkout_depot

      resolve_source_roots
      if [[ "$DENIAL_ARCH" == aarch64 ]]; then
        source_state="${source_state}:linux-arm64-sync-v1"
      fi

      flutter_repository="$(lock_value '.flutter.repository')"
      flutter_revision="$(lock_value '.flutter.revision')"
      skia_revision="$(lock_value '.skia.revision')"
      depot_repository="$(lock_value '.depot_tools.repository')"
      depot_revision="$(lock_value '.depot_tools.revision')"
      checkout_depot="$CHECKOUT/engine/src/flutter/third_party/depot_tools"

      if [[ -f "$STATE_ROOT/sources.sha256" ]] \
        && [[ "$(<"$STATE_ROOT/sources.sha256")" == "$source_state" ]] \
        && checkout_matches_lock \
          "$flutter_revision" "$skia_revision" "$depot_revision"; then
        printf 'Reusing synchronized Flutter/Skia checkout %s\n' \
          "${flutter_revision:0:12}"
        return
      fi

      if [[ "${DENIAL_FLUTTER_ENGINE_OFFLINE:-0}" == 1 ]]; then
        fail 'synchronized source cache is missing/stale; populate the source cache with ANLAND_OFFLINE=0 before an offline build'
      fi

      ensure_repository \
        'bootstrap depot_tools' \
        "$depot_repository" \
        "$depot_revision" \
        "$DEPOT_TOOLS"
      # A retained Flutter checkout has source-owned post-checkout hooks. Keep the
      # pinned bootstrap depot_tools available while moving it to a new lock so
      # those hooks resolve vpython3 without depending on the runner's host PATH.
      PATH="$DEPOT_TOOLS/.cipd_bin:$DEPOT_TOOLS:$PATH" \
        DEPOT_TOOLS_UPDATE=0 \
        VPYTHON_VIRTUALENV_ROOT="$VPYTHON_CACHE_ROOT" \
        ensure_repository \
          'Flutter' \
          "$flutter_repository" \
          "$flutter_revision" \
          "$CHECKOUT"
      write_gclient_config "$flutter_repository"

      printf 'Synchronizing Flutter dependencies for %s\n' \
        "${flutter_revision:0:12}"
      install -d -m 0700 "$VPYTHON_CACHE_ROOT"
      sync_managed_checkout

      [[ -x "$checkout_depot/gclient" ]] \
        || fail "synchronized depot_tools is missing: $checkout_depot"
      (
        cd "$CHECKOUT"
        PATH="$checkout_depot/.cipd_bin:$checkout_depot:$PATH" \
          DEPOT_TOOLS_UPDATE=0 \
          VPYTHON_VIRTUALENV_ROOT="$VPYTHON_CACHE_ROOT" \
          gclient runhooks
      )

      checkout_matches_lock \
        "$flutter_revision" "$skia_revision" "$depot_revision" \
        || fail 'synchronized checkout does not match the resolved Flutter DEPS'
      install -d -m 0700 "$STATE_ROOT"
      printf '%s\n' "$source_state" >"$STATE_ROOT/sources.sha256"
    }
    install -d -m 0700 "$CACHE_ROOT" "$STATE_ROOT" "$VPYTHON_CACHE_ROOT"
    prepare_checkout "$(file_sha256 "$SOURCE_LOCK")"
    engine_patch_manager apply \
        --checkout "$CHECKOUT" --patch "$ENGINE_PATCH" \
        --state "$STATE_ROOT" || fail 'engine local patch application failed'
)

engine_inputs_ready() {
    local directory=$1 file
    for file in dart-sdk/bin/dart gen_snapshot font-subset impellerc; do
        [[ -x $directory/$file ]] || return 1
    done
    for file in libflutter_engine.so icudtl.dat args.gn gen/dart-pkg/sky_engine/pubspec.yaml \
                flutter_patched_sdk/platform_strong.dill gen/const_finder.dart.snapshot; do
        [[ -s $directory/$file ]] || return 1
    done
}
engine_policy_id() {
    {
        printf '%s\n' "$ENGINE_POLICY" "$FLUTTER_REV"
        sha256sum "$ENGINE_SOURCES" | awk '{print $1}'
        sha256sum "$ENGINE_PATCH" | awk '{print $1}'
    } | sha256sum | awk '{print $1}'
}
engine_receipt_ready() {
    local directory=$1 identity
    engine_inputs_ready "$directory" || return 1
    [[ -f $directory/.anland-engine.json && -f $directory/.anland-engine.SHA256SUMS ]] || return 1
    identity=$(engine_policy_id) || return 1
    jq -e --arg identity "$identity" --arg policy "$ENGINE_POLICY" \
        --arg patch "$(sha256sum "$ENGINE_PATCH" | awk '{print $1}')" \
        '.schema == 1 and .identity == $identity and .policy == $policy
         and .local_patch_sha256 == $patch
         and (.package_version | test("^[A-Za-z0-9.+_]+-[0-9]+$"))
         and (.engine_sha256 | test("^[0-9a-f]{64}$"))
         and (.icu_sha256 | test("^[0-9a-f]{64}$"))' \
        "$directory/.anland-engine.json" >/dev/null || return 1
    (cd -- "$directory" && sha256sum --check --status .anland-engine.SHA256SUMS) || return 1
    [[ $(sha256sum "$directory/libflutter_engine.so" | awk '{print $1}') == \
        $(jq -r .engine_sha256 "$directory/.anland-engine.json") ]] || return 1
    [[ $(sha256sum "$directory/icudtl.dat" | awk '{print $1}') == \
        $(jq -r .icu_sha256 "$directory/.anland-engine.json") ]] || return 1
}
use_engine_receipt() {
    ENGINE_VERSION=$(jq -er .package_version "$ENGINE/.anland-engine.json") || die 'missing engine package identity'
    ENGINE_SHA256=$(jq -er .engine_sha256 "$ENGINE/.anland-engine.json") || die 'missing engine hash'
    ENGINE_ICU_SHA256=$(jq -er .icu_sha256 "$ENGINE/.anland-engine.json") || die 'missing engine ICU hash'
    DENIAL_ENGINE_POLICY='Impeller + Ganesh GLES/Vulkan, no-LTO, no-GTK'
}
verify_engine_symbols() {
    local exports symbol
    exports=$(nm -D --defined-only "$ENGINE/libflutter_engine.so") || die 'cannot inspect engine exports'
    for symbol in DenialFlutterEngineRequestFrameForExternalTextures DenialFlutterEngineScheduleFrameForExternalTextures \
                  FlutterEngineGetProcAddresses FlutterEngineRunsAOTCompiledDartCode; do
        grep -Eq "[[:space:]]${symbol}$" <<< "$exports" || die "missing engine API: $symbol"
    done
}
build_flutter_engine() {
    # The locked depot_tools ninja wrapper imports pipes, removed in Python 3.13.
    # Use Arch's native binary, not the Python wrapper selected by the depot PATH.
    local native_ninja=/usr/bin/ninja
    [[ -x $native_ninja ]] || die 'native Ninja missing (install the Arch ninja package)'
    run "$native_ninja" --version
    local identity generated_source="$FLUTTER/engine/src" rc
    local checkout_depot="$FLUTTER/engine/src/flutter/third_party/depot_tools"
    local engine_build="$WORKDIR/engine/build/$FLUTTER_REV" target=${ENGINE##*/} policy expected actual
    identity=$(engine_policy_id) || die 'cannot identify engine policy'
    [[ $ENGINE == "$engine_build/out/$target" && $target == denial_host_release_arm64_anland_ganesh ]] ||
        die 'compilation uses WORKDIR/engine/build/out/denial_host_release_arm64_anland_ganesh; prepared overrides are read-only'
    if [[ -f $ENGINE/libflutter_engine.so ]]; then
        [[ -f $ENGINE/.anland-engine-policy ]] ||
            die 'unrecognized engine output preserved; build receipt is required'
        if [[ $(cat "$ENGINE/.anland-engine-policy") != "$identity" ]]; then
            # Permit only the exact recorded v2 cache to migrate to v3 in place;
            # source and build files stay in the unique existing working copy.
            local previous_identity
            previous_identity=$({
                printf '%s\n' 'release-arm64-impeller-ganesh-vulkan-nolto-no-gtk-v2' "$FLUTTER_REV"
                sha256sum "$ENGINE_SOURCES" | awk '{print $1}'
            } | sha256sum | awk '{print $1}') || die 'cannot identify previous engine policy'
            [[ $(cat "$ENGINE/.anland-engine-policy") == "$previous_identity" ]] ||
                die 'unrecognized engine policy preserved'
            jq -e --arg previous "$previous_identity" --arg revision "$FLUTTER_REV" \
                --arg hash "$(sha256sum "$ENGINE/libflutter_engine.so" | awk '{print $1}')" \
                '.schema == 1 and .identity == $previous and .flutter_revision == $revision
                 and .engine_sha256 == $hash' "$ENGINE/.anland-engine.json" >/dev/null ||
                die 'unverified previous engine cache preserved'
            in_dir "$ENGINE" sha256sum --check --status .anland-engine.SHA256SUMS
        fi
    fi
    log 'Build current Flutter release engine and AOT tools (Impeller+Ganesh/Vulkan/no-LTO/no-GTK)'
    # Keep this a simple command: an OR-list would disable errexit in its isolated subshell.
    prepare_engine_sources
    rc=$?
    [[ $rc == 0 ]] || die "engine source synchronization failed: rc=$rc"
    run mkdir -p "$ENGINE"
    printf '%s\n' "$identity" > "$ENGINE/.anland-engine-policy" || die 'cannot write engine preparation identity'
    in_dir "$generated_source" env PATH="$checkout_depot/.cipd_bin:$checkout_depot:$PATH" \
        DEPOT_TOOLS_UPDATE=0 VPYTHON_VIRTUALENV_ROOT="$WORKDIR/engine/vpython" \
        ./flutter/tools/gn --linux --linux-cpu=arm64 --runtime-mode=release --enable-fontconfig \
        --no-lto --disable-desktop-embeddings --gn-args=shell_enable_vulkan=true \
        --gn-args=test_enable_vulkan=false --gn-args=skia_use_vulkan=true \
        --gn-args=slimpeller=false --gn-args=skia_enable_ganesh=true --gn-args=skia_enable_graphite=false \
        --out-dir="$engine_build" --target-dir="$target"
    for policy in enable_lto:false enable_desktop_embeddings:false shell_enable_vulkan:true skia_use_vulkan:true \
                  test_enable_vulkan:false slimpeller:false skia_enable_ganesh:true skia_enable_graphite:false host_cpu:arm64; do
        expected=${policy#*:}
        actual=$(awk -v key="${policy%%:*}" '$1 == key && $2 == "=" {value=$3} END {gsub(/"/, "", value); print value}' "$ENGINE/args.gn") ||
            die 'cannot read generated engine configuration'
        [[ $actual == "$expected" ]] || die "engine configuration differs: $policy (actual=$actual)"
    done
    in_dir "$generated_source" env PATH="$checkout_depot/.cipd_bin:$checkout_depot:$PATH" \
        DEPOT_TOOLS_UPDATE=0 VPYTHON_VIRTUALENV_ROOT="$WORKDIR/engine/vpython" \
        ./flutter/third_party/gn/gn gen --check "$ENGINE"
    run env PATH="$checkout_depot/.cipd_bin:$checkout_depot:$PATH" DEPOT_TOOLS_UPDATE=0 \
        VPYTHON_VIRTUALENV_ROOT="$WORKDIR/engine/vpython" "$native_ninja" -C "$ENGINE" -j "$JOBS" \
        libflutter_engine.so dart_sdk flutter/sky/packages/sky_engine:sky_engine \
        flutter_patched_sdk/platform_strong.dill gen_snapshot \
        flutter/tools/const_finder:const_finder flutter/tools/font_subset:font_subset flutter/impeller/compiler:impellerc
    engine_inputs_ready "$ENGINE" || die 'incomplete newly built engine/SDK'
    verify_engine_symbols
    check_elf "$ENGINE/libflutter_engine.so"
    ENGINE_SHA256=$(sha256sum "$ENGINE/libflutter_engine.so" | awk '{print $1}') || die 'cannot hash engine'
    ENGINE_ICU_SHA256=$(sha256sum "$ENGINE/icudtl.dat" | awk '{print $1}') || die 'cannot hash engine ICU'
    # Put a monotonic feature revision before the content hash: hash-only
    # versions may sort lower than the previous unpatched engine in APT.
    ENGINE_VERSION="${FLUTTER_VERSION//-/.}.r1.g${FLUTTER_REV:0:12}.vknolto.ganesh.shadowopacity1.h${ENGINE_SHA256:0:12}-1"
    DENIAL_ENGINE_POLICY='Impeller + Ganesh GLES/Vulkan, no-LTO, no-GTK'
    # New policy uses a content-derived package identity.
    in_dir "$ENGINE" sha256sum libflutter_engine.so icudtl.dat args.gn dart-sdk/bin/dart gen_snapshot font-subset impellerc \
        gen/dart-pkg/sky_engine/pubspec.yaml flutter_patched_sdk/platform_strong.dill gen/const_finder.dart.snapshot \
        > "$ENGINE/.anland-engine.SHA256SUMS" || die 'cannot record engine/tool checksums'
    run jq -n --arg identity "$identity" --arg policy "$ENGINE_POLICY" --arg version "$ENGINE_VERSION" \
        --arg engine "$ENGINE_SHA256" --arg icu "$ENGINE_ICU_SHA256" --arg flutter "$FLUTTER_REV" \
        --arg patch "$(sha256sum "$ENGINE_PATCH" | awk '{print $1}')" \
        '{schema:1, identity:$identity, policy:$policy, package_version:$version, engine_sha256:$engine, icu_sha256:$icu, flutter_revision:$flutter, local_patch_sha256:$patch}' \
        > "$ENGINE/.anland-engine.json" || die 'cannot record engine provenance'
    engine_receipt_ready "$ENGINE" || die 'new engine cache failed verification'
}
prepare_engine() {
    local lock="$ENGINE_SOURCES"
    local generated="$WORKDIR/engine/build/$FLUTTER_REV/out/denial_host_release_arm64_anland_ganesh"
    jq -e --arg flutter "$FLUTTER_REV" --arg repository "$FLUTTER_URL" \
        '.schema_version == 1 and .flutter.revision == $flutter and .flutter.repository == $repository
         and (.skia.revision | test("^[0-9a-f]{40}$"))
         and (.depot_tools.revision | test("^[0-9a-f]{40}$"))' "$lock" >/dev/null || die 'invalid resolved engine sources'
    run mkdir -p "$WORKDIR/engine"
    exec 7>"$WORKDIR/engine/build.lock" || die 'cannot open engine cache lock'
    flock -n 7 || die 'another builder owns the engine cache'
    if [[ $ANLAND_ENGINE_MODE == build ]]; then
        [[ -z ${DENIAL_ENGINE_DIR:-} || $ENGINE == "$generated" ]] ||
            die 'build mode cannot overwrite a prepared engine override; unset DENIAL_ENGINE_DIR'
        ENGINE=$generated
        build_flutter_engine
    else
        if [[ -z ${DENIAL_ENGINE_DIR:-} && -f $generated/.anland-engine.json ]]; then ENGINE=$generated; fi
        if engine_receipt_ready "$ENGINE"; then
            log 'Reuse verified engine/SDK build receipt'
            use_engine_receipt
        else
            [[ $ANLAND_ENGINE_MODE != prepared ]] || die 'prepared engine/SDK absent or invalid; prepared mode never builds it'
            [[ -z ${DENIAL_ENGINE_DIR:-} ]] || die 'unverified engine override preserved; unset DENIAL_ENGINE_DIR to build'
            [[ ! -f $ENGINE/libflutter_engine.so || -f $ENGINE/.anland-engine-policy ]] ||
                die 'unrecognized engine preserved; use build mode for a separate output'
            ENGINE=$generated
            build_flutter_engine
        fi
    fi
    verify_engine_symbols
    # package_all records the selected engine identity in cached inputs only.
}

prepare_flutter_tools() {
    local adapter_identity
    adapter_identity=$(declare -f prepare_flutter_tools | sha256sum | awk '{print $1}') || die 'cannot identify Flutter tool adapter'
    run python3 - --flutter-root "$FLUTTER" \
        --flutter-revision "$FLUTTER_REV" --plugin-kit-script "$SOURCE/tools/prepare-denial-plugin-kit" \
        --output "$FLUTTER_EMBEDDER" --adapter-identity "$adapter_identity" <<'PY'
"""Prepare managed Flutter tool inputs for Denial's non-GTK Linux embedder.

Keep the locked checkout untouched. Only omit desktop runner unpacking: Linux
AOT, asset/shader/font processing and native asset hooks remain upstream targets.
The plugin build kit vendors the same adapted tool sources.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from urllib.parse import urljoin


def digest(data):
    return hashlib.sha256(data).hexdigest()


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise SystemExit(f'unsupported locked {label}; expected exactly one adapter point')
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flutter-root', type=Path, required=True)
    parser.add_argument('--flutter-revision', required=True)
    parser.add_argument('--plugin-kit-script', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--adapter-identity', required=True)
    args = parser.parse_args()
    flutter = args.flutter_root.absolute()
    output = args.output.absolute()
    if (output.is_symlink() or output.resolve().is_relative_to(flutter.resolve()) or
            flutter.resolve().is_relative_to(output.resolve())):
        parser.error('output must be separate from the Flutter checkout')
    tool = flutter / 'packages/flutter_tools'
    config_path = tool / '.dart_tool/package_config.json'
    config_bytes = config_path.read_bytes()
    linux_source = (tool / 'lib/src/build_system/targets/linux.dart').read_text()
    linux = replace_once(
        linux_source, '    UnpackLinux(targetPlatform),',
        '    // Denial supplies its own embedder; do not unpack the GTK runner.',
        'Linux bundle dependency graph')
    kit_source = args.plugin_kit_script.read_text()
    kit = replace_once(
        kit_source,
        "'flutter_patched_sdk', 'flutter_linux', 'libflutter_linux_gtk.so',",
        "'flutter_patched_sdk',", 'plugin kit engine artifact list')
    config = json.loads(config_bytes)
    for package in config['packages']:
        package['rootUri'] = ('../' if package['name'] == 'flutter_tools' else
                              urljoin(config_path.as_uri(), package['rootUri']))
    config.pop('flutterRoot', None)
    config.pop('pubCache', None)
    config_text = json.dumps(config, indent=2) + '\n'
    marker = {
        'schema': 1, 'policy': 'linux-embedder-assets-no-gtk-v1',
        'flutter_root': str(flutter), 'flutter_revision': args.flutter_revision,
        'helper_sha256': args.adapter_identity,
        'source_linux_sha256': digest(linux_source.encode()),
        'source_config_sha256': digest(config_bytes),
        'source_kit_sha256': digest(kit_source.encode()),
    }
    generated = {
        'packages/flutter_tools/lib/src/build_system/targets/linux.dart': linux,
        'packages/flutter_tools/.dart_tool/package_config.json': config_text,
        'prepare-denial-plugin-kit': kit,
    }
    tool_paths = ['bin', 'lib', 'templates', 'static', 'pubspec.yaml', 'pubspec.lock', 'LICENSE']
    copied = {}
    for name in tool_paths:
        source = tool / name
        files = sorted(source.rglob('*')) if source.is_dir() else [source]
        for file in files:
            if file.is_file():
                copied['packages/flutter_tools/' + file.relative_to(tool).as_posix()] = digest(file.read_bytes())
    # Detect missing or modified copied files too, not just the two adapter points.
    marker['generated_sha256'] = {
        **copied, **{name: digest(text.encode()) for name, text in generated.items()},
    }
    receipt = output / '.anland-embedder-tools.json'
    if output.exists():
        if not receipt.is_file():
            parser.error(f'unmanaged output preserved: {output}')
        previous = json.loads(receipt.read_text())
        if previous.get('schema') != 1 or previous.get('policy') != marker['policy']:
            parser.error(f'unrecognized managed output preserved: {output}')
        if previous == marker and all(
                (output / name).is_file() and
                digest((output / name).read_bytes()) == expected
                for name, expected in marker['generated_sha256'].items()):
            print(f'Reuse verified non-GTK Flutter tool adapter: {output}')
            return
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for source in flutter.iterdir():
        if source.name != 'packages':
            (output / source.name).symlink_to(source, target_is_directory=source.is_dir())
    packages = output / 'packages'
    packages.mkdir()
    for source in (flutter / 'packages').iterdir():
        if source.name != 'flutter_tools':
            (packages / source.name).symlink_to(source, target_is_directory=source.is_dir())
    target_tool = packages / 'flutter_tools'
    target_tool.mkdir()
    for name in tool_paths:
        source = tool / name
        destination = target_tool / name
        if source.is_dir():
            shutil.copytree(source, destination)
        elif source.is_file():
            shutil.copy2(source, destination)
    for name, text in generated.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text)
    receipt.write_text(json.dumps(marker, indent=2) + '\n')
    print(f'Prepared non-GTK Flutter tools and plugin-kit adapter: {output}')


if __name__ == '__main__':
    main()
PY
}

prepare_flutter() {
    log 'Configure the verified engine/SDK for Denial AOT'
    checkout "$FLUTTER" "$FLUTTER_URL" "$FLUTTER_REV"
    check_sha "$ENGINE_SHA256" "$ENGINE/libflutter_engine.so"
    check_sha "$ENGINE_ICU_SHA256" "$ENGINE/icudtl.dat"
    local file cache="$FLUTTER/bin/cache" tool="$FLUTTER/packages/flutter_tools" engine_revision
    local adapter_identity
    FLUTTER_EMBEDDER="$WORKDIR/engine/flutter-embedder-sdk"
    local tool_identity="$FLUTTER_REV:$ENGINE_SHA256:$ENGINE_ICU_SHA256:${ENGINE##*/}"
    for file in dart-sdk/bin/dart gen_snapshot font-subset impellerc; do
        [[ -x $ENGINE/$file ]] || die "prepared engine tool missing: $ENGINE/$file"
    done
    for file in icudtl.dat gen/dart-pkg/sky_engine/pubspec.yaml flutter_patched_sdk/platform_strong.dill gen/const_finder.dart.snapshot; do
        [[ -f $ENGINE/$file ]] || die "prepared engine input missing: $ENGINE/$file"
    done
    run mkdir -p "$cache/pkg"
    link_cache "$ENGINE/dart-sdk" "$cache/dart-sdk"
    link_cache "$ENGINE/gen/dart-pkg/sky_engine" "$cache/pkg/sky_engine"
    engine_revision=$(cat "$FLUTTER/bin/internal/engine.version") || die 'missing Flutter engine revision'
    for file in engine.stamp engine-dart-sdk.stamp; do
        printf '%s\n' "$engine_revision" > "$cache/$file" || die 'cannot write SDK stamp'
    done
    printf '\n' > "$cache/engine.realm" || die 'cannot write engine realm'
    local_engine=("--local-engine-src-path=$(dirname -- "$(dirname -- "$ENGINE")")"
                  "--local-engine=${ENGINE##*/}" "--local-engine-host=${ENGINE##*/}")
    flutter=("$ENGINE/dart-sdk/bin/dart" "$cache/flutter_tools.snapshot" "${local_engine[@]}" --no-version-check --suppress-analytics)
    if [[ ! -s $tool/.dart_tool/package_config.json || ! -f $cache/flutter_tools.stamp ]] ||
       [[ $(cat "$cache/flutter_tools.stamp") != "$FLUTTER_REV:" ]]; then
        local locked=()
        [[ ! -f $tool/pubspec.lock ]] || locked=(--enforce-lockfile)
        in_dir "$tool" "$ENGINE/dart-sdk/bin/dart" pub get "${network[@]}" "${locked[@]}"
    fi
    # Validate every adapted/copied tool input even when an old snapshot exists.
    # Linux bundle assets need AOT + hooks, not the optional GTK desktop runner.
    prepare_flutter_tools
    adapter_identity=$(sha256sum "$FLUTTER_EMBEDDER/.anland-embedder-tools.json" | awk '{print $1}') || die 'cannot identify prepared Flutter tool inputs'
    tool_identity+=":$adapter_identity"
    if [[ ! -f $cache/flutter_tools.snapshot || ! -f $cache/flutter_tools.stamp || ! -f $cache/.anland-flutter-tools.identity ]] ||
       [[ $(cat "$cache/flutter_tools.stamp") != "$FLUTTER_REV:" ]] ||
       [[ $(cat "$cache/.anland-flutter-tools.identity") != "$tool_identity" ]]; then
        in_dir "$FLUTTER" "$ENGINE/dart-sdk/bin/dart" --verbosity=error \
            "--snapshot=$cache/flutter_tools.snapshot" --snapshot-kind=app-jit \
            "--packages=$FLUTTER_EMBEDDER/packages/flutter_tools/.dart_tool/package_config.json" --no-enable-mirrors \
            "$FLUTTER_EMBEDDER/packages/flutter_tools/bin/flutter_tools.dart" "${local_engine[@]}" --no-version-check --suppress-analytics --help
        printf '%s:\n' "$FLUTTER_REV" > "$cache/flutter_tools.stamp" || die 'cannot write Flutter stamp'
        printf '%s\n' "$tool_identity" > "$cache/.anland-flutter-tools.identity" || die 'cannot record Flutter tool inputs'
    fi
    if [[ ! -f $cache/flutter.version.json ]]; then
        in_dir "$FLUTTER" "${flutter[@]}" --version --machine > "$cache/flutter.version.json" || die 'Flutter version failed'
    fi
    # Supply a valid dynamic version when this branch has no release tag.
    run jq --arg version "$FLUTTER_VERSION" --arg revision "$FLUTTER_REV" \
        '.frameworkVersion=$version | .flutterVersion=$version | .frameworkRevision=$revision' \
        "$cache/flutter.version.json" > "$cache/flutter.version.json.new"
    run mv "$cache/flutter.version.json.new" "$cache/flutter.version.json"
}
fetch_dependencies() {
    log 'Fetch Cargo and Pub dependencies using their upstream lockfiles'
    local manifest app
    local manifests=(compositor) apps=(packages/denial_flutter_sdk dart_shell)
    if [[ $ANLAND_WITH_DE == 1 ]]; then
        manifests+=(native_app)
        apps+=(plugins/denial_desktop settings_app polkit_app packages/denial_sdk packages/denial_plugin_manager plugin_manager_app)
    fi
    for manifest in "${manifests[@]}"; do
        run cargo fetch --manifest-path "$SOURCE/$manifest/Cargo.toml" --locked --target aarch64-unknown-linux-gnu "${network[@]}"
    done
    for app in "${apps[@]}"; do
        in_dir "$SOURCE/$app" "${flutter[@]}" pub get --enforce-lockfile "${network[@]}"
    done
}
check_elf() {
    local file=$1 dynamic deps
    readelf -h "$file" | grep -q 'Machine:.*AArch64' || die "not AArch64 ELF: $file"
    dynamic=$(readelf -d "$file") || die "cannot inspect ELF: $file"
    if grep -E '(RPATH|RUNPATH)' <<< "$dynamic" | grep -Eq '/(home|data|cache)/'; then
        die "build path in runtime search: $file"
    fi
    # Dart AOT snapshots have no DT_NEEDED and need not pass ldd.
    if [[ $dynamic == *'(NEEDED)'* ]]; then
        deps=$(env -u LD_LIBRARY_PATH ldd "$file") || die "ldd failed: $file"
        [[ $deps != *'not found'* ]] || die "missing runtime library: $file: $deps"
    fi
}
build_native() {
    log 'Build public library, C bridge and Rust compositor'
    run cmake -S "$PUBLIC/producers/denial/anland_backend" -B "$BUILD/bridge" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib '-DCMAKE_INSTALL_RPATH=$ORIGIN'
    run cmake --build "$BUILD/bridge" --parallel "$JOBS"
    run env DESTDIR="$BUILD/native" cmake --install "$BUILD/bridge"
    run cargo build --manifest-path "$SOURCE/compositor/Cargo.toml" --release --locked --offline \
        --jobs "$JOBS" --features anland --bin deniald --bin denialctl
}
build_aot() {
    local app=$1
    log "Build Flutter AOT: $app"
    in_dir "$SOURCE/$app" "${flutter[@]}" assemble "--resource-pool-size=$JOBS" "--output=$BUILD/$app" \
        -dTargetFile=lib/main.dart -dBuildMode=release -dTargetPlatform=linux-arm64 \
        -dDartObfuscation=false -dTrackWidgetCreation=true -dTreeShakeIcons=true release_bundle_linux-arm64_assets
    [[ -d $BUILD/$app/flutter_assets && -f $BUILD/$app/lib/libapp.so ]] || die "incomplete AOT bundle: $app"
    check_elf "$BUILD/$app/lib/libapp.so"
}
build_desktop() {
    log 'Build native app runner, Polkit, portal and Plugin Manager'
    run cargo build --manifest-path "$SOURCE/native_app/Cargo.toml" --release --locked --offline --jobs "$JOBS"
    run cargo build --manifest-path "$SOURCE/compositor/Cargo.toml" --release --locked --offline --jobs "$JOBS" \
        -p denial -p denial-polkit-agent -p denial-portal --features anland --bin denial-polkit-agent --bin denial-portal
    local app name input="$BUILD/kit-input" release="$BUILD/kit-input/prebuilt/flutter-engine/linux-arm64-release"
    for app in settings_app polkit_app plugin_manager_app; do build_aot "$app"; done
    in_dir "$SOURCE/packages/denial_plugin_manager" "$ENGINE/dart-sdk/bin/dart" compile exe \
        bin/denial_plugins.dart "--output=$BUILD/denial-plugins"
    # Upstream's runtime build-kit format is generated, not a hand-maintained source lock.
    run rm -rf -- "$input" "$BUILD/plugin-build-kit"
    run mkdir -p "$release"
    for name in packages plugins protocol LICENSE compositor; do run ln -s "$SOURCE/$name" "$input/$name"; done
    # The upstream kit expects this filename; its contents describe this run's sources.
    run install -m644 "$ENGINE_SOURCES" "$input/prebuilt/flutter-engine/SOURCE_LOCK.json"
    printf '%s  libflutter_engine.so\n' "$ENGINE_SHA256" > "$release/libflutter_engine.so.sha256" || die 'engine metadata failed'
    for name in LICENSE.flutter LICENSE.third_party; do
        run install -m644 "$SOURCE/prebuilt/flutter-engine/linux-x64-release/$name" "$release/$name"
    done
    # The relocatable plugin tool must use the same no-GTK asset target.
    run python3 "$FLUTTER_EMBEDDER/prepare-denial-plugin-kit" --source-root "$input" --source-revision "$DENIAL_REV" \
        --flutter-root "$FLUTTER_EMBEDDER" --flutter-revision "$FLUTTER_REV" --engine-root "$(dirname -- "$(dirname -- "$ENGINE")")" \
        --engine-source-root "$FLUTTER/engine" --engine-target "${ENGINE##*/}" --platform linux-arm64 --output "$BUILD/plugin-build-kit"
}
record_package_outputs() {
    # Workflow version and verification evidence stay in the build cache/log.
    # No signatures, checksum lists, keys or build manifests enter artifacts.
    printf '%s\n' "${DENIAL_VERSION//-/.}" > "$WORKDIR/.denial-version" || die 'cannot record workflow version'
    log 'Verified native packages (SHA256 recorded in build log only)'
    local package
    for package in "${packages[@]}"; do
        [[ -s $package ]] || die "missing package: $package"
    done
    run sha256sum "${packages[@]}"
}

publish_native_package() {
    local source=$1 destination=$2
    if [[ -e $destination.sig || -L $destination.sig ]]; then
        [[ -f $destination ]] && cmp -s -- "$source" "$destination" ||
            die "existing signature preserved: $destination.sig; choose a clean ARTIFACTS_DIR before replacing this package"
    fi
    run cp -p -- "$source" "$destination"
}

collect_arch_packages() {
    local stage=$1 marker=$2 listed package destination package_name collected=0
    listed=$(in_dir "$stage" makepkg --packagelist) || die 'cannot obtain native package paths'
    [[ -n $listed ]] || die 'makepkg returned an empty package list'
    while IFS= read -r package; do
        [[ -n $package && -s $package && $package -nt $marker ]] ||
            die "package was not produced by this build: $package"
        run bsdtar -tf "$package" >/dev/null
        package_name=$(bsdtar -xOf "$package" .PKGINFO | sed -n 's/^pkgname = //p') ||
            die "cannot read package identity: $package"
        [[ -n $package_name && $package_name != *$'\n'* ]] || die "invalid package identity: $package"
        [[ $package_name != *-debug ]] || continue
        destination="$ARTIFACTS_DIR/${package##*/}"
        publish_native_package "$package" "$destination"
        packages+=("$destination")
        ((collected += 1))
    done <<< "$listed"
    ((collected > 0)) || die 'makepkg produced no runtime packages'
    run rm -f -- "$marker"
}

# Build the current Xwayland upstream branch with fuzzy patch context.
build_xwayland() {
    log 'Build patched Xwayland from the upstream default branch (no installation)'
    local stage="$WORKDIR/xwayland-package" sources="$WORKDIR/xwayland-sources" marker
    run mkdir -p "$stage" "$sources"
    run install -m644 "$SCRIPT_DIR/xorg-xwayland.PKGBUILD" "$stage/PKGBUILD"
    run install -m644 "${XWAYLAND_PATCH:-$SCRIPT_DIR/xwayland.patch}" "$stage/xwayland.patch"
    marker=$(mktemp "$WORKDIR/.xwayland-package-start.XXXXXX") || die 'cannot mark Xwayland build start'
    in_dir "$stage" env SRCDEST="$sources" BUILDDIR="$WORKDIR/xwayland-build" JOBS="$JOBS" \
        makepkg --nodeps --force --cleanbuild --noconfirm --nosign
    collect_arch_packages "$stage" "$marker"
}

package_all() {
    log 'Build Arch packages (no installation)'
    local pack="$WORKDIR/package" kind package file runtime app marker native_packages
    local package_version=${DENIAL_VERSION//-/.}

    local kinds=(engine wm) packages=()
    [[ $ANLAND_WITH_DE == 0 ]] || kinds+=(de)
    run mkdir -p "$pack"
    run cp "$SCRIPT_DIR/PKGBUILD" "$SCRIPT_DIR/sources.conf" "$SCRIPT_DIR/startup.sh" "$pack/"
    # Keep repository packaging files unchanged; specialize this build's cached copy.
    {
        printf '\n# Resolved upstream source identities selected by build.sh.\n'
        printf 'DENIAL_REV=%q\nDENIAL_VERSION=%q\nFLUTTER_REV=%q\nFLUTTER_VERSION=%q\n' \
            "$DENIAL_REV" "$DENIAL_VERSION" "$FLUTTER_REV" "$FLUTTER_VERSION"
        printf '\n# Verified engine output selected by build.sh.\n'
        printf 'ENGINE_VERSION=%q\nENGINE_SHA256=%q\nENGINE_ICU_SHA256=%q\n' \
            "$ENGINE_VERSION" "$ENGINE_SHA256" "$ENGINE_ICU_SHA256"
    } >> "$pack/sources.conf" || die 'cannot record cached engine identity'
    printf '\n[[ $_kind != engine ]] || pkgdesc=%q\n' \
        "Denial private ARM64 Flutter embedder engine ($DENIAL_ENGINE_POLICY)" \
        >> "$pack/PKGBUILD" || die 'cannot record cached engine description'
    export DENIAL_SOURCE_TREE="$SOURCE" DENIAL_BUILD_DIR="$BUILD" DENIAL_ENGINE_DIR="$ENGINE"
    # makepkg removes an existing same-name .sig even with --nosign. Build in
    # a fresh cache directory and publish only the verified native packages.
    native_packages=$(mktemp -d "$WORKDIR/package-output.XXXXXX") || die 'cannot stage native packages'
    export PKGDEST="$native_packages" BUILDDIR="$pack/build" PKGEXT=.pkg.tar.xz XZ_OPT="-T$JOBS"
    build_xwayland
    for kind in "${kinds[@]}"; do
        export DENIAL_PACKAGE_KIND=$kind
        # Package the verified payload without local release signatures.
        marker=$(mktemp "$WORKDIR/.denial-package-start.XXXXXX") || die 'cannot mark package build start'
        in_dir "$pack" makepkg --nodeps --force --noconfirm --nosign
        collect_arch_packages "$pack" "$marker"
    done
    log 'Check packaged files, runtime links and help/bundle commands; no graphical session'
    run rm -rf -- "$BUILD/package-check"
    run mkdir -p "$BUILD/package-check"
    for package in "${packages[@]}"; do
        local info
        info=$(bsdtar -xOf "$package" .PKGINFO) || die "cannot read package metadata: $package"
        grep -qx 'arch = aarch64' <<< "$info" || die "wrong package architecture: $package"
        case ${package##*/} in
            denial-anland-engine-*)
                grep -qx 'pkgname = denial-anland-engine' <<< "$info" || die 'wrong engine package name'
                grep -qx "pkgver = $ENGINE_VERSION" <<< "$info" || die 'wrong engine package version' ;;
            denial-"$package_version"-*)
                grep -qx 'pkgname = denial' <<< "$info" || die 'wrong WM package name'
                grep -qx "pkgver = $package_version-$ANLAND_PKGREL" <<< "$info" || die 'wrong WM package version'
                grep -qx "depend = denial-anland-engine=$ENGINE_VERSION" <<< "$info" || die 'wrong WM/engine dependency' ;;
            denial-desktop-"$package_version"-*)
                grep -qx 'pkgname = denial-desktop' <<< "$info" || die 'wrong desktop package name'
                grep -qx "pkgver = $package_version-$ANLAND_DE_PKGREL" <<< "$info" || die 'wrong desktop version'
                grep -qx "depend = denial=$package_version-$ANLAND_PKGREL" <<< "$info" || die 'wrong desktop/WM version dependency'
                grep -qx "depend = denial-anland-engine=$ENGINE_VERSION" <<< "$info" || die 'wrong desktop/engine dependency' ;;
        esac
        run bsdtar -xf "$package" -C "$BUILD/package-check"
    done
    runtime="$BUILD/package-check/usr/lib/denial-anland"
    check_sha "$ENGINE_SHA256" "$runtime/flutter/lib/libflutter_engine.so"
    check_sha "$ENGINE_ICU_SHA256" "$runtime/flutter/data/icudtl.dat"
    check_elf "$runtime/flutter/lib/libflutter_engine.so"
    for file in "$runtime/wm/deniald" "$runtime/wm/denialctl" "$runtime/wm/"*.so "$runtime/wm/shell/lib/"*.so; do check_elf "$file"; done
    run env -u LD_LIBRARY_PATH "$runtime/wm/deniald" --help
    local reported
    reported=$(env -u LD_LIBRARY_PATH "$runtime/wm/deniald" --version) || die 'version inspection failed'
    [[ $reported == *"$DENIAL_VERSION"* ]] || die "stale runtime version: $reported"
    [[ $(cat "$BUILD/package-check/usr/share/denial/version") == "$DENIAL_VERSION" ]] || die 'wrong release identity file'
    run "$BUILD/package-check/usr/bin/denial" --help
    run "$BUILD/package-check/usr/bin/denial" --version
    if [[ $ANLAND_WITH_DE == 1 ]]; then
        for file in "$runtime/desktop/native/"*; do [[ $file == *.json ]] || check_elf "$file"; done
        for app in settings:denial-settings polkit:denial-app plugins:denial-plugin-manager; do
            check_elf "$runtime/desktop/${app%%:*}/lib/libapp.so"
            run "$runtime/desktop/${app%%:*}/${app#*:}" --bundle "$runtime/desktop/${app%%:*}" \
                --engine "$runtime/flutter/lib/libflutter_engine.so" --check
        done
        run "$runtime/desktop/settings/denial-settings" --welcome --check
        run "$runtime/desktop/native/denial-plugins" --help
        for file in usr/lib/systemd/user/denial-session.target usr/lib/systemd/user/denial-polkit-agent.service \
            usr/lib/systemd/user/denial-portal.service usr/share/xdg-desktop-portal/portals/denial.portal \
            usr/share/xdg-desktop-portal/denial-portals.conf \
            usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.denial.service \
            usr/share/wayland-sessions/denial.desktop; do
            [[ -r $BUILD/package-check/$file ]] || die "missing desktop integration: $file"
        done
    fi
    record_package_outputs
    if [[ $ANLAND_INSTALL == 1 ]]; then
        run sudo pacman -U --needed "${packages[@]}"
    fi
    log "Done. Native packages: $ARTIFACTS_DIR (DE=$ANLAND_WITH_DE, installed=$ANLAND_INSTALL; no graphical test)"
}

# Declare host tools and runtime libraries explicitly: prepared-engine builds
# also need archive tools, and deniald dlopens libpulse.so.0 for audio controls.
build_deps=(base-devel cmake ninja rust python git curl jq pkgconf binutils libarchive gnupg dbus
            bash coreutils util-linux procps-ng ca-certificates glibc gcc-libs tar unzip zip xz zlib
            libpipewire libpulse libglvnd mesa libdrm libinput seatd libxkbcommon xkeyboard-config
            systemd-libs pam polkit fontconfig freetype2 wayland)
# Dependencies of the KDE Arch Xwayland recipe (makepkg does not install them).
build_deps+=(meson xorgproto xtrans libxkbfile xorg-font-util wayland-protocols mesa-libgl systemd
             nettle libepoxy libxfont2 pixman xorg-server-common libxcvt libxau libtirpc libei libxshmfence libdecor)
# Clean ARM64 CI workers can build missing engine inputs from the selected Flutter revision.
[[ $ANLAND_ENGINE_MODE == prepared ]] || build_deps+=(clang lld llvm glib2 libunwind libffi)
# Keep full-desktop packages' external dependencies available on clean workers;
# installing them never implies enabling/restarting any desktop service.
if [[ $ANLAND_WITH_DE == 1 ]]; then
    build_deps+=(xdg-utils zenity xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr)
fi
if [[ $ANLAND_DENIAL_DISTRO == ubuntu2604 ]]; then
    # Ubuntu reuses this exact engine/AOT implementation and supplies only native packaging.
    source "$SCRIPT_DIR/build.sh" || die 'cannot load Ubuntu package functions'
    ubuntu_preflight
    ubuntu_dependencies
elif [[ $ANLAND_DENIAL_DISTRO == debian13 ]]; then
    # Same engine/AOT backend; Debian supplies only distro preflight and packaging.
    source "$SCRIPT_DIR/build.sh" || die 'cannot load Debian package functions'
    debian_preflight
    debian_dependencies
    if [[ $ANLAND_STAGE == check ]]; then
        printf 'DEBIAN13_RECIPE_OK denial (no compilation or graphical test)\n'
        exit 0
    fi
elif [[ $ANLAND_DENIAL_DISTRO == fedora4[34] ]]; then
    source "$SCRIPT_DIR/build.sh" || die 'cannot load Fedora package functions'
    fedora_denial_preflight
    fedora_denial_dependencies
    if [[ $ANLAND_STAGE == check ]]; then
        printf 'FEDORA%s_RECIPE_OK denial (no compilation or graphical test)\n' "${ANLAND_DENIAL_DISTRO#fedora}"
        exit 0
    fi
else
    if [[ $ANLAND_WITH_DEPS == 1 ]]; then
        [[ $ANLAND_OFFLINE == 0 ]] || die 'dependency installation conflicts with ANLAND_OFFLINE=1'
        run sudo pacman -S --needed --noconfirm "${build_deps[@]}"
    fi
    missing=$(pacman -T "${build_deps[@]}") || die "missing dependencies: $missing (use ANLAND_WITH_DEPS=1)"
fi
prepare_sources
if [[ $ANLAND_DENIAL_DISTRO == debian13 && $ANLAND_STAGE == prepare ]]; then
    prepare_debian_rust
    prepare_xwayland
    printf 'DEBIAN13_PREPARED denial (no engine/AOT compilation or graphical test)\n'
    exit 0
fi
if [[ $ANLAND_DENIAL_DISTRO == fedora4[34] && $ANLAND_STAGE == prepare ]]; then
    fedora_xwayland
    printf 'FEDORA%s_PREPARED denial\n' "${ANLAND_DENIAL_DISTRO#fedora}"
    exit 0
fi
case $ANLAND_DENIAL_DISTRO in
    debian13) prepare_debian_rust ;;
    ubuntu2604) prepare_ubuntu_rust ;;
    fedora43|fedora44) prepare_fedora_rust ;;
esac
prepare_engine
prepare_flutter
fetch_dependencies
build_native
build_aot dart_shell
if [[ $ANLAND_WITH_DE == 1 ]]; then build_desktop; fi
package_all
