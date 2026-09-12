#!/usr/bin/env bash
#
# The local mirror of .github/workflows/ci.yml — every check, in the order CI runs it.
#
# It exists because the checks are spread across four CI jobs and running a remembered subset by
# hand is how a red build reaches the branch: the checkers have their own test suites, and those
# are the easiest thing to forget precisely because they never change while you are working.
#
# If you add a step to ci.yml, add it here. If you add one here, add it to ci.yml. A step that
# only exists in one of them is a step that is not really enforced.
#
# Usage: tools/gate.sh
set -u
cd "$(dirname "$0")/.."

# Same default location tools/setup_emsdk.sh installs into, overridable the same way.
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
if ! command -v emcc >/dev/null 2>&1 && [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
  # shellcheck disable=SC1091
  source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
fi
have_emcc=$(command -v emcc >/dev/null 2>&1 && echo yes || echo no)

# The Python tooling runs through uv so the interpreter and any dependency come from pyproject.toml
# and uv.lock rather than from whatever the machine happens to have. Without it the checkers below
# would each fail one at a time with a confusing message; say it once, here.
if ! command -v uv >/dev/null 2>&1; then
  echo "FAIL  uv is not on PATH — the tooling runs through it (https://docs.astral.sh/uv/)."
  echo "      Install it, then re-run: curl -LsSf https://astral.sh/uv/install.sh | sh"
  exit 1
fi

fail=0
step() {
  local name="$1"; shift
  if out=$("$@" 2>&1); then
    printf 'PASS  %s\n' "$name"
  else
    printf 'FAIL  %s\n' "$name"
    printf '%s\n' "$out" | tail -30
    fail=1
  fi
}

echo "== contracts and architecture rules =="
step "layer checker tests"        uv run --locked tools/test_layer_check.py
step "layer rules"                uv run --locked tools/layer_check.py
step "contract generator tests"   uv run --locked tools/test_contract_gen.py
step "codec generator tests"      uv run --locked tools/test_codec_gen.py
step "generated files not stale"  uv run --locked tools/contract_gen.py --check
step "size budget checker tests"  uv run --locked tools/test_size_budget.py
step "no-browser checker tests"   uv run --locked tools/test_no_browser_check.py
step "no browser assumptions"     uv run --locked tools/no_browser_check.py
step "marker checker tests"       uv run --locked tools/test_conflict_marker_check.py
step "no conflict markers"        uv run --locked tools/conflict_marker_check.py
step "table checker tests"        uv run --locked tools/test_markdown_table_check.py
step "no broken tables"           uv run --locked tools/markdown_table_check.py
# The one step that needs a dependency, so it names the group that carries it. Everything
# above is standard-library only and stays that way (ADR 0048, ADR 0050).
step "dataset renderer tests"     uv run --locked --group datasets tools/test_synth_dataset.py

echo "== native =="
step "native configure"  cmake --preset native-debug
step "native build"      cmake --build build/native-debug
step "native test"       ctest --test-dir build/native-debug --output-on-failure
# Mirrors CI's step of the same name. `ctest` reports a skipped test as a pass, so the line above is
# green whether the accuracy measurement ran or not — and it needs `uv` and the `datasets` group to
# run at all. It counts the measurement's own output lines rather than grepping for SKIPPED, because
# the first version was green on a filter that matched nothing: zero tests, no SKIPPED, exit 0, so a
# renamed suite or a build without OpenCV reported the number as taken.
step "accuracy measured"  sh -c 'log=$(mktemp); expected=$(./build/native-debug/bin/sphanorama_tests --gtest_list_tests --gtest_filter="EveryDetector/Accuracy.ConsecutiveFrames*" | grep -c ConsecutiveFrames); ./build/native-debug/bin/sphanorama_tests --gtest_filter="EveryDetector/Accuracy.*:Acceptance.*" >"$log" 2>&1; status=$?; cat "$log"; measured=$(grep -c "^\\[accuracy\\]" "$log" || true); if [ "$expected" -lt 1 ]; then echo "the binary contains no accuracy measurement to run" >&2; status=1; fi; if [ "$measured" -ne "$expected" ]; then echo "the measurement ran $measured detectors and the binary has $expected" >&2; status=1; fi; if grep -q SKIPPED "$log"; then echo "something skipped, and a skip is not a pass" >&2; status=1; fi; rm -f "$log"; exit $status'

echo "== sanitizers =="
step "asan configure"    cmake --preset native-asan
step "asan build"        cmake --build build/native-asan
step "asan test"         ctest --test-dir build/native-asan --output-on-failure
# CI's sanitizer job has this step too, and `tools/gate.sh` mirrors CI step for step — it was
# mirroring only the native one, which is the drift its own header exists to prevent.
step "asan accuracy"      sh -c 'log=$(mktemp); expected=$(./build/native-asan/bin/sphanorama_tests --gtest_list_tests --gtest_filter="EveryDetector/Accuracy.ConsecutiveFrames*" | grep -c ConsecutiveFrames); ./build/native-asan/bin/sphanorama_tests --gtest_filter="EveryDetector/Accuracy.*:Acceptance.*" >"$log" 2>&1; status=$?; cat "$log"; measured=$(grep -c "^\\[accuracy\\]" "$log" || true); if [ "$expected" -lt 1 ]; then echo "the binary contains no accuracy measurement to run" >&2; status=1; fi; if [ "$measured" -ne "$expected" ]; then echo "the measurement ran $measured detectors and the binary has $expected" >&2; status=1; fi; if grep -q SKIPPED "$log"; then echo "something skipped, and a skip is not a pass" >&2; status=1; fi; rm -f "$log"; exit $status'

echo "== wasm, size budget and browser tests =="
if [ "$have_emcc" = no ]; then
  # Loudly, not silently: a skipped half of the gate that reads as a pass is worse than no gate.
  echo "SKIP  wasm builds and browser tests — no emcc on PATH (run tools/setup_emsdk.sh)"
  fail=1
else
  step "wasm build"          bash -c "cmake --preset wasm-release && cmake --build build/wasm-release"
  step "wasm threaded build" bash -c "cmake --preset wasm-release-threaded && cmake --build build/wasm-release-threaded"
  step "size budget"         bash -c "uv run --locked tools/size_budget.py --profile wasm-release --build-dir build/wasm-release/bridge && uv run --locked tools/size_budget.py --profile wasm-release-threaded --build-dir build/wasm-release-threaded/bridge"
  step "shell unit tests"    npm test
  step "build the shell"     npm run build
  step "browser tests"       npx playwright test
fi

echo "----"
if [ $fail -eq 0 ]; then echo "GATE GREEN"; else echo "GATE RED"; fi
exit $fail
