#!/usr/bin/env bash
# The registration table, measured in each WebAssembly build given (ADR 0069).
#
# Renders the photograph ring `registration_accuracy_test.cpp` measures, at the shape the runner
# itself reports (`--ring`, from `core/test/support/solved_ring.h`, so there is no copy here to
# drift), and runs `sphanorama_wasm_accuracy` over it under node, once per build directory. One
# script for the gate and CI, because the two inline copies of the native accuracy step have to be
# kept in step by hand, and this is the third.
#
# A run that measured nothing fails: the count of `[wasm-solved]` lines has to equal the detector
# count the binary reports, and anything but a clean answer to `--detectors` or `--ring` — a failed
# call, a banner line, a word — is a failure rather than a comparison bash cannot make. A missing
# runner is named rather than skipped.
#
# Usage:  tools/wasm_accuracy.sh build/wasm-release [build/wasm-release-threaded ...]
set -uo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <wasm build directory>..." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# A whole number, and nothing else: no sign, no space, no second line.
count() {
  case "$1" in
    ''|*[!0-9]*) return 1 ;;
  esac
}

status=0
for dir in "$@"; do
  runner="$dir/bin/sphanorama_wasm_accuracy.cjs"
  if [ ! -f "$runner" ]; then
    echo "$dir has no $runner: the build did not produce the measurement" >&2
    status=1
    continue
  fi
  if ! expected=$(node "$runner" --detectors) || ! count "$expected" || [ "$expected" -lt 1 ]; then
    echo "$dir: --detectors did not answer with a count of at least one: '${expected:-}'" >&2
    status=1
    continue
  fi
  ring=$(node "$runner" --ring) || ring=
  read -r frames width height extra <<<"$ring"
  if [ -n "${extra:-}" ] || ! count "${frames:-}" || ! count "${width:-}" || ! count "${height:-}" \
      || [ "$(printf '%s' "$ring" | wc -l)" -ne 0 ]; then
    echo "$dir: --ring did not answer with frames, width and height: '$ring'" >&2
    status=1
    continue
  fi
  rendered="$scratch/ring-$frames-$width-$height"
  if [ ! -d "$rendered" ] && ! uv run --locked --group datasets tools/synth_dataset.py \
      --out "$rendered" --frames "$frames" --width "$width" --height "$height" \
      --panorama core/test/data/panoramas/small_hangar_01_1k.jpg >"$scratch/render.log" 2>&1; then
    cat "$scratch/render.log"
    echo "$dir: the ring did not render, so nothing was measured" >&2
    status=1
    continue
  fi
  node "$runner" "$rendered" >"$scratch/run.log" 2>&1
  run=$?
  sed "s|^|$dir: |" "$scratch/run.log"
  measured=$(grep -c '^\[wasm-solved\]' "$scratch/run.log" || true)
  if [ "$run" -ne 0 ]; then
    echo "$dir: the measurement failed (exit $run)" >&2
    status=1
  fi
  if [ "$measured" -ne "$expected" ]; then
    echo "$dir: measured $measured detectors and the binary has $expected" >&2
    status=1
  fi
done
exit $status
