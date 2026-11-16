#!/usr/bin/env bash

# Fetch the newest source package visible to the configured APT repositories,
# then apply an Anland patch with up to two context lines of fuzz. The prepared-tree names
# include content identities, so a newer upstream source never overwrites an
# existing cache or local edits.
anland_prepare_latest_apt_source() {
    local package=$1 workdir=$2 patch_file=${3:-} offline=${4:-0} extra_identity=${5:-} suite=${6:-} requested_version=${7:-}
    local source_target=$package
    [[ -z $suite ]] || source_target+="/$suite"
    local downloads latest_record temp dsc version safe_version source_id patch_id identity tree file src_cache
    local -a dscs=()
    downloads="$workdir/source-downloads"
    latest_record="$workdir/$package.latest-source"
    [[ -z $suite ]] || latest_record="$workdir/$package-$suite.latest-source"
    if [[ -n $requested_version ]]; then
        source_target="$package=$requested_version"
        latest_record="$workdir/$package-${requested_version//:/_}.latest-source"
    fi
    mkdir -p "$downloads" || return 1

    if [[ $offline == 1 ]]; then
        [[ -s $latest_record ]] || {
            printf 'No previously resolved latest %s source for offline use\n' "$package" >&2
            return 1
        }
        IFS=$'\t' read -r version source_id < "$latest_record"
        [[ $version && $source_id ]] || return 1
        src_cache="$downloads/$package-$source_id"
    else
        if ! grep -rqsE '^Types:.*deb-src|^deb-src[[:space:]]' \
                /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
            if [[ -f /etc/apt/sources.list.d/debian.sources ]]; then
                sudo sed -i 's/^Types:[[:space:]]*deb[[:space:]]*$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources || return 1
            elif [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
                sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources || return 1
            elif [[ -f /etc/apt/sources.list ]]; then
                sudo sed -i 's/^deb \(.*\)$/deb \1\ndeb-src \1/' /etc/apt/sources.list || return 1
            else
                printf 'Enable deb-src repositories to fetch latest %s source\n' "$package" >&2
                return 1
            fi
        fi
        if [[ $(id -u) == 0 ]]; then apt-get update >&2; else sudo apt-get update >&2; fi || return 1
        temp=$(mktemp -d "$downloads/.$package.XXXXXX") || return 1
        if ! (cd "$temp" && apt-get source --download-only --only-source "$source_target" >&2); then
            printf 'Cannot fetch latest %s source; enable deb-src repositories\n' "$package" >&2
            return 1
        fi
        while IFS= read -r -d '' file; do dscs+=("$file"); done \
            < <(find "$temp" -maxdepth 1 -type f -name '*.dsc' -print0)
        [[ ${#dscs[@]} == 1 ]] || {
            printf 'Expected one %s source descriptor, found %s\n' "$package" "${#dscs[@]}" >&2
            return 1
        }
        dsc=${dscs[0]}
        version=$(sed -n 's/^Version:[[:space:]]*//p' "$dsc" | head -n 1)
        [[ $version ]] || return 1
        source_id=$(cd "$temp" && find . -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
        src_cache="$downloads/$package-$source_id"
        if [[ -e $src_cache ]]; then
            rm -rf -- "$temp"
        else
            mv -- "$temp" "$src_cache" || return 1
        fi
        printf '%s\t%s\n' "$version" "$source_id" > "$latest_record" || return 1
    fi

    dsc=$(find "$src_cache" -maxdepth 1 -type f -name '*.dsc' -print -quit)
    [[ -s $dsc ]] || {
        printf 'Cached %s source descriptor is missing\n' "$package" >&2
        return 1
    }
    version=${version:-$(sed -n 's/^Version:[[:space:]]*//p' "$dsc" | head -n 1)}
    safe_version=${version//:/_}
    patch_id=none
    if [[ -n $patch_file ]]; then
        [[ -f $patch_file ]] || return 1
        patch_id=$(sha256sum "$patch_file" | cut -d' ' -f1) || return 1
    fi
    source_id=${source_id:-$(find "$src_cache" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)}
    identity=$(printf '%s\n%s\n%s\n%s\n' "$source_id" "$patch_id" "$extra_identity" patch-fuzz-2 | sha256sum | cut -d' ' -f1) || return 1
    tree="$workdir/src/$package-$safe_version-${identity:0:12}"
    mkdir -p "$workdir/src" || return 1
    if [[ -d $tree ]]; then
        [[ -f $tree/.anland-source-identity && $(cat "$tree/.anland-source-identity") == "$identity" ]] || {
            printf 'Prepared source differs; preserving existing tree: %s\n' "$tree" >&2
            return 1
        }
    else
        dpkg-source -x "$dsc" "$tree" >&2 || return 1
        if [[ -n $patch_file ]]; then
            patch --batch --forward --fuzz=2 --no-backup-if-mismatch -p1 -d "$tree" -i "$patch_file" >&2 || return 1
        fi
        printf '%s\n' "$identity" > "$tree/.anland-source-identity" || return 1
    fi
    printf '%s\t%s\n' "$tree" "$version"
}

# Clone the default branch's current HEAD. Prepared trees are keyed by commit
# and patch content, so fetching a newer upstream commit never replaces a
# build tree that may contain local edits.
anland_prepare_latest_git_source() {
    local name=$1 url=$2 workdir=$3 patch_file=${4:-} offline=${5:-0} extra_identity=${6:-}
    local history=${7:-shallow} ref=${8:-} suffix= downloads record temp commit repo patch_id identity tree
    local -a clone_options=(--quiet)
    case $history in
        shallow) clone_options+=(--depth=1) ;;
        full) clone_options+=(--single-branch --filter=blob:none); suffix=-history ;;
        *) printf 'Unknown Git history mode: %s\n' "$history" >&2; return 1 ;;
    esac
    downloads="$workdir/git-sources"
    record="$workdir/$name.latest-git"
    mkdir -p "$downloads" "$workdir/src" || return 1

    if [[ $offline == 1 ]]; then
        [[ -s $record ]] || {
            printf 'No previously resolved latest %s commit for offline use\n' "$name" >&2
            return 1
        }
        IFS=$'\t' read -r commit < "$record"
        [[ $commit =~ ^[0-9a-f]{40,64}$ ]] || return 1
        repo="$downloads/$name-$commit$suffix.git"
        [[ -d $repo/.git || -f $repo/HEAD ]] || {
            printf 'Cached %s repository is missing\n' "$name" >&2
            return 1
        }
    else
        temp=$(mktemp -d "$downloads/.$name.XXXXXX") || return 1
        rmdir "$temp" || return 1
        if [[ -n $ref ]]; then
            local -a fetch_options=(--quiet --filter=blob:none)
            [[ $history != shallow ]] || fetch_options+=(--depth=1)
            git init --quiet "$temp" &&
                git -C "$temp" remote add origin "$url" &&
                git -C "$temp" fetch "${fetch_options[@]}" origin "$ref" &&
                git -C "$temp" checkout --quiet --detach FETCH_HEAD || return 1
        elif ! git clone "${clone_options[@]}" "$url" "$temp"; then
            rm -rf -- "$temp"
            return 1
        fi
        commit=$(git -C "$temp" rev-parse HEAD) || return 1
        [[ $commit =~ ^[0-9a-f]{40,64}$ ]] || return 1
        repo="$downloads/$name-$commit$suffix.git"
        if [[ -e $repo ]]; then
            rm -rf -- "$temp"
        else
            mv -- "$temp" "$repo" || return 1
        fi
        printf '%s\n' "$commit" > "$record" || return 1
    fi

    patch_id=none
    if [[ -n $patch_file ]]; then
        [[ -f $patch_file ]] || return 1
        patch_id=$(sha256sum "$patch_file" | cut -d' ' -f1) || return 1
    fi
    identity=$(printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' "$url" "$commit" "$patch_id" "$name" "$extra_identity" "$history" patch-fuzz-2 | sha256sum | cut -d' ' -f1) || return 1
    tree="$workdir/src/$name-$commit-${identity:0:12}"
    if [[ -d $tree ]]; then
        [[ -f $tree/.anland-source-identity && $(cat "$tree/.anland-source-identity") == "$identity" ]] || {
            printf 'Prepared source differs; preserving existing tree: %s\n' "$tree" >&2
            return 1
        }
    else
        git clone --quiet --no-hardlinks "$repo" "$tree" || return 1
        if [[ -n $patch_file ]]; then
            patch --batch --forward --fuzz=2 --no-backup-if-mismatch -p1 -d "$tree" -i "$patch_file" >&2 || return 1
        fi
        printf '%s\n' "$identity" > "$tree/.anland-source-identity" || return 1
    fi
    printf '%s\t%s\n' "$tree" "$commit"
}
