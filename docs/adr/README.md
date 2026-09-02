# Architecture decision records

One file per decision that is expensive to reverse. Format: context, decision, consequences,
and the alternative we rejected and why.

| ADR | Decision |
| --- | -------- |
| [0001](0001-decompose-by-volatility.md) | Decompose by volatility (iDesign), not by function |
| [0002](0002-cpp-wasm-core-ts-shell.md) | C++/WASM core owns business logic; TypeScript supplies resource-access adapters |
| [0003](0003-candidate-sets-not-frames.md) | A capture cell owns a set of candidates, not a frame |
| [0004](0004-build-as-incremental-graph.md) | A build is a fingerprinted DAG, not a pipeline run |
| [0005](0005-opencv-piecemeal-not-stitching-module.md) | Use OpenCV algorithms piecemeal; do not use its `stitching` module |
| [0006](0006-no-exceptions-result-type.md) | `Result<T>` everywhere; no exceptions across layers or the WASM boundary |
| [0007](0007-tests-and-docs-are-gated.md) | Tests and documentation are gated in CI, not left to discipline |
| [0008](0008-contracts-are-the-include-path.md) | One interface per header; `contracts/cpp` is the include root |
| [0009](0009-the-cpp-header-is-the-idl.md) | The C++ header is the IDL; a strict parser generates the TypeScript mirror |
