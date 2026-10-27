#!/bin/sh
set -eu

archive_url='https://alist.homelabproject.cc/p/foxipan/vGPU/CMP_90HX/GraphicsUnlock/nvpermissive-dist-380bdf3.tar.gz'
archive_sha='d5e89e77e121e1295cb39d331e0d511275b6edc1d7236376e0fd13ac6f0c79c0'
core_sha='c9702b4887d397272f86dcc25eea2bb11a46d636c91311d7b71f2fb8b01951e5'

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
vendor_dir="$project_dir/vendor"
archive="$vendor_dir/nvpermissive-dist-380bdf3.tar.gz"
core="$vendor_dir/nvpermissive-core.o"

mkdir -p "$vendor_dir"

if [ ! -f "$archive" ]; then
    curl -fL --retry 2 --output "$archive" "$archive_url"
fi

printf '%s  %s\n' "$archive_sha" "$archive" | sha256sum -c -

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT INT TERM
tar -xzf "$archive" -C "$tmp_dir"
found=$(find "$tmp_dir" -type f -path '*/obj/nvpermissive-core.o' -print -quit)
if [ -z "$found" ]; then
    echo 'nvpermissive-core.o was not present in the pinned archive' >&2
    exit 1
fi
cp "$found" "$core"
printf '%s  %s\n' "$core_sha" "$core" | sha256sum -c -

