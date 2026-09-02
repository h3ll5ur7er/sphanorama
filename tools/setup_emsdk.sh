#!/usr/bin/env bash
# Install the Emscripten SDK used to build the WASM core.
#
# Idempotent: re-running is a no-op once the requested version is active. The version is pinned
# rather than tracking "latest", because a toolchain that moves under you turns an unrelated
# upstream release into a red build on a day nobody touched this repo.
#
# Usage:  tools/setup_emsdk.sh [version]
#         source ~/emsdk/emsdk_env.sh     # to put emcc on PATH afterwards
set -euo pipefail

EMSDK_VERSION="${1:-${EMSDK_VERSION:-6.0.9}}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"

if [ ! -d "$EMSDK_DIR" ]; then
  echo "cloning emsdk into $EMSDK_DIR"
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"
git fetch --depth 1 origin main 2>/dev/null || true

echo "installing emscripten $EMSDK_VERSION"
./emsdk install "$EMSDK_VERSION"
./emsdk activate "$EMSDK_VERSION"

# shellcheck disable=SC1091
source ./emsdk_env.sh
echo "resolved: $(emcc --version | head -1)"
