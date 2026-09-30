#!/usr/bin/env bash
# The registration table, measured in each WebAssembly build given (ADR 0069).
#
# Renders the photograph ring `registration_accuracy_test.cpp` renders — twelve frames, 640 by 480 —
# and runs `sphanorama_wasm_accuracy` over it under node, once per build directory. One script for
# the gate and CI, because the two inline copies of the native accuracy step have to be kept in step
# by hand, and this is the third.
#
# A run that measured nothing fails: the count of `[wasm-solved]` lines has to equal the detector
# count the binary reports, which is zero when the target was never built, and a missing runner is
# named rather than skipped.
#
# Usage:  tools/wasm_accuracy.sh build/wasm-release [build/wasm-release-threaded ...]
set -uo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <wasm build directory>..." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

if ! uv run --locked --group datasets tools/synth_dataset.py --out "$scratch/ring" --frames 12 \
    --width 640 --height 480 --panorama core/test/data/panoramas/small_hangar_01_1k.jpg \
    >"$scratch/render.log" 2>&1; then
  cat "$scratch/render.log"
  echo "the ring did not render, so nothing was measured" >&2
  exit 1
fi

status=0
for dir in "$@"; do
  runner="$dir/bin/sphanorama_wasm_accuracy.cjs"
  if [ ! -f "$runner" ]; then
    echo "$dir has no $runner: the build did not produce the measurement" >&2
    status=1
    continue
  fi
  expected=$(node "$runner" --detectors)
  node "$runner" "$scratch/ring" >"$scratch/run.log" 2>&1
  run=$?
  sed "s|^|$dir: |" "$scratch/run.log"
  measured=$(grep -c '^\[wasm-solved\]' "$scratch/run.log" || true)
  if [ "$run" -ne 0 ]; then
    echo "$dir: the measurement failed (exit $run)" >&2
    status=1
  fi
  if [ -z "$expected" ] || [ "$expected" -lt 1 ] || [ "$measured" -ne "$expected" ]; then
    echo "$dir: measured ${measured} detectors and the binary has ${expected:-none}" >&2
    status=1
  fi
done
exit $status
