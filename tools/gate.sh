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
step "layer checker tests"        python3 tools/test_layer_check.py
step "layer rules"                python3 tools/layer_check.py
step "contract generator tests"   python3 tools/test_contract_gen.py
step "codec generator tests"      python3 tools/test_codec_gen.py
step "generated files not stale"  python3 tools/contract_gen.py --check
step "size budget checker tests"  python3 tools/test_size_budget.py
step "no-browser checker tests"   python3 tools/test_no_browser_check.py
step "no browser assumptions"     python3 tools/no_browser_check.py

echo "== native =="
step "native configure"  cmake --preset native-debug
step "native build"      cmake --build build/native-debug
step "native test"       ctest --test-dir build/native-debug --output-on-failure

echo "== sanitizers =="
step "asan configure"    cmake --preset native-asan
step "asan build"        cmake --build build/native-asan
step "asan test"         ctest --test-dir build/native-asan --output-on-failure

echo "== wasm, size budget and browser tests =="
if [ "$have_emcc" = no ]; then
  # Loudly, not silently: a skipped half of the gate that reads as a pass is worse than no gate.
  echo "SKIP  wasm builds and browser tests — no emcc on PATH (run tools/setup_emsdk.sh)"
  fail=1
else
  step "wasm build"          bash -c "cmake --preset wasm-release && cmake --build build/wasm-release"
  step "wasm threaded build" bash -c "cmake --preset wasm-release-threaded && cmake --build build/wasm-release-threaded"
  step "size budget"         bash -c "python3 tools/size_budget.py --profile wasm-release --build-dir build/wasm-release/bridge && python3 tools/size_budget.py --profile wasm-release-threaded --build-dir build/wasm-release-threaded/bridge"
  step "shell unit tests"    npm test
  step "build the shell"     npm run build
  step "browser tests"       npx playwright test
fi

echo "----"
if [ $fail -eq 0 ]; then echo "GATE GREEN"; else echo "GATE RED"; fi
exit $fail
