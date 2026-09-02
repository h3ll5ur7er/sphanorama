#!/usr/bin/env python3
"""Fail the build if browser assumptions have leaked into the core.

The core compiles natively with zero Emscripten symbols. That is not tidiness: it is what makes
managers testable against fakes on a desktop, and it is the property that stops the WASM build and
the native build from quietly diverging in behaviour.

bridge/ is the one tree allowed to know Emscripten exists.

Usage:  python3 tools/no_browser_check.py [repo_root]
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

# Scanned trees. bridge/ is deliberately absent.
SCANNED_ROOTS = ("core/src", "core/test", "contracts/cpp", "bench")
SUFFIXES = (".h", ".hpp", ".cpp", ".cc")

# Deliberately blunt. The bare word in a comment counts too: "on Emscripten we do X" means the
# core is reasoning about the platform, which is the thing this prevents.
PATTERNS = (
    (re.compile(r"emscripten", re.IGNORECASE), "references Emscripten"),
    (re.compile(r"\bEM_ASM\b|\bEM_JS\b|\bMAIN_THREAD_EM_ASM\b"), "embeds inline JavaScript"),
    (re.compile(r"\b__wasm\w*__\b"), "branches on a WebAssembly build macro"),
)


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    reason: str
    text: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.reason} — {self.text.strip()}"


def check(root: Path) -> list[Violation]:
    root = Path(root)
    violations: list[Violation] = []

    for scanned in SCANNED_ROOTS:
        base = root / scanned
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            for number, text in enumerate(path.read_text(errors="replace").splitlines(), start=1):
                for pattern, reason in PATTERNS:
                    if pattern.search(text):
                        violations.append(Violation(rel, number, reason, text))
                        break
    return violations


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    violations = check(root)
    if not violations:
        return 0
    print(f"{len(violations)} browser assumption(s) in the core:\n", file=sys.stderr)
    for violation in violations:
        print(f"  {violation}", file=sys.stderr)
    print("\nOnly bridge/ may reference Emscripten. See docs/03-architecture.md §3.5.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
