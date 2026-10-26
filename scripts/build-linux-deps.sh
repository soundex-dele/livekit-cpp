#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${1:-$project_dir/out/linux-deps}"
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
cache="$project_dir/out/linux-deps-cache"
mkdir -p "$cache"
archive="${DAV1D_ARCHIVE:-$cache/dav1d-1.5.3.tar.gz}"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 2 'https://github.com/videolan/dav1d/archive/refs/tags/1.5.3.tar.gz' -o "$archive"
fi
# Same release archive and checksum as the pinned Windows vcpkg dependency.
expected=8d976b93135213d41385c20205475269a6826a68ebfd716c4d9a7a3ff2a79703e8df0573e43207c81b5db44807d2721db18ec84c0fc6bef98efab86a2cccb6cc
actual="$(sha512sum "$archive")"
if [[ "${actual%% *}" != "$expected" ]]; then
  printf 'dav1d archive checksum mismatch: %s\n' "$archive" >&2
  exit 1
fi
source_dir="$cache/dav1d-1.5.3"
if [[ ! -f "$source_dir/meson.build" ]]; then
  tar -xzf "$archive" -C "$cache"
fi
build_dir="$cache/build-dav1d"
reconfigure=()
if [[ -f "$build_dir/build.ninja" ]]; then reconfigure=(--reconfigure); fi
meson setup "${reconfigure[@]}" "$build_dir" "$source_dir" --prefix "$prefix" --libdir lib \
  --default-library static --buildtype release -Denable_tools=false -Denable_tests=false
ninja -C "$build_dir" -j 4 install
printf 'dav1d 1.5.3 installed; configure with -DCMAKE_PREFIX_PATH=%s\n' "$prefix"
