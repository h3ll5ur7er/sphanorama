# ADR 0014 — Resource-access ports are synchronous over a resident host

**Status:** accepted

## Context
The resource-access contracts are synchronous: `ReadDocument` returns `Result<std::string>`, not a
future. The browser APIs behind them are not — IndexedDB, `getUserMedia` and the File System
Access API are all asynchronous. Something has to give, and where it gives decides how much of the
platform's shape leaks upward.

Three ways out:

- **Make the contracts asynchronous.** Every manager becomes a state machine, and V9–V15's
  volatility — which the resource-access layer exists to contain — is spread across every
  component that touches a resource. This is the outcome the architecture is built to prevent.
- **Asyncify / JSPI.** Emscripten can suspend the WASM stack across an async JS call, preserving
  the synchronous contracts. It costs binary size (Asyncify instruments the call graph) and JSPI
  is not available everywhere. Real, but heavy for a document read.
- **Keep the contracts synchronous and make the host's state resident**, persisting behind it.

## Decision
Ports are synchronous. The page-side host keeps the data the core reads **resident in memory**,
hydrated before the core is handed to it, and persists asynchronously behind that.

For the project store this is a clean fit rather than a compromise: documents are small, the whole
set is loaded once at startup, reads are memory reads, and a write is visible to the next read
immediately. Durability follows within a flush window, and the client flushes explicitly at
session end and on `pagehide`.

Two consequences are deliberate:

- **A write is not durable when the call returns.** For project metadata that is the right trade;
  for anything where it is not, the client awaits `flush()`.
- **A failed persist does not roll back the resident copy.** Losing durability is bad; losing the
  session in progress is worse. The failure is reported separately through `lastPersistError()`
  rather than by making the core's read fail.

Ports live under `bridge/resource_access/`, because they are resource-access code that needs
Emscripten and `bridge/` is the only tree allowed to reference it (`tools/no_browser_check.py`).
The layer checker judges those subtrees as resource access rather than as clients, so a port is
held to the same rules as a native implementation.

**The composition root is exempt from the layer rules**, and only it. Deciding which
implementation each contract gets means naming every concrete type, which is not a business
dependency — every rule in docs/03 §3.3 is about who may *call* whom. `tools/layer_check.py`
lists `bridge/runtime.h` and `bridge/runtime.cpp` by exact path so the exemption cannot widen to
the code around them.

## Consequences
- The contracts stay as written, so no manager learns that storage is asynchronous.
- `MemoryProjectStoreAccess` and `BrowserProjectStoreAccess` are both real implementations of one
  contract, held to the same shared suite (ADR 0010) — which is what makes the composition root
  able to swap them per platform with a preprocessor branch and nothing else.
- **This does not settle the camera.** A burst takes time and cannot be made resident in advance,
  so `ICameraAccess::CaptureBurst` will need either Asyncify on that call path or a redesign in
  which the client drives burst timing. That is a decision to take with measurements — the size
  budget tool exists for exactly this — and it is deliberately not taken here.

## Rejected
- *Asynchronous contracts.* It solves the mechanical problem by spreading the volatility the
  architecture exists to contain.
- *Asyncify everywhere, now.* Pays binary size on every call path to solve a problem the project
  store does not have. Worth revisiting when the camera port forces it, with numbers.
- *Letting a failed persist fail the read.* It would turn a storage hiccup into a lost session.
