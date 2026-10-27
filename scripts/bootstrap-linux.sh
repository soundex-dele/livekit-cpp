#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$project_dir/third_party"

checkout() {
  local name="$1" repository="$2" revision="$3"
  local destination="$project_dir/third_party/$name"
  if [[ ! -e "$destination" ]]; then
    git init -q "$destination"
    git -C "$destination" remote add origin "$repository"
  fi
  if [[ ! -e "$destination/.git" ]]; then
    printf 'Cannot verify existing non-Git source directory: %s\n' "$destination" >&2
    exit 1
  fi
  local actual
  if ! git -C "$destination" rev-parse --verify HEAD >/dev/null 2>&1; then
    if [[ -n "$(git -C "$destination" status --porcelain)" ]]; then
      printf 'Incomplete dependency checkout contains local files: %s\n' "$destination" >&2
      exit 1
    fi
    local fetch_source="$repository"
    if [[ -n "${LK_SOURCE_MIRROR:-}" ]]; then fetch_source="$LK_SOURCE_MIRROR/$name"; fi
    GIT_TERMINAL_PROMPT=0 timeout 90s git -C "$destination" \
      -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=30 fetch --depth 1 "$fetch_source" "$revision"
    git -C "$destination" checkout --detach FETCH_HEAD
  fi
  actual="$(git -C "$destination" rev-parse --verify HEAD)"
  if [[ "$actual" != "$revision" ]]; then
    printf '%s is at %s; required revision is %s. Existing sources were not changed.\n' "$name" "$actual" "$revision" >&2
    exit 1
  fi
  if ! git -C "$destination" diff --quiet HEAD --; then
    printf 'Pinned dependency has local changes: %s. Existing sources were not changed.\n' "$destination" >&2
    exit 1
  fi
}

checkout protocol https://github.com/livekit/protocol.git f09f87aab29b865850a967924726bf76b7db4755
checkout protobuf https://github.com/protocolbuffers/protobuf.git f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c
checkout yaml-cpp https://github.com/jbeder/yaml-cpp.git f7320141120f720aecc4c32be25586e7da9eb978
printf 'Pinned protocol, Protobuf 21.12 and yaml-cpp 0.8.0 sources are ready.\n'
