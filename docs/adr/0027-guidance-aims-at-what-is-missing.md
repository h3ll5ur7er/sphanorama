# 0027 — Guidance aims at what is missing

## Context

`ICoveragePlannerEngine::Locate(current, plan)` answered "which cell is this orientation nearest
to". It had no way of knowing what had already been captured, because nothing in its arguments
said so.

That is the wrong question. A person holding a phone is not asking which cell is closest; they are
asking where to point next. Aiming them at a cell they finished a minute ago and telling them to
hold still is an instruction to stand there photographing what they already have — and from behind
the phone it is indistinguishable from working, because the only feedback is an angular error that
closes exactly as it should.

It showed up on the first device run: the guidance line read `cell 3 · hold still · 4/22 done`
with four cells captured, and cell 3 may well have been one of them. It became urgent because the
next change puts a marker in the viewfinder for the cell being aimed at, which would have pointed
confidently at nothing worth capturing.

`GuidanceAction::SphereDone` had also been in the contract since the start, handled by the capture
client, and emitted by nothing. A finished sphere went on naming whichever cell the phone happened
to be nearest, forever.

## Decision

`Locate` takes the coverage state as a third argument and searches only the cells it lists as
holes. With no holes left it reports `SphereDone`.

Coverage arrives as `Evaluate`'s answer rather than as the candidate set, so what counts as
covered is defined in one place — the engine's own `Evaluate` — instead of being re-derived by
whoever calls `Locate`. The manager therefore evaluates on every tick and passes the result
through.

An **empty** state means *no information*, not *nothing missing*. At the start of a session
nothing is captured and nothing is a hole, and reading that as a finished sphere would end a
capture before it began. The engine distinguishes the two by `nodesTotal`, which is zero only for
a state nobody computed.

## Consequences

Guidance sends the user to a cell they still need, and says so when there are none left. The
reticle, the guidance line and the coming viewfinder markers all inherit this for free, because
they read `targetNode`.

Coverage is evaluated once per tick — a scan of the plan against the candidates, a few thousand
comparisons for a 32-cell sphere. That is the deliberate trade against caching it: a cached answer
would have to be invalidated at each of the five places candidates change, and a missed one puts
the reticle back on a captured cell with nothing on screen to show it happened. The cost is
measurable and bounded; the bug it avoids is silent.

A holes list naming cells the plan does not contain falls back to the whole plan rather than
leaving nothing to aim at. An odd target beats refusing to guide.

## Rejected alternative

**Let the manager filter.** It holds the candidates and knows perfectly well which cells have
none, so it could ask `Locate` for the nearest and override the answer. This was rejected because
"which cell does the user still need" is a coverage question, and coverage is V4's axis. A manager
deciding it would be a second definition of covered, free to drift from `Evaluate`'s — and the
first time the definition grew a quality bar or required a *selected* candidate, the two would
disagree with nothing to catch it.

**Pass the candidates and let `Locate` re-derive coverage.** Same information, but it would put a
second implementation of `Evaluate`'s rule inside the same engine.
