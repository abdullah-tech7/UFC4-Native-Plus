#!/usr/bin/env bash
# SacredFire Windows cross-build, stage 1: fetch the pinned inputs of packaging/windows/deps.lock.
# Archives go to out/windows/downloads and are verified against their sha256 (--pin records the
# hash of a download whose entry still says sha256:-); git inputs are checked out at their exact
# commit into out/windows/deps/src/<name>. Idempotent: present inputs are verified, not refetched.
# Only this stage touches the network; the later stages work from these directories.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
lock=packaging/windows/deps.lock
root=${BB_WINDOWS_ROOT:-$PWD/out/windows}
downloads=$root/downloads
sources=$root/deps/src
pin=0
[[ ${1:-} == --pin ]] && pin=1
mkdir -p "$downloads" "$sources"
failed=0
while read -r name kind source ref _licence _role; do
    [[ -z $name || $name == \#* ]] && continue
    case $kind in
    archive)
        file=$downloads/${source##*/}
        if [[ ! -f $file ]]; then
            echo "fetch: $name <- $source"
            curl -fsSL --retry 3 -o "$file.part" "$source" && mv "$file.part" "$file"
        fi
        actual=$(sha256sum "$file" | cut -d' ' -f1)
        if [[ $ref == sha256:- ]]; then
            if (( pin )); then
                sed -i "s|^$name archive $source sha256:- |$name archive $source sha256:$actual |" "$lock"
                echo "fetch: $name pinned sha256:$actual"
            else
                echo "fetch: $name has no pinned hash (sha256:$actual); run with --pin to record it" >&2; failed=1
            fi
        elif [[ $ref != "sha256:$actual" ]]; then
            echo "fetch: $name: checksum mismatch: expected ${ref#sha256:}, got $actual ($file)" >&2; failed=1
        else
            echo "fetch: $name verified"
        fi
        ;;
    git)
        dir=$sources/$name
        if [[ -d $dir/.git ]] && [[ $(git -C "$dir" rev-parse HEAD) == "$ref" ]]; then
            echo "fetch: $name at $ref"
            continue
        fi
        echo "fetch: $name <- $source @ $ref"
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" remote add origin "$source"
        git -C "$dir" fetch -q --depth 1 origin "$ref"
        git -C "$dir" checkout -q FETCH_HEAD
        git -C "$dir" submodule update -q --init --depth 1 --recursive 2>/dev/null || git -C "$dir" submodule update -q --init --recursive
        [[ $(git -C "$dir" rev-parse HEAD) == "$ref" ]] || { echo "fetch: $name: checkout is not $ref" >&2; failed=1; }
        ;;
    *) echo "fetch: $name: unknown kind $kind" >&2; failed=1 ;;
    esac
done < "$lock"
(( failed )) && { echo 'fetch: some inputs are missing or unverified' >&2; exit 1; }
echo "fetch: all inputs of $lock present in $root"
