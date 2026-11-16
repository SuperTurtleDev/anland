#!/usr/bin/env bash

# Download the current SRPM visible to configured Fedora repositories and
# prepare a private build tree keyed by the actual SRPM and patch bytes.
# fedora_run, fedora_die, fedora_reuse and fedora_seal come from build.sh.
fedora_srpm_prepare() {
    local name=$1 patch=$2 overlay=${3:-} destination=${4:-} options=${5:-}
    local record downloads temp srpm archive version release actual_name sourcepackage sha
    local patch_sha identity safe_identity top spec

    [[ -f $patch ]] || fedora_die "patch not found: $patch"
    patch_sha=$(sha256sum "$patch" | awk '{print $1}') || fedora_die 'patch checksum'
    downloads="$WORKDIR/downloads"
    record="$WORKDIR/$name.latest-srpm"
    fedora_run mkdir -p "$downloads"

    if [[ ${ANLAND_OFFLINE:-0} == 1 ]]; then
        [[ -s $record ]] || fedora_die "no cached latest $name SRPM identity for offline use"
        IFS=$'\t' read -r archive version release sha < "$record"
        srpm="$downloads/$archive"
        [[ -s $srpm ]] || fedora_die "cached latest $name SRPM is missing: $srpm"
        printf '%s  %s\n' "$sha" "$srpm" | sha256sum -c --status - || fedora_die "cached $name SRPM checksum"
    else
        temp=$(mktemp -d "$downloads/.$name.XXXXXX") || fedora_die 'SRPM download directory'
        if ! dnf download --source --destdir="$temp" "$name"; then
            fedora_die "cannot download the latest $name SRPM; install the DNF download plugin"
        fi
        local -a srpms=()
        while IFS= read -r -d '' archive; do srpms+=("$archive"); done \
            < <(find "$temp" -maxdepth 1 -type f -name '*.src.rpm' -print0)
        [[ ${#srpms[@]} == 1 ]] || fedora_die "expected one current $name SRPM, found ${#srpms[@]}"
        srpm=${srpms[0]}
        IFS='|' read -r actual_name version release sourcepackage < <(
            rpm -qp --qf '%{NAME}|%{VERSION}|%{RELEASE}|%{SOURCEPACKAGE}\n' "$srpm"
        ) || fedora_die 'cannot inspect downloaded SRPM'
        [[ $actual_name == "$name" && $sourcepackage == 1 && -n $version && -n $release ]] ||
            fedora_die "unexpected SRPM identity: $actual_name-$version-$release"
        fedora_run rpm -K "$srpm"
        archive="${name}-${version}-${release}.src.rpm"
        sha=$(sha256sum "$srpm" | awk '{print $1}') || fedora_die 'SRPM checksum'
        if [[ -e $downloads/$archive ]]; then
            printf '%s  %s\n' "$sha" "$downloads/$archive" | sha256sum -c --status - ||
                fedora_die "existing SRPM cache differs: $archive"
            fedora_run rm -rf -- "$temp"
        else
            fedora_run mv -- "$srpm" "$downloads/$archive"
            fedora_run rm -rf -- "$temp"
        fi
        srpm="$downloads/$archive"
        printf '%s\t%s\t%s\t%s\n' "$archive" "$version" "$release" "$sha" > "$record" ||
            fedora_die "cannot record current $name SRPM"
    fi

    IFS='|' read -r actual_name version release sourcepackage < <(
        rpm -qp --qf '%{NAME}|%{VERSION}|%{RELEASE}|%{SOURCEPACKAGE}\n' "$srpm"
    ) || fedora_die 'cannot inspect cached SRPM'
    [[ $actual_name == "$name" && $sourcepackage == 1 ]] || fedora_die 'cached SRPM identity mismatch'
    if [[ $name == mutter ]]; then
        local mutter_major=${version%%.*}
        [[ $mutter_major =~ ^[0-9]+$ ]] || fedora_die 'cannot derive Mutter API major from current SRPM'
        options+=" -Dc_args=\"%{build_cflags} -DANLAND_MUTTER_$mutter_major\""
    fi
    fedora_run rpm -K "$srpm"

    # Include the recipe, downloaded SRPM and Anland patch in the cache key.
    identity=$(printf '%s\n%s\n%s\n%s\n%s\n%s\n' \
        "$FEDORA_INPUT_ID" "$sha" "$patch_sha" "$destination" "$options" patch-fuzz-2 | sha256sum | awk '{print $1}') ||
        fedora_die 'SRPM source identity'
    local FEDORA_INPUT_ID=$identity
    safe_identity=${version//[^A-Za-z0-9._+-]/_}-${release//[^A-Za-z0-9._+-]/_}-${identity:0:12}
    top="$WORKDIR/srpm-$name-$safe_identity"
    FEDORA_SRPM_TOP=$top
    FEDORA_SRPM_SPEC="$top/SPECS/$name.spec"
    if fedora_reuse "$top"; then return; fi
    [[ ! -e $top && ! -L $top ]] || fedora_die "incomplete source tree preserved: $top"

    fedora_run mkdir -p "$top"/{SOURCES,SPECS,BUILD,BUILDROOT,RPMS,SRPMS}
    fedora_run rpm -ivh --define "_topdir $top" "$srpm"
    spec=$FEDORA_SRPM_SPEC
    [[ -f $spec ]] || fedora_die "SRPM did not provide $name.spec"
    fedora_run cp "$patch" "$top/SOURCES/anland.patch"
    if [[ -n $overlay ]]; then
        [[ -d $overlay ]] || fedora_die "source overlay not found: $overlay"
        fedora_run tar -chf "$top/SOURCES/anland-overlay.tar" -C "$overlay" .
    fi
    python3 - "$spec" "$overlay" "$destination" "$options" <<'PY' || fedora_die 'unsupported Fedora SRPM recipe'
from pathlib import Path
import re,sys
p=Path(sys.argv[1]); overlay,destination,options=sys.argv[2:]
s=p.read_text()
if len(re.findall(r'^%autosetup[^\n]*$',s,re.M)) != 1: raise SystemExit('expected one autosetup')
used=[int(n or 0) for n in re.findall(r'^Source(\d*)\s*:',s,re.M)]
n=max(used,default=0)+1
sources=f'Source{n}: anland.patch\n'
prep=f'\npatch --batch --forward --fuzz=2 --no-backup-if-mismatch -p1 < %{{SOURCE{n}}}'
if overlay:
    if not re.fullmatch(r'[a-zA-Z0-9_./-]+',destination) or '..' in destination.split('/'):
        raise SystemExit('invalid overlay path')
    sources+=f'Source{n+1}: anland-overlay.tar\n'
    prep+=f'\nmkdir -p {destination}\ntar xf %{{SOURCE{n+1}}} -C {destination}'
s=re.sub(r'^(%description(?:[^\n]*))$',lambda m:sources+'\n'+m[0],s,count=1,flags=re.M)
s=re.sub(r'^(%autosetup[^\n]*)$',lambda m:m[0]+prep,s,count=1,flags=re.M)
s=re.sub(r'^Release:[^\n]*$', 'Release: 1.anland%{?dist}',s,count=1,flags=re.M)
if options:
    if len(re.findall(r'^%meson(?:\s|$)',s,re.M)) != 1: raise SystemExit('expected one meson invocation')
    s=re.sub(r'^%meson(?=\s|$)',lambda m:'%meson '+options,s,count=1,flags=re.M)
p.write_text(s)
PY
    if [[ $ANLAND_WITH_DEPS == 1 ]]; then
        fedora_run sudo dnf --setopt=install_weak_deps=False builddep -y "$spec"
    fi
    fedora_run rpmbuild -bp --nodeps --define "_topdir $top" "$spec"
    fedora_seal "$top"
}
