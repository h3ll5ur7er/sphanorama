# ADR 0001 — Decompose by volatility, not by function

**Status:** accepted

## Context
The obvious decomposition of a panorama app is `Capture → Stitch → Export`. All three headline
features of this product (burst per cell, retakes, ghost removal) cut across all three of those
boxes, which means every one of them would be a multi-component change forever.

## Decision
Apply the iDesign method: identify axes of volatility ([docs/02](../02-volatility-map.md)), give
each one exactly one owning component, and arrange components into the client / manager / engine /
resource-access / resource layers with enforced call rules.

## Consequences
- Three managers, five engines, seven resource accesses — and each one names the change it absorbs.
- Call rules are checked in CI; a violating include fails the build. Architecture that is not
  enforced decays.
- Cost: more indirection than a 2000-line stitcher would need on day one. Accepted, because the
  features that make this project worth doing are precisely the ones that cut across a naive split.

## Rejected
Functional decomposition, and "one big `Stitcher` class with options". Both make retakes and
burst selection structural changes rather than parameters.
