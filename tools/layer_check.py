#!/usr/bin/env python3
"""Fail the build on a call edge the architecture forbids.

The layer rules in docs/03-architecture.md §3.3 are only real if something checks them, so this
walks the include graph and rejects any edge the matrix below disallows. It works at contract
granularity, which is why contracts are one interface per header: at aggregate-header granularity
a manager calling another manager is indistinguishable from a manager implementing its own
interface.

Usage:  python3 tools/layer_check.py [repo_root]
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

CONTRACT_ROOT = Path("contracts/cpp")
CONTRACT_PACKAGE = "sphanorama"

# Quoted includes are resolved against the includer's directory first, then these, mirroring the
# include path the build actually uses. Root-relative resolution is what makes an edge like an
# engine including "managers/capture_session_manager.h" visible: without it the include simply
# would not resolve, and the check would report the wrong problem.
INCLUDE_ROOTS = (CONTRACT_ROOT, Path("core/src"))

# Directory name under contracts/cpp/sphanorama/ and core/src/ -> layer.
LAYER_DIRS = {
    "utilities": "utilities",
    "engines": "engine",
    "managers": "manager",
    "resource_access": "resource_access",
}

# Source roots that are not layered by directory: everything under them is one layer.
FLAT_ROOTS = {
    "bench": "client",
    "bridge": "client",
    "shell/src/clients": "client",
}

# Test trees are not layer-checked: a test may reach for whatever it needs to set up a scenario.
EXCLUDED_PREFIXES = ("bridge/test",)

SOURCE_ROOTS = ("core/src", "contracts/cpp", "bench", "bridge", "shell/src")
SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc")

# Which layers each layer may depend on. Same-layer edges are handled separately: they are legal
# only when a component depends on itself.
ALLOWED = {
    "client":          {"manager", "shared", "utilities"},
    "manager":         {"engine", "resource_access", "shared", "utilities"},
    "engine":          {"shared", "utilities"},
    "resource_access": {"shared", "utilities"},
    "utilities":       {"shared", "utilities"},
    "shared":          {"shared"},
}

# The one sanctioned exception (docs/03 §3.3 rule 5): compute placement and pixel residency are
# properties of the device, not of the algorithm, so threading them through every engine
# signature would invert the dependency for no gain.
ENGINE_RESOURCE_EXCEPTIONS = {"compute_device_access", "frame_store_access"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


@dataclass(frozen=True)
class Unit:
    """A file's position in the architecture: which layer it belongs to, and which component."""
    layer: str
    component: str | None

    def __str__(self) -> str:
        return f"{self.layer}:{self.component}" if self.component else self.layer


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    include: str
    source: Unit
    target: Unit | None
    reason: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.reason} — #include \"{self.include}\""


def classify(rel: Path) -> Unit | None:
    """Where does this file sit in the architecture? None means 'not layered source'."""
    parts = rel.parts
    posix = rel.as_posix()

    if any(posix.startswith(prefix + "/") for prefix in EXCLUDED_PREFIXES):
        return None

    for root, layer in FLAT_ROOTS.items():
        if posix.startswith(root + "/"):
            return Unit(layer, None)

    tail: tuple[str, ...] | None = None
    if parts[:2] == ("core", "src"):
        tail = parts[2:]
    elif parts[:3] == ("contracts", "cpp", CONTRACT_PACKAGE):
        tail = parts[3:]
    if tail is None:
        return None

    if len(tail) == 1:                      # sphanorama/types.h — pure data, no layer
        return Unit("shared", None)

    layer = LAYER_DIRS.get(tail[0])
    if layer is None:
        return None
    # Component is the next path segment with any extension stripped, so that
    # engines/pose_engine.cpp and engines/pose_engine/detail.h are the same component.
    return Unit(layer, Path(tail[1]).stem if len(tail) == 2 else tail[1])


def resolve(include: str, includer: Path, root: Path) -> Path | None:
    """Resolve an include to a repo-relative path, quoted-include rules: local dir, then roots."""
    local = (includer.parent / include).resolve()
    try:
        rel = local.relative_to(root.resolve())
    except ValueError:
        rel = None
    if rel is not None and (root / rel).is_file():
        return rel
    for include_root in INCLUDE_ROOTS:
        candidate = include_root / include
        if (root / candidate).is_file():
            return candidate
    return None


def check(root: Path) -> list[Violation]:
    root = Path(root)
    violations: list[Violation] = []

    for source_root in SOURCE_ROOTS:
        base = root / source_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root)
            unit = classify(rel)
            if unit is None:
                continue
            text = path.read_text(errors="replace")
            for m in INCLUDE_RE.finditer(text):
                include = m.group(1)
                line = text.count("\n", 0, m.start()) + 1
                target_path = resolve(include, path, root)
                if target_path is None:
                    violations.append(Violation(
                        rel.as_posix(), line, include, unit, None,
                        "include does not resolve to a file in this repository"))
                    continue
                target = classify(target_path)
                if target is None:
                    continue
                v = judge(unit, target, rel, line, include)
                if v:
                    violations.append(v)
    return violations


def judge(source: Unit, target: Unit, rel: Path, line: int, include: str) -> Violation | None:
    if source.layer == target.layer:
        # A component may depend on itself — its own contract, its own private headers — and on
        # nothing else in its layer. This is what forbids manager-to-manager coupling.
        if source.component is None or source.component == target.component:
            return None
        if source.layer == "utilities":
            return None   # the utilities bar is a bar, not a sequence; internal reuse is fine
        return Violation(rel.as_posix(), line, include, source, target,
                         f"{source.layer} '{source.component}' may not depend on "
                         f"'{target.component}' in the same layer")

    if source.layer == "engine" and target.layer == "resource_access":
        if target.component in ENGINE_RESOURCE_EXCEPTIONS:
            return None
        return Violation(rel.as_posix(), line, include, source, target,
                         f"engines may use only {sorted(ENGINE_RESOURCE_EXCEPTIONS)}, "
                         f"not '{target.component}'")

    if target.layer in ALLOWED[source.layer]:
        return None

    return Violation(rel.as_posix(), line, include, source, target,
                     f"{source.layer} may not depend on {target.layer}")


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    violations = check(root)
    if not violations:
        return 0
    print(f"{len(violations)} layer violation(s):\n", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    print("\nSee docs/03-architecture.md §3.3. An exception is an ADR, not a comment.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
