# ADR 0010 — Resource access is verified by a shared contract suite

**Status:** accepted

## Context
Every resource access will have at least two implementations: a TypeScript adapter for the browser
and a native one for the bench and tests. The property that actually matters is not that either
works, but that **they agree**. Two independently written test files cannot establish that, and
they diverge quietly — the browser store spills at a different moment than the native one, a
manager test passes, and the phone crashes.

Fakes are needed for the same reason from the other direction: a manager that can only be tested
against a real camera cannot be tested at all.

## Decision
For each resource access, write **one contract suite** parameterised over the implementation
(GoogleTest typed tests), and list every implementation in its type list. Adding the native or
browser-backed store is one line, and it inherits every case already written.

The fakes model *behaviour*, not storage. `FakeFrameStoreAccess` has a heap ceiling, refuses
allocations it cannot fit, and moves spilled bytes out of the heap budget. Modelling the ceiling
matters far more than modelling OPFS: every interesting bug in the real store is about running out
of room, and a fake with unlimited memory lets manager tests pass while the device dies.

Fakes live in `core/test/support/` and are excluded from the layer check, which scans `core/src`
and `contracts/` only.

## Consequences
- The contract suite is the specification. When the OPFS store lands, the question "does it behave
  like the fake?" is answered by CI rather than by reading two files side by side.
- Writing the suite before any real implementation surfaced a defect in the contract itself:
  `FrameRef` carried a `Residency` field, which is a snapshot that every copy of the handle
  invalidates the moment the store spills the frame. It is replaced by
  `IFrameStoreAccess::ResidencyOf`, because residency is store state, not frame identity. The
  contract test could not be written honestly against the old shape — which is the whole argument
  for writing it first.
- Some behaviour is not shared and stays outside the suite as a direct test: only the fake sensor
  can be configured to have no motion access at all, and only the fake camera can report what the
  session asked it to do.
- Cost: a fake is real code with real bugs. Accepted — the alternative is untestable managers.

## Rejected
- *Mocks with expectation scripts* (gmock). They assert that a manager made particular calls in a
  particular order, which nails tests to today's implementation and makes every refactor a test
  rewrite. The fakes assert on observable outcomes instead.
- *A separate test file per implementation.* Half the work of a shared suite and none of the
  guarantee.
