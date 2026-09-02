#!/usr/bin/env python3
"""Generate contracts/ts/contracts.d.ts from the C++ contract headers.

The C++ header is the IDL (ADR 0009). To make that safe, this parser accepts only a small,
declared subset of C++ and raises on anything else. Strictness is the feature: a lenient parser
would silently drop a construct it did not understand, and the mirror would drift exactly where
nobody was looking.

Usage:
  python3 tools/contract_gen.py            # rewrite the mirror
  python3 tools/contract_gen.py --check    # fail if the committed mirror is stale
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

BOUNDARY_MARKER = "@boundary"

# The scalar subset. Anything else is a hard error rather than a guess.
SCALARS = {
    "bool": "boolean",
    "double": "number",
    "float": "number",
    "int8_t": "number", "int16_t": "number", "int32_t": "number",
    "uint8_t": "number", "uint16_t": "number", "uint32_t": "number",
    # int64_t is a count or a timestamp in these contracts and stays a JS number; uint64_t is
    # only ever a hash, where the low 11 bits a double drops are the ones that matter.
    "int64_t": "number",
    "uint64_t": "bigint",
    "std::string": "string",
    "std::string_view": "string",
    "void": "void",
}

HEADERS_IN_ORDER = (
    "types.h",
    "managers/capture_session_manager.h",
    "managers/panorama_build_manager.h",
    "managers/project_manager.h",
    "resource_access/camera_access.h",
    "resource_access/motion_sensor_access.h",
    "resource_access/project_store_access.h",
    "resource_access/image_codec_access.h",
    "resource_access/export_access.h",
)


class ContractSyntaxError(Exception):
    """The header contains something outside the subset the mirror can represent."""


@dataclass
class IdAlias:
    name: str
    doc: list[str] = field(default_factory=list)


@dataclass
class Enum:
    name: str
    members: list[str]
    doc: list[str] = field(default_factory=list)


@dataclass
class Field:
    name: str
    type: str
    doc: list[str] = field(default_factory=list)


@dataclass
class Struct:
    name: str
    fields: list[Field]
    doc: list[str] = field(default_factory=list)


@dataclass
class Method:
    name: str
    params: list[tuple[str, str]]
    returns: str
    doc: list[str] = field(default_factory=list)


@dataclass
class Interface:
    name: str
    methods: list[Method]
    doc: list[str] = field(default_factory=list)


@dataclass
class Module:
    declarations: list[object] = field(default_factory=list)


# ---------------------------------------------------------------------------- type mapping

def map_type(cpp: str, context: str, as_return: bool = False) -> str:
    """C++ type -> TypeScript type. Raises on anything outside the subset.

    `as_return` distinguishes the two meanings of Status: a method returning Status is reporting
    success or failure, which is Result<void> on the wire, while a *field* of type Status is a
    status value carried inside a message. Conflating them silently drops the field.
    """
    t = cpp.strip()
    t = re.sub(r"\bconst\b", "", t).strip()
    t = t.rstrip("&").strip()

    if "*" in t:
        raise ContractSyntaxError(f"{context}: pointers are not part of the contract subset: {cpp!r}")

    m = re.fullmatch(r"(?:std::)?(?:vector|span)<(.+)>", t)
    if m:
        inner = m.group(1).strip()
        # A byte sequence is a binary payload on both sides. Mirroring it as number[] would box
        # one JS value per byte for something that is already a buffer.
        if inner == "uint8_t":
            return "Uint8Array"
        return map_type(inner, context) + "[]"

    m = re.fullmatch(r"Result<(.+)>", t)
    if m:
        return f"Result<{map_type(m.group(1), context)}>"

    if t == "Status":
        return "Result<void>" if as_return else "Status"

    if t in SCALARS:
        return SCALARS[t]

    # A bare identifier is a type declared elsewhere in the contracts; the TypeScript compiler
    # catches a name that does not resolve, so we do not need a symbol table here.
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", t):
        return t

    raise ContractSyntaxError(f"{context}: cannot map type {cpp!r} into the mirror")


# ---------------------------------------------------------------------------------- parsing

DOC_RE = re.compile(r"^\s*//\s?(.*)$")
ID_RE = re.compile(r"^\s*using\s+(\w+)\s*=\s*Id<\w+>\s*;")
ENUM_RE = re.compile(r"^\s*enum\s+class\s+(\w+)\s*(?::\s*\w+\s*)?\{")
STRUCT_RE = re.compile(r"^\s*struct\s+(\w+)\s*\{")
CLASS_RE = re.compile(r"^\s*class\s+(\w+)\s*\{")
METHOD_RE = re.compile(r"^\s*virtual\s+(.+?)\s+(\w+)\s*\((.*?)\)\s*(?:const\s*)?=\s*0\s*;")

# Lines inside a declaration body that carry no contract meaning.
SKIPPABLE = re.compile(
    r"^\s*(public:|private:|protected:|virtual\s+~\w+\(\)\s*=\s*default;|//|$|\})")


def _collect_doc(pending: list[str]) -> list[str]:
    doc = [line for line in pending if BOUNDARY_MARKER not in line]
    return [d for d in doc if d.strip()]


def parse(text: str) -> Module:
    module = Module()
    lines = text.splitlines()
    i = 0
    pending: list[str] = []
    templated = False

    while i < len(lines):
        line = lines[i]

        if line.strip().startswith("#") or "namespace" in line:
            i += 1
            continue

        doc_match = DOC_RE.match(line)
        if doc_match:
            pending.append(doc_match.group(1))
            i += 1
            continue

        if not line.strip():
            pending.clear()
            i += 1
            continue

        m = ID_RE.match(line)
        if m:
            module.declarations.append(IdAlias(m.group(1), _collect_doc(pending)))
            pending, i = [], i + 1
            continue

        m = ENUM_RE.match(line)
        if m:
            body, i = _read_body(lines, i)
            module.declarations.append(Enum(m.group(1), _parse_enum_members(body, m.group(1)),
                                            _collect_doc(pending)))
            pending = []
            continue

        m = STRUCT_RE.match(line)
        if m:
            body, i = _read_body(lines, i)
            if templated:
                pending, templated = [], False
                continue
            struct = Struct(m.group(1), _parse_fields(body, m.group(1)), _collect_doc(pending))
            if struct.fields:
                module.declarations.append(struct)
            pending = []
            continue

        m = CLASS_RE.match(line)
        if m:
            body, i = _read_body(lines, i)
            is_boundary = any(BOUNDARY_MARKER in d for d in pending)
            if is_boundary:
                name = m.group(1)
                module.declarations.append(
                    Interface(name, _parse_methods(body, name), _collect_doc(pending)))
            pending = []
            continue

        # Templates, macros, free functions: not part of the mirror, and not an error either —
        # they are C++ implementation detail that never crosses the boundary.
        templated = line.lstrip().startswith("template")
        pending = []
        i += 1

    return module


def _read_body(lines: list[str], start: int) -> tuple[str, int]:
    """Return the text between the braces opening on `start`, and the index just past them."""
    depth = 0
    body: list[str] = []
    i = start
    while i < len(lines):
        line = lines[i]
        opened = line.count("{")
        closed = line.count("}")
        if depth == 0:
            body.append(line[line.index("{") + 1:] if "{" in line else "")
        else:
            body.append(line)
        depth += opened - closed
        i += 1
        if depth <= 0:
            break
    text = "\n".join(body)
    tail = text.rfind("}")
    return (text[:tail] if tail >= 0 else text), i


def _parse_enum_members(body: str, name: str) -> list[str]:
    members = []
    for raw in body.replace("\n", " ").split(","):
        item = re.sub(r"//.*", "", raw).strip()
        if not item:
            continue
        m = re.fullmatch(r"(\w+)(?:\s*=\s*[^,]+)?", item)
        if not m:
            raise ContractSyntaxError(f"enum {name}: cannot parse member {item!r}")
        members.append(m.group(1))
    return members


FUNCTION_MEMBER_RE = re.compile(r"^[\w:<>,\s*&~]*\w\s*\(.*\)")


def _split_type_and_declarators(decl: str) -> tuple[str, str] | None:
    """Split "std::span<const Candidate> siblings" into its type and its declarator list.

    A regex cannot do this: the type may itself contain spaces inside angle brackets, and a
    lazy match would cut "std::span<const" off as the type. The boundary is the first space at
    bracket depth zero.
    """
    depth = 0
    for index, ch in enumerate(decl):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch.isspace() and depth == 0:
            cpp_type, declarators = decl[:index].strip(), decl[index + 1:].strip()
            if not cpp_type or not declarators:
                return None
            return cpp_type, declarators
    return None


def _parse_fields(body: str, name: str) -> list[Field]:
    fields: list[Field] = []
    pending: list[str] = []

    for raw in body.splitlines():
        line = raw.strip()
        if not line:
            pending.clear()
            continue
        doc = DOC_RE.match(raw)
        if doc:
            pending.append(doc.group(1))
            continue
        if line in ("public:", "private:", "protected:"):
            continue

        # A value type may carry a small helper (Status::ok(), Id::valid()). Those are C++
        # convenience with no data to mirror, and they are recognised by their parentheses —
        # a data member never has any. Anything else still raises.
        if FUNCTION_MEMBER_RE.match(line):
            continue

        # A trailing comment documents the field it sits on; strip it before checking the
        # statement terminator, or a documented field looks unterminated.
        code, _, trailing = line.partition("//")
        code = code.strip()
        if trailing.strip():
            pending = pending + [trailing.strip()]

        if not code.endswith(";"):
            raise ContractSyntaxError(f"struct {name}: cannot parse {line!r}")

        # One line may hold several declarations, as single-line value types do.
        for decl in (d.strip() for d in code.split(";")):
            if not decl:
                continue
            split = _split_type_and_declarators(decl)
            if split is None:
                raise ContractSyntaxError(f"struct {name}: cannot parse field {decl!r}")
            cpp_type, declarators = split

            for declarator in declarators.split(","):
                piece = declarator.split("=")[0].strip()
                if not re.fullmatch(r"\w+", piece):
                    raise ContractSyntaxError(
                        f"struct {name}: cannot parse declarator {piece!r}")
                fields.append(Field(piece, map_type(cpp_type, f"struct {name}.{piece}"),
                                    _collect_doc(pending)))
        pending = []
    return fields


def _parse_methods(body: str, name: str) -> list[Method]:
    methods: list[Method] = []
    pending: list[str] = []
    buffer = ""

    for raw in body.splitlines():
        stripped = raw.strip()
        doc = DOC_RE.match(raw)
        if doc and not buffer:
            pending.append(doc.group(1))
            continue
        if not stripped:
            if not buffer:
                pending.clear()
            continue
        if not buffer and SKIPPABLE.match(stripped):
            continue

        # A declaration may span lines; accumulate until the statement terminates.
        buffer = f"{buffer} {stripped}" if buffer else stripped
        if not buffer.endswith(";"):
            continue

        m = METHOD_RE.match(buffer)
        if not m:
            raise ContractSyntaxError(f"interface {name}: cannot parse member {buffer!r}")

        returns, method_name, params = m.group(1), m.group(2), m.group(3)
        methods.append(Method(
            method_name,
            _parse_params(params, f"{name}::{method_name}"),
            map_type(returns, f"{name}::{method_name} return", as_return=True),
            _collect_doc(pending)))
        pending = []
        buffer = ""

    if buffer:
        raise ContractSyntaxError(f"interface {name}: unterminated declaration {buffer!r}")
    return methods


def _parse_params(params: str, context: str) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    if not params.strip():
        return out
    for part in _split_params(params):
        piece = part.strip()
        m = re.fullmatch(r"(.+?[>\w&])\s+(\w+)", piece)
        if not m:
            raise ContractSyntaxError(
                f"{context}: parameter {piece!r} has no name — an unnamed parameter cannot be "
                f"mirrored, and a guessed name would document nothing")
        out.append((m.group(2), map_type(m.group(1), f"{context} parameter {m.group(2)}")))
    return out


def _split_params(params: str) -> list[str]:
    """Split on commas that are not inside angle brackets."""
    parts, depth, current = [], 0, ""
    for ch in params:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        parts.append(current)
    return parts


# ============================================================================== wire format
#
# Both sides of the boundary are emitted from this one parse, so they cannot disagree about field
# order or widths — the failure that would otherwise decode into plausible nonsense rather than
# failing. Little-endian throughout: every platform this runs on is.

WIRE_KINDS = {
    "bool": "bool",
    "int8_t": "i32", "int16_t": "i32", "int32_t": "i32",
    "uint8_t": "i32", "uint16_t": "i32", "uint32_t": "i32",
    "int64_t": "f64",     # counts and timestamps; stays a JS number, as the mirror documents
    "uint64_t": "u64",    # only ever a content hash, where the low bits are the point
    "double": "f64", "float": "f64",
    "std::string": "string", "std::string_view": "string",
    "void": "void",
}


@dataclass(frozen=True)
class BoundaryMethod:
    id: int
    interface: str          # TypeScript-facing name, e.g. ProjectManager
    cpp_interface: str      # e.g. IProjectManager
    method: str             # C++ name, e.g. Create
    wire_name: str          # e.g. ProjectManager.create
    params: list[tuple[str, str]]
    returns: str


def wire_kind(cpp: str, context: str) -> str:
    """Map a C++ type onto a wire kind, or raise. Named types resolve to struct/enum at emit."""
    t = re.sub(r"\bconst\b", "", cpp).strip().rstrip("&").strip()

    if "*" in t:
        raise ContractSyntaxError(f"{context}: pointers have no wire representation: {cpp!r}")

    m = re.fullmatch(r"(?:std::)?(?:vector|span)<(.+)>", t)
    if m:
        inner = re.sub(r"\bconst\b", "", m.group(1)).strip()
        return "bytes" if inner == "uint8_t" else f"list:{inner}"

    m = re.fullmatch(r"Result<(.+)>", t)
    if m:
        return wire_kind(m.group(1), context)

    if t == "Status":
        return "void"
    if t in WIRE_KINDS:
        return WIRE_KINDS[t]
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", t):
        return f"named:{t}"

    raise ContractSyntaxError(f"{context}: no wire representation for {cpp!r}")


def _index(module: Module) -> tuple[dict[str, Struct], dict[str, Enum], set[str]]:
    structs = {d.name: d for d in module.declarations if isinstance(d, Struct)}
    enums = {d.name: d for d in module.declarations if isinstance(d, Enum)}
    ids = {d.name for d in module.declarations if isinstance(d, IdAlias)}
    return structs, enums, ids


def method_table(module: Module) -> list[BoundaryMethod]:
    """Every method of every @boundary interface, in declaration order, with dense ids."""
    methods: list[BoundaryMethod] = []
    for decl in module.declarations:
        if not isinstance(decl, Interface):
            continue
        facing = decl.name[1:] if decl.name.startswith("I") else decl.name
        for method in decl.methods:
            methods.append(BoundaryMethod(
                id=len(methods), interface=facing, cpp_interface=decl.name,
                method=method.name, wire_name=f"{facing}.{lower_camel(method.name)}",
                params=list(method.params), returns=method.returns))
    return methods


# --------------------------------------------------------------------------------- emitting

PREAMBLE = """/**
 * GENERATED FILE — DO NOT EDIT.
 *
 * Produced from the C++ contract headers by tools/contract_gen.py, which is the mechanism that
 * keeps this mirror from drifting (ADR 0009). To change anything here, change the header and
 * regenerate:
 *
 *     python3 tools/contract_gen.py
 *
 * Only interfaces marked `// @boundary` in C++ appear here: engines and the utilities bar never
 * cross into JavaScript.
 *
 * Identifiers are branded numbers rather than bigints; they are minted by the core and stay well
 * below 2^53. Content hashes are bigint, because the bits a double drops are the ones that decide
 * whether a build stage is reused.
 */

export type Result<T> = { ok: true; value: T } | { ok: false; status: Status };
"""


def lower_camel(name: str) -> str:
    return name[0].lower() + name[1:] if name else name


def _doc_block(doc: list[str], indent: str = "") -> list[str]:
    if not doc:
        return []
    if len(doc) == 1:
        return [f"{indent}/** {doc[0]} */"]
    out = [f"{indent}/**"]
    out += [f"{indent} * {line}".rstrip() for line in doc]
    out.append(f"{indent} */")
    return out


def emit_typescript(module: Module) -> str:
    out: list[str] = [PREAMBLE]

    for decl in module.declarations:
        if isinstance(decl, IdAlias):
            out += _doc_block(decl.doc)
            out.append(f"export type {decl.name} = number & "
                       f"{{ readonly __brand: '{decl.name}' }};")
            out.append("")
        elif isinstance(decl, Enum):
            out += _doc_block(decl.doc)
            union = " | ".join(f"'{m}'" for m in decl.members)
            out.append(f"export type {decl.name} = {union};")
            out.append("")
        elif isinstance(decl, Struct):
            out += _doc_block(decl.doc)
            out.append(f"export interface {decl.name} {{")
            for f in decl.fields:
                out += _doc_block(f.doc, "  ")
                out.append(f"  {f.name}: {f.type};")
            out.append("}")
            out.append("")
        elif isinstance(decl, Interface):
            name = decl.name[1:] if decl.name.startswith("I") else decl.name
            out += _doc_block(decl.doc)
            out.append(f"export interface {name} {{")
            for method in decl.methods:
                out += _doc_block(method.doc, "  ")
                args = ", ".join(f"{p}: {t}" for p, t in method.params)
                out.append(f"  {lower_camel(method.name)}({args}): Promise<{method.returns}>;")
            out.append("}")
            out.append("")

    text = "\n".join(out).rstrip() + "\n"
    return re.sub(r"\n{3,}", "\n\n", text)


# ------------------------------------------------------------------------------------ driver

def _parse_contracts(root: Path) -> Module:
    base = root / "contracts/cpp/sphanorama"
    module = Module()
    for rel in HEADERS_IN_ORDER:
        path = base / rel
        if not path.is_file():
            continue
        module.declarations.extend(parse(path.read_text()).declarations)
    return module


def generate(root: Path) -> str:
    return emit_typescript(_parse_contracts(root))


# Every generated artefact, keyed by its path relative to the repository root. CI regenerates all
# of them and fails on any diff, so a header change that is not regenerated is a red build rather
# than a boundary that disagrees with itself.
def generate_all(root: Path) -> dict[str, str]:
    module = _parse_contracts(root)
    return {
        "contracts/ts/contracts.d.ts": emit_typescript(module),
        "contracts/cpp/sphanorama/codec.h": emit_cpp_codec(module),
        "shell/src/bridge/codec.generated.ts": emit_ts_codec(module),
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed mirror differs from what would be generated")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args(argv[1:])

    root = Path(args.root)
    try:
        generated = generate_all(root)
    except ContractSyntaxError as exc:
        print(f"contract generation failed: {exc}", file=sys.stderr)
        return 2

    stale = []
    for relative, content in generated.items():
        target = root / relative
        if args.check:
            current = target.read_text() if target.is_file() else ""
            if current != content:
                stale.append(relative)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
        print(f"wrote {target}")

    if stale:
        for relative in stale:
            print(f"{relative} is stale", file=sys.stderr)
        print("run: python3 tools/contract_gen.py", file=sys.stderr)
        return 1
    return 0



# ========================================================================= codec and facade

CPP_CODEC_PREAMBLE = """// GENERATED FILE — DO NOT EDIT. Produced by tools/contract_gen.py.
//
// Per-type encoders over the hand-written primitives in sphanorama/wire.h. Both this and the
// TypeScript codec come from one parse of the contract headers, so the two sides cannot disagree
// about field order or widths — the failure that would decode into plausible nonsense rather
// than failing (ADR 0013).
//
// Only types reachable from a @boundary method are emitted: nothing else crosses.
#pragma once

#include "sphanorama/types.h"
#include "sphanorama/wire.h"

namespace sphanorama::codec {

using wire::Reader;
using wire::Writer;
"""

TS_CODEC_PREAMBLE = """/**
 * GENERATED FILE — DO NOT EDIT. Produced by tools/contract_gen.py.
 *
 * The TypeScript half of the boundary codec. Emitted from the same parse as the C++ half, so
 * field order and widths cannot drift apart (ADR 0013).
 */
import type * as C from '../../../contracts/ts/contracts';
import { Reader, Writer } from './wire';
"""


def _reachable_types(module: Module, methods: list[BoundaryMethod]) -> list[str]:
    """Names of every value type a boundary call can carry, in declaration order."""
    structs, enums, ids = _index(module)
    wanted: set[str] = set()

    def visit(kind: str) -> None:
        if kind.startswith("list:"):
            return visit(f"named:{kind[5:]}")
        if not kind.startswith("named:"):
            return
        name = kind[6:]
        if name in wanted:
            return
        if name in enums or name in ids:
            wanted.add(name)
            return
        struct = structs.get(name)
        if struct is None:
            raise ContractSyntaxError(f"boundary references unknown type {name!r}")
        wanted.add(name)
        for field in struct.fields:
            visit(_field_wire_kind(struct, field))

    for method in methods:
        visit(wire_kind_from_ts(method.returns))
        for _name, ts in method.params:
            visit(wire_kind_from_ts(ts))

    order = [d.name for d in module.declarations
             if isinstance(d, (Struct, Enum, IdAlias)) and d.name in wanted]
    return order


def _field_wire_kind(struct: Struct, field: Field) -> str:
    return wire_kind_from_ts(field.type)


def wire_kind_from_ts(ts: str) -> str:
    """Wire kind for an already-mapped TypeScript type.

    Fields and method signatures are mapped to TypeScript during parsing, so this is the mapper
    the emitters use; wire_kind() maps raw C++ and is what the strictness tests exercise.
    """
    ts = ts.strip()
    m = re.fullmatch(r"Result<(.+)>", ts)
    if m:
        return wire_kind_from_ts(m.group(1))
    if ts == "void":
        return "void"
    if ts.endswith("[]"):
        inner = ts[:-2]
        return f"list:{inner}"
    if ts == "Uint8Array":
        return "bytes"
    if ts == "number":
        return "f64"
    if ts == "bigint":
        return "u64"
    if ts == "boolean":
        return "bool"
    if ts == "string":
        return "string"
    return f"named:{ts}"


def _cpp_put(kind: str, expr: str, enums: set[str], ids: set[str]) -> str:
    if kind == "bool":
        return f"out.PutBool({expr});"
    if kind == "i32":
        return f"out.PutI32(static_cast<int32_t>({expr}));"
    if kind == "f64":
        return f"out.PutF64(static_cast<double>({expr}));"
    if kind == "u64":
        return f"out.PutU64({expr});"
    if kind == "string":
        return f"out.PutString({expr});"
    if kind == "bytes":
        return f"out.PutBytes({expr});"
    if kind.startswith("list:"):
        inner = kind[5:]
        return (f"out.PutCount({expr}.size());\n"
                f"  for (const auto& item : {expr}) "
                f"{{ {_cpp_put(f'named:{inner}', 'item', enums, ids)} }}")
    name = kind[6:]
    if name in enums:
        return f"out.PutI32(static_cast<int32_t>({expr}));"
    if name in ids:
        return f"out.PutF64(static_cast<double>({expr}.value));"
    return f"Encode(out, {expr});"


def _cpp_get(kind: str, target: str, enums: set[str], ids: set[str]) -> str:
    if kind == "bool":
        return f"{target} = in.GetBool();"
    if kind == "i32":
        return f"{target} = static_cast<decltype({target})>(in.GetI32());"
    if kind == "f64":
        return f"{target} = static_cast<decltype({target})>(in.GetF64());"
    if kind == "u64":
        return f"{target} = in.GetU64();"
    if kind == "string":
        return f"{target} = in.GetString();"
    if kind == "bytes":
        return f"{target} = in.GetBytes();"
    if kind.startswith("list:"):
        inner = kind[5:]
        return (f"{{ const size_t count = in.GetCount(1);\n"
                f"    if (!in.ok()) return false;\n"
                f"    {target}.clear();\n"
                f"    {target}.resize(count);\n"
                f"    for (auto& item : {target}) "
                f"{{ {_cpp_get(f'named:{inner}', 'item', enums, ids)} }} }}")
    name = kind[6:]
    if name in enums:
        return f"{target} = static_cast<{name}>(in.GetI32());"
    if name in ids:
        return f"{target}.value = static_cast<uint64_t>(in.GetF64());"
    return f"if (!Decode(in, {target})) return false;"


def emit_cpp_codec(module: Module) -> str:
    structs, enum_map, id_set = _index(module)
    enums = set(enum_map)
    methods = method_table(module)
    names = _reachable_types(module, methods) if methods else [d.name for d in module.declarations
                                                              if isinstance(d, Struct)]
    emitted = [n for n in names if n in structs]

    out = [CPP_CODEC_PREAMBLE]
    for name in emitted:
        out.append(f"void Encode(Writer& out, const {name}& value);")
        out.append(f"bool Decode(Reader& in, {name}& value);")
    out.append("")

    for name in emitted:
        struct = structs[name]
        out.append(f"inline void Encode(Writer& out, const {name}& value) {{")
        if not struct.fields:
            out.append("  (void)out; (void)value;")
        for field in struct.fields:
            kind = _field_wire_kind(struct, field)
            out.append(f"  {_cpp_put(kind, f'value.{field.name}', enums, id_set)}")
        out.append("}")
        out.append("")
        out.append(f"inline bool Decode(Reader& in, {name}& value) {{")
        if not struct.fields:
            out.append("  (void)in; (void)value;")
        for field in struct.fields:
            kind = _field_wire_kind(struct, field)
            out.append(f"  {_cpp_get(kind, f'value.{field.name}', enums, id_set)}")
        out.append("  return in.ok();")
        out.append("}")
        out.append("")

    out.append("}  // namespace sphanorama::codec")
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out)) + "\n"


def _ts_put(kind: str, expr: str, enums: dict[str, Enum], ids: set[str]) -> str:
    if kind == "bool":
        return f"out.bool({expr});"
    if kind in ("i32",):
        return f"out.i32({expr});"
    if kind == "f64":
        return f"out.f64({expr});"
    if kind == "u64":
        return f"out.u64({expr});"
    if kind == "string":
        return f"out.string({expr});"
    if kind == "bytes":
        return f"out.bytes({expr});"
    if kind.startswith("list:"):
        inner = kind[5:]
        return (f"out.count({expr}.length);\n"
                f"  for (const item of {expr}) {{ {_ts_put(f'named:{inner}', 'item', enums, ids)} }}")
    name = kind[6:]
    if name in enums:
        return f"out.i32({name}Values.indexOf({expr}));"
    if name in ids:
        return f"out.f64({expr});"
    return f"encode{name}(out, {expr});"


def _ts_get(kind: str, enums: dict[str, Enum], ids: set[str]) -> str:
    if kind == "bool":
        return "input.bool()"
    if kind == "i32":
        return "input.i32()"
    if kind == "f64":
        return "input.f64()"
    if kind == "u64":
        return "input.u64()"
    if kind == "string":
        return "input.string()"
    if kind == "bytes":
        return "input.bytes()"
    if kind.startswith("list:"):
        inner = kind[5:]
        return (f"Array.from({{ length: input.count() }}, () => "
                f"{_ts_get(f'named:{inner}', enums, ids)})")
    name = kind[6:]
    if name in enums:
        return f"{name}Values[input.i32()]"
    if name in ids:
        return f"input.f64() as C.{name}"
    return f"decode{name}(input)"


def emit_ts_codec(module: Module) -> str:
    structs, enum_map, id_set = _index(module)
    methods = method_table(module)
    names = _reachable_types(module, methods) if methods else [d.name for d in module.declarations
                                                               if isinstance(d, Struct)]

    out = [TS_CODEC_PREAMBLE]
    for name in names:
        if name in enum_map:
            members = ", ".join(f"'{m}'" for m in enum_map[name].members)
            out.append(f"export const {name}Values: C.{name}[] = [{members}];")
    out.append("")

    for name in names:
        struct = structs.get(name)
        if struct is None:
            continue
        out.append(f"export function encode{name}(out: Writer, value: C.{name}): void {{")
        if not struct.fields:
            out.append("  void out; void value;")
        for field in struct.fields:
            kind = _field_wire_kind(struct, field)
            out.append(f"  {_ts_put(kind, f'value.{field.name}', enum_map, id_set)}")
        out.append("}")
        out.append("")
        out.append(f"export function decode{name}(input: Reader): C.{name} {{")
        out.append("  return {")
        for field in struct.fields:
            kind = _field_wire_kind(struct, field)
            out.append(f"    {field.name}: {_ts_get(kind, enum_map, id_set)},")
        out.append("  };")
        out.append("}")
        out.append("")

    return re.sub(r"\n{3,}", "\n\n", "\n".join(out)).rstrip() + "\n"

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
