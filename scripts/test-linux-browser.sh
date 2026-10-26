#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
export LK_SERVER="${LK_SERVER:-$project_dir/out/linux-x64/bin/livekit-server}"
export LK_CHROME="${LK_CHROME:-$(command -v google-chrome || true)}"
export LK_TEST_RESULTS="${LK_TEST_RESULTS:-$project_dir/tests/results/linux-browser/$(date +%Y%m%d-%H%M%S)}"
export LK_TEST_PORT="${LK_TEST_PORT:-17980}"
unset LK_EXTERNAL_SERVER LK_TEST_URL
if [[ ! -x "$LK_SERVER" || ! -x "$LK_CHROME" ]]; then
  printf 'Set LK_SERVER and LK_CHROME to executable Linux server and Chrome binaries.\n' >&2
  exit 1
fi
node -e 'if (Number(process.versions.node.split(".")[0]) < 18) throw Error("Node.js 18+ required")'
if [[ ! -d tests/node_modules/playwright ]]; then
  printf 'Install test dependencies first: cd tests && npm ci\n' >&2
  exit 1
fi
mkdir -p "$LK_TEST_RESULTS"
sha256sum "$LK_SERVER" "$LK_CHROME" > "$LK_TEST_RESULTS/binaries.sha256"
node --version > "$LK_TEST_RESULTS/node-version.txt"
"$LK_CHROME" --version > "$LK_TEST_RESULTS/browser-version.txt"
node tests/integration.mjs > "$LK_TEST_RESULTS/console.log" 2>&1
printf 'Linux browser results: %s\n' "$LK_TEST_RESULTS"
