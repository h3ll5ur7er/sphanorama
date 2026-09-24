#!/usr/bin/env bash
#
# Refuses a native-contracting build that could not see what it exists to see: rounding that
# differs between compiled copies of one expression. That needs `-ffp-contract=fast` on the compile
# and a host that fuses multiply-adds, and each is checked on its own so the message says which one
# is missing. Shared by CI and tools/gate.sh so the two cannot drift.
#
# Usage: tools/check_fused_build.sh <build dir>
set -u
build="${1:?usage: tools/check_fused_build.sh <build dir>}"
commands="$build/compile_commands.json"
object="$build/core/CMakeFiles/sphanorama_core.dir/src/utilities/quaternion.cpp.o"

if [ ! -f "$commands" ]; then
  echo "no $commands — configure the build first" >&2; exit 1
fi
if ! grep -E '"command".*-ffp-contract=fast.*src/utilities/quaternion\.cpp' "$commands" >/dev/null; then
  echo "quaternion.cpp was not compiled with -ffp-contract=fast, so this build rounds like the -O0 ones" >&2
  exit 1
fi
if [ ! -f "$object" ]; then
  echo "no $object — build first" >&2; exit 1
fi
if ! command -v objdump >/dev/null 2>&1; then
  echo "objdump is not on PATH, so the fused instructions cannot be counted" >&2; exit 1
fi
fused=$(objdump -d "$object" | grep -c -E 'vfn?m(add|sub)|fmadd|fmsub|fmla|fmls')
echo "fused multiply-adds in quaternion.cpp: $fused"
if [ "$fused" -lt 1 ]; then
  echo "the flag reached the compile but nothing fused, so this host cannot show the defect" >&2
  exit 1
fi
