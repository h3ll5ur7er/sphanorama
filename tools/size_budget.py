#!/usr/bin/env python3
"""Fail the build when a shipped artifact outgrows its budget.

Sizes are measured gzipped: that is what crosses a mobile network, and it is the number that
decides whether someone waits for the page or closes the tab. Budgets live in size_budgets.toml.

Usage:
  python3 tools/size_budget.py --profile wasm-release --build-dir build/wasm-release/bridge
"""
from __future__ import annotations

import argparse
import gzip
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

BUDGET_FILE = Path("tools/size_budgets.toml")


@dataclass(frozen=True)
class Measurement:
    name: str
    measured: int
    budget: int
    missing: bool = False

    @property
    def within_budget(self) -> bool:
        return not self.missing and self.measured <= self.budget

    def __str__(self) -> str:
        if self.missing:
            return f"{self.name}: MISSING (budget {self.budget:,} B)"
        headroom = self.budget - self.measured
        verdict = "ok" if self.within_budget else "OVER"
        return (f"{self.name}: {self.measured:,} B gz / {self.budget:,} B "
                f"({headroom:+,} B) {verdict}")


def compressed_size(path: Path) -> int:
    # mtime=0 so the measurement is reproducible; otherwise the timestamp in the gzip header
    # makes the number wobble between runs.
    return len(gzip.compress(path.read_bytes(), mtime=0))


def load_budgets(root: Path) -> dict[str, dict[str, int]]:
    return tomllib.loads((root / BUDGET_FILE).read_text())


def check(build_dir: Path, budgets: dict[str, int]) -> list[Measurement]:
    results = []
    for name, budget in sorted(budgets.items()):
        path = build_dir / name
        if not path.is_file():
            # A missing artifact is a failure, never a pass: the budget would otherwise look
            # green exactly when the build produced nothing.
            results.append(Measurement(name, 0, budget, missing=True))
        else:
            results.append(Measurement(name, compressed_size(path), budget))
    return results


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--profile", required=True, help="a section name in size_budgets.toml")
    ap.add_argument("--build-dir", required=True, help="directory holding the built artifacts")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args(argv[1:])

    budgets = load_budgets(Path(args.root))
    if args.profile not in budgets:
        print(f"unknown profile {args.profile!r}; known: {sorted(budgets)}", file=sys.stderr)
        return 2

    build_dir = Path(args.build_dir)
    if not build_dir.is_dir():
        print(f"build directory {build_dir} does not exist", file=sys.stderr)
        return 2

    results = check(build_dir, budgets[args.profile])
    print(f"size budget · {args.profile}")
    for result in results:
        print(f"  {result}")

    failures = [r for r in results if not r.within_budget]
    if failures:
        print(f"\n{len(failures)} artifact(s) outside budget", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
