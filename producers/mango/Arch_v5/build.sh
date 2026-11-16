#!/usr/bin/env bash
# Arch Linux ARM, native aarch64. No build invokes startup.sh.
set -uo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
WORKDIR=${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-mango-arch-v5}
ARTIFACTS=${ARTIFACTS:-$SCRIPT_DIR/artifacts}
JOBS=${JOBS:-4}
die() { printf 'mango-build: %s\n' "$*" >&2; exit 1; }
run() { "$@" || die "failed: $*"; }
[[ $# == 0 ]] || die 'use environment variables, not positional arguments'
[[ $(id -u) != 0 ]] || die 'makepkg must run as an ordinary user'
[[ $(uname -m) == aarch64 && -f /etc/arch-release ]] || die 'native Arch Linux ARM aarch64 required'
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
for flag in ANLAND_INSTALL ANLAND_WITH_DEPS ANLAND_OFFLINE; do
    [[ ${!flag:-0} =~ ^[01]$ ]] || die "$flag must be 0 or 1"
done
[[ ${ANLAND_OFFLINE:-0} != 1 || ${ANLAND_WITH_DEPS:-0} != 1 ]] || die 'offline dependency installation is not supported'
for tool in python3 flock makepkg pacman tar sha256sum bsdtar; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
run mkdir -p "$WORKDIR" "$(dirname -- "$ARTIFACTS")"
[[ ! -L $ARTIFACTS ]] || die 'ARTIFACTS must not be a symlink'
WORKDIR=$(realpath "$WORKDIR") || die 'WORKDIR'; ARTIFACTS=$(realpath -m "$ARTIFACTS") || die 'ARTIFACTS'
[[ ! -e $ARTIFACTS || -d $ARTIFACTS ]] || die 'ARTIFACTS must be a directory'
[[ $SCRIPT_DIR != "$ARTIFACTS" && $SCRIPT_DIR != "$ARTIFACTS/"* &&
   $WORKDIR != "$ARTIFACTS" && $WORKDIR != "$ARTIFACTS/"* ]] || die 'ARTIFACTS must not contain the source or cache'
exec 8>"$WORKDIR/build.lock" || die 'cannot open build lock'
flock -n 8 || die 'WORKDIR is already in use'
# Keep the existing artifact lock shared by all builders using this directory.
lockdir="$(dirname -- "$ARTIFACTS")/.cache/anland-artifacts-locks"
run mkdir -p "$lockdir"
exec 9>"$lockdir/${ARTIFACTS##*/}.lock" || die 'cannot open artifacts lock'
flock -n 9 || die 'artifacts are already in use'
# Fingerprint includes lock, recipes, patch bytes, backend and public sources.
# Input preparation stays in this entrypoint; no companion Python file.
python3 - "$SCRIPT_DIR" "$WORKDIR" <<'PY_INPUTS' || die 'input preparation failed'
from pathlib import Path
import sys, json, hashlib, shutil, subprocess, tarfile
here, work = map(Path, sys.argv[1:])
root = here.parents[2]
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
lock = json.loads((here/'sources.json').read_text())
for name, expected in lock['patches'].items():
    if name != 'xwayland.patch' and sha(here/name) != expected:
        raise SystemExit('patch checksum mismatch: '+name)
for name, key in [('xwayland.patch','patch_sha256'), ('xorg-xwayland.PKGBUILD','recipe_sha256')]:
    if sha(here/name) != lock['xwayland'][key]:
        raise SystemExit('Xwayland checksum mismatch: '+name)
folders = [root/'common', root/'libdisplay_producer', here.parent/'anland_backend']
files = [root/'LICENSE'] + [p for p in here.iterdir() if p.is_file()]
for folder in folders:
    files.extend(p for p in folder.rglob('*') if p.is_file())
inputs = {str(p.relative_to(root)): sha(p) for p in sorted(files)}
identity = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
context = work/('inputs-'+identity)
if not context.exists():
    context.mkdir(); dst = context/'wm'; dst.mkdir(); (context/'xwayland').mkdir()
    with tarfile.open(dst/'anland-inputs.tar.gz', 'w:gz', dereference=True) as archive:
        for folder in folders:
            archive.add(folder, arcname='anland-inputs/'+('backend' if folder.name=='anland_backend' else folder.name))
        for p in here.glob('*.patch'):
            if p.name!='xwayland.patch': archive.add(p, arcname='anland-inputs/patches/'+p.name)
        for name in ('startup.sh','sources.json'):
            archive.add(here/name, arcname='anland-inputs/'+name)
        archive.add(root/'LICENSE', arcname='anland-inputs/LICENSE')
    (dst/'PKGBUILD').write_text((here/'PKGBUILD').read_text().replace('@INPUT_SHA256@', sha(dst/'anland-inputs.tar.gz')))
    shutil.copyfile(here/'xorg-xwayland.PKGBUILD', context/'xwayland/PKGBUILD')
    shutil.copyfile(here/'xwayland.patch', context/'xwayland/xwayland.patch')
    revision = subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    (context/'build-inputs.json').write_text(json.dumps({'input_identity':identity,'anland_revision':revision,'file_sha256':inputs,'lock':lock},indent=2)+'\n')
if not (context/'build-inputs.json').is_file():
    raise SystemExit('incomplete cached inputs: '+str(context))
(work/'input-id').write_text(identity+'\n')
print('Prepared makepkg inputs: '+str(context))
PY_INPUTS
input_id=$(cat "$WORKDIR/input-id") || die 'input identity unavailable'
BUILDROOT="$WORKDIR/inputs-$input_id"
export JOBS MAKEFLAGS="-j$JOBS"
export SRCDEST="$WORKDIR/downloads" PKGDEST="$BUILDROOT/packages"
run mkdir -p "$SRCDEST" "$PKGDEST"
# Keep system Xwayland isolated from the private WM dependency graph.
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CMAKE_PREFIX_PATH LD_LIBRARY_PATH DESTDIR
flags=(--force --cleanbuild --noconfirm)
[[ ${ANLAND_WITH_DEPS:-0} != 1 ]] || flags+=(--syncdeps)
[[ ${ANLAND_OFFLINE:-0} != 1 ]] || flags+=(--holdver)
packages=()
build_marker=$(mktemp "$BUILDROOT/.build-start.XXXXXX") || die 'cannot create build marker'
trap 'rm -f -- "$build_marker"' EXIT
for recipe in wm xwayland; do
    before=${#packages[@]}
    cwd="$BUILDROOT/$recipe"
    if [[ ${ANLAND_OFFLINE:-0} == 1 ]]; then
        # makepkg can otherwise fetch absent archives even with --holdver.
        (cd "$cwd" && bash -c 'source ./PKGBUILD; for item in "${source[@]}"; do
            if [[ $item == *git+* ]]; then file=${item%%::*};
            elif [[ $item == *::* ]]; then file=${item%%::*};
            else file=${item##*/}; fi
            [[ -e $file || -e $SRCDEST/$file ]] || { echo "offline source missing: $file" >&2; exit 1; }
        done') || die 'offline source preflight failed'
    fi
    printf '\n==> makepkg %s (input %s)\n' "$recipe" "$input_id"
    (cd "$cwd" && makepkg "${flags[@]}") || die "makepkg $recipe failed"
    (cd "$cwd" && makepkg --packagelist) > "$BUILDROOT/$recipe.packagelist" || die 'cannot query package list'
    while IFS= read -r file; do
        [[ -s $file && $file -nt $build_marker ]] || die "missing or stale package: $file"
        metadata=$(bsdtar -xOf "$file" .PKGINFO) || die "cannot read package metadata"
        pkgname=$(awk -F' = ' '$1 == "pkgname" {print $2; exit}' <<<"$metadata") || die 'cannot read package name'
        [[ -n $pkgname ]] || die "package name missing in metadata: $file"
        case $pkgname in *-debug|*-dbg|*-dbgsym) continue ;; esac
        pkgarch=$(awk -F' = ' '$1 == "arch" {print $2; exit}' <<<"$metadata") || die 'cannot read package architecture'
        [[ $pkgarch == aarch64 || $pkgarch == any ]] || die "wrong package architecture: $file"
        run pacman -Qip "$file"
        run bsdtar -tf "$file" >/dev/null
        packages+=("$file")
    done < "$BUILDROOT/$recipe.packagelist"
    [[ ${#packages[@]} -gt $before ]] || die "no binary packages returned: $recipe"
done
[[ ${#packages[@]} -ge 2 ]] || die 'incomplete package set'
# Publish only this build's native packages. Existing unrelated artifacts and
# signing keys are preserved; provenance remains under BUILDROOT.
run mkdir -p "$ARTIFACTS"
paths=()
for file in "${packages[@]}"; do
    destination="$ARTIFACTS/${file##*/}"
    if [[ -e $destination.sig || -L $destination.sig ]]; then
        cmp -s "$file" "$destination" || die "existing signature preserved; move $destination.sig aside before replacing its package"
    fi
    run cp -p "$file" "$destination"
    paths+=("$destination")
done
run sha256sum "${paths[@]}"
if [[ ${ANLAND_INSTALL:-0} == 1 ]]; then
    printf '\nInstalling private WM and replacing system xorg-xwayland; no session/service restart.\n'
    run sudo pacman --noconfirm -U "${paths[@]}"
fi
printf '\nPackages: %s\nBuild/package completed; graphical validation NOT performed.\n' "$ARTIFACTS"
