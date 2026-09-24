#!/usr/bin/env python3
"""Refuse a native-contracting build that is not contracting.

That build exists to show rounding that differs between compiled copies of one expression, which the
-O0 builds cannot. A build that lost `-ffp-contract=fast` still compiles and still passes every
test, so it would read as the job having looked. This reads the compile database and requires the
flag on every compile, taking the last `-ffp-contract=` on each line because that is the one the
compiler obeys: a round-12 reviewer appended `-ffp-contract=off` and the first version of this check,
which only asked whether `fast` appeared, passed it.

It does not count fused instructions, which it once did. `Norm` calls `std::fma` now, so the object
holds fused instructions whatever the contraction mode, and the count could no longer fail.

Usage:  uv run tools/fused_build_check.py <build dir>
"""
from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path

FLAG = "-ffp-contract="
# The translation unit the defect lived in. Required to be present, so that an empty or unrelated
# database cannot pass by having nothing wrong in it.
NORM_SOURCE = "core/src/utilities/quaternion.cpp"


def _arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def _contraction(arguments: list[str]) -> str | None:
    last = None
    for argument in arguments:
        if argument.startswith(FLAG):
            last = argument[len(FLAG):]
    return last


def problems(build: Path) -> list[str]:
    database = build / "compile_commands.json"
    if not database.is_file():
        return [f"no {database}: configure the build first"]
    try:
        entries = json.loads(database.read_text())
    except (OSError, ValueError) as error:
        return [f"{database} could not be read: {error}"]

    found = []
    saw_norm = False
    for entry in entries:
        source = str(entry.get("file", ""))
        if source.replace("\\", "/").endswith(NORM_SOURCE):
            saw_norm = True
        mode = _contraction(_arguments(entry))
        if mode != "fast":
            said = f"-ffp-contract={mode}" if mode else "no -ffp-contract"
            found.append(f"{source} is compiled with {said}, so it rounds like the -O0 builds")
    if not saw_norm:
        found.append(f"{NORM_SOURCE} is not in {database}, so there is nothing to check")
    return found


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    found = problems(Path(argv[0]))
    for problem in found:
        print(problem, file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
