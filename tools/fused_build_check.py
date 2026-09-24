#!/usr/bin/env python3
"""Refuse a native-contracting build that is not contracting.

That build exists to show rounding that differs between compiled copies of one expression, which the
-O0 builds cannot. A build that stopped contracting still compiles and still passes every test, so it
would read as the job having looked. Two questions, because neither answers the other:

- **Is fast contraction what every compile asked for?** Read from the compile database, taking the
  last flag that sets the mode — `-ffp-contract=` or `-ffp-model=` — since that is the one clang
  obeys. Round 12 appended `-ffp-contract=off` to pass a grep that only asked whether `fast`
  appeared; round 13 found `-ffp-model=precise` after it doing the same.
- **Did the compiler actually fuse?** Counted in the averager's object, which calls no `std::fma` of
  its own, so every fused instruction in it is contraction. This catches what flags cannot: a build
  without the FMA instruction set, where `std::fma` is a library call, and `-frounding-math`, which
  turns contraction off beside a `-ffp-contract=fast`. Measured with clang 18: 26 fused with the
  preset, 0 without `-march=x86-64-v3`, 0 with `-frounding-math`. It cannot tell `fast` from clang's
  default (30 with that), which is why the first question stays.

Usage:  uv run tools/fused_build_check.py <build dir>
"""
from __future__ import annotations

import json
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Callable

# The translation unit the defect lived in. Required to be present, so that an empty or unrelated
# database cannot pass by having nothing wrong in it.
NORM_SOURCE = "core/src/utilities/quaternion.cpp"
# The object whose fused instructions can only have come from contraction.
MEASURED_SOURCE = "core/src/utilities/quaternion_average.cpp"
# x86 (vfmadd231sd, vfnmsub132pd, ...) and AArch64 (fmadd, fmsub, fmla, fmls).
FUSED = re.compile(r"\b(vfn?m(add|sub)[0-9]*[sp][sd]|fn?m(add|sub)|fml[as])\b")
# What each `-ffp-model=` sets contraction to, on clang 18.
MODEL_CONTRACTION = {"fast": "fast", "precise": "on", "strict": "off"}


def _arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def _contraction(arguments: list[str]) -> str | None:
    last = None
    for argument in arguments:
        if argument.startswith("-ffp-contract="):
            last = argument[len("-ffp-contract="):]
        elif argument.startswith("-ffp-model="):
            last = MODEL_CONTRACTION.get(argument[len("-ffp-model="):], argument)
    return last


def fused_instructions(disassembly: str) -> int:
    return sum(1 for line in disassembly.splitlines() if FUSED.search(line))


def objdump(path: Path) -> str:
    return subprocess.run(["objdump", "-d", str(path)], check=True, capture_output=True,
                          text=True).stdout


def _is(entry: dict, source: str) -> bool:
    return str(entry.get("file", "")).replace("\\", "/").endswith(source)


def problems(build: Path, disassemble: Callable[[Path], str] = objdump) -> list[str]:
    database = build / "compile_commands.json"
    if not database.is_file():
        return [f"no {database}: configure the build first"]
    try:
        entries = json.loads(database.read_text())
    except (OSError, ValueError) as error:
        return [f"{database} could not be read: {error}"]

    found = []
    for entry in entries:
        mode = _contraction(_arguments(entry))
        if mode != "fast":
            said = f"contraction '{mode}'" if mode else "no -ffp-contract"
            found.append(f"{entry.get('file')} is compiled with {said}, so it rounds like the -O0 "
                         "builds")
    if not any(_is(entry, NORM_SOURCE) for entry in entries):
        found.append(f"{NORM_SOURCE} is not in {database}, so there is nothing to check")

    measured = [entry for entry in entries if _is(entry, MEASURED_SOURCE)]
    if not measured:
        found.append(f"{MEASURED_SOURCE} is not in {database}, so fusion cannot be measured")
    else:
        entry = measured[0]
        output = Path(entry.get("directory", build)) / entry.get("output", "")
        try:
            count = fused_instructions(disassemble(output))
        except (OSError, subprocess.CalledProcessError) as error:
            found.append(f"could not disassemble {output}: {error}")
        else:
            if count == 0:
                found.append(f"{output} holds no fused instructions: this build asked for "
                             "contraction and the compiler did not do it (no FMA instruction set, "
                             "or a flag such as -frounding-math)")
    return found


def main(argv: list[str], disassemble: Callable[[Path], str] = objdump) -> int:
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    found = problems(Path(argv[0]), disassemble)
    for problem in found:
        print(problem, file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
