#!/usr/bin/env bash
# The registration table, measured in each WebAssembly build given (ADR 0069).
#
# Renders the photograph ring `registration_accuracy_test.cpp` measures, at the shape and from the
# panorama the runner itself reports (`--ring`, from `core/test/support/solved_ring.h` and
# `photograph.h`, so there is no copy here to drift), and runs `sphanorama_wasm_accuracy` over it under node, once per build directory. One
# script for the gate and CI, because the two inline copies of the native accuracy step have to be
# kept in step by hand, and this is the third.
#
# A run that measured nothing fails: the count of `[wasm-solved]` lines has to equal the detector
# count the binary reports, and anything but a clean answer to `--detectors` or `--ring` — a failed
# call, a banner line, a word, a number too wide for `[` — is a failure rather than a comparison
# bash cannot make. Every comparison is written so that one bash cannot make fails too: `[` answers
# an error with status 2, which an `if [ … ]` reads as false and so as a pass. A missing runner is
# named rather than skipped.
#
# Usage:  tools/wasm_accuracy.sh build/wasm-release [build/wasm-release-threaded ...]
set -uo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <wasm build directory>..." >&2
  exit 2
fi

# The panorama `--ring` names is relative to the repository root, as the native renderer reads it,
# so it and the renderer are found from here rather than from wherever this was called.
root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." >/dev/null && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# A whole number, and nothing else: no sign, no space, no second line. How large is the comparisons'
# business: every number the runner answers is held to at least one, and a number too wide for `[`
# fails that (`tools/wasm_accuracy.test.mjs`). One that fits and is merely huge reaches the renderer.
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
  if ! expected=$(node "$runner" --detectors) || ! count "$expected" || ! [ "$expected" -ge 1 ]; then
    echo "$dir: --detectors did not answer with a count of at least one: '${expected:-}'" >&2
    status=1
    continue
  fi
  ring=$(node "$runner" --ring) || ring=
  read -r frames width height panorama extra <<<"$ring"
  if [ -n "${extra:-}" ] || ! count "${frames:-}" || ! count "${width:-}" || ! count "${height:-}" \
      || ! [ "$frames" -ge 1 ] || ! [ "$width" -ge 1 ] || ! [ "$height" -ge 1 ] \
      || [ ! -f "$root/${panorama:-}" ] || ! [ "$(printf '%s' "$ring" | wc -l)" -eq 0 ]; then
    echo "$dir: --ring did not answer with frames, width, height and a panorama: '$ring'" >&2
    status=1
    continue
  fi
  rendered="$scratch/ring-$frames-$width-$height-$(printf '%s' "$panorama" | tr '/' '_')"
  if [ ! -d "$rendered" ] && ! uv run --locked --project "$root" --group datasets \
      "$root/tools/synth_dataset.py" \
      --out "$rendered" --frames "$frames" --width "$width" --height "$height" \
      --panorama "$root/$panorama" >"$scratch/render.log" 2>&1; then
    cat "$scratch/render.log"
    echo "$dir: the ring did not render, so nothing was measured" >&2
    status=1
    continue
  fi
  node "$runner" "$rendered" >"$scratch/run.log" 2>&1
  run=$?
  sed "s|^|$dir: |" "$scratch/run.log"
  measured=$(grep -c '^\[wasm-solved\]' "$scratch/run.log" || true)
  if ! [ "$run" -eq 0 ]; then
    echo "$dir: the measurement failed (exit $run)" >&2
    status=1
  fi
  if ! [ "$measured" -eq "$expected" ]; then
    echo "$dir: measured $measured detectors and the binary has $expected" >&2
    status=1
  fi
done
exit $status
