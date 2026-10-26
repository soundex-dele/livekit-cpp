#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

# Ubuntu 22.04's CMake is older than the project minimum. Keep the newer tool
# local to this checkout rather than replacing the distribution executable.
cmake_command="${CMAKE_COMMAND:-cmake}"
cmake_version="$("$cmake_command" --version 2>/dev/null | head -1 | awk '{print $3}' || true)"
if [[ -z "$cmake_version" ]] || ! printf '%s\n' 3.24 "$cmake_version" | sort -V -C; then
  tool_env="$project_dir/out/linux-tools"
  if [[ ! -x "$tool_env/bin/cmake" ]]; then
    python3 -m venv "$tool_env"
    "$tool_env/bin/python" -m pip install --disable-pip-version-check 'cmake==3.31.6'
  fi
  cmake_command="$tool_env/bin/cmake"
fi
build_dir="${LK_BUILD_DIR:-$project_dir/out/linux-x64}"
"$cmake_command" --preset linux-x64 -B "$build_dir" "$@"
"$cmake_command" --build "$build_dir" --parallel "${LK_BUILD_JOBS:-4}" --target livekit-server livekit-tests livekit-native-rtp-tests livekit-native-send-tests livekit-turn-tests
ctest_command="$(dirname -- "$(command -v "$cmake_command")")/ctest"
"$ctest_command" --test-dir "$build_dir" --output-on-failure
