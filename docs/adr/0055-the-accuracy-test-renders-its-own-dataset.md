# 0055 — The accuracy test renders its own dataset and skips without the generator

**Status:** accepted

## Context

ADR 0053 committed a four-frame 48x36 dataset as a **format** fixture and drew a line that this ADR
now has to stand on: *a format fixture may be committed, a measurement dataset may not.* The reason
is size and staleness — a dataset big enough to measure a feature detector on is megabytes, and a
committed one is a snapshot of a generator that has since moved.

Phase 2 exits on a median angular error. Computing one needs a dataset big enough for a detector to
find features in, which `synthetic-ring-4` is not: 48x36 frames are a format specimen. So the
measurement needed a dataset that cannot be committed, and the three ways to get one are the ones
ADR 0053's Rejected section listed two of.

That section weighed **generating at build time** (rejected: a contributor without `uv` gets a broken
build) and **committing the output** (rejected for a measurement dataset, for the reasons above). A
reviewer pointed out during round 1 of that PR's review that a third option was never weighed:
generating at *test* time, where the absence of the generator is a skipped check rather than a
broken build. The reply agreed and did not act, because nothing yet needed it.

## Decision

`core/test/engines/registration_accuracy_test.cpp` renders its own dataset into a temporary
directory by shelling out to `uv run --group datasets tools/synth_dataset.py`, reads it with the
loader, measures, and deletes it. When that command fails — no `uv`, no `datasets` group, no network
for a first resolve — the test calls `GTEST_SKIP()` and says what was missing.

**Shelled out rather than linked**, because the generator is Python and stays Python for the reason
ADR 0050 gives: a dataset rendered through the code under test cancels any error the two share. A
C++ reimplementation inside the test would be exactly the thing ADR 0050 exists to prevent, and it
would be *harder* to notice because it would live next to the code it was supposed to be independent
of.

**Skipped rather than failed**, and the skip message says so in those words: a skipped measurement is
not a passing one. The gate runs `uv` already (`tools/gate.sh` refuses to start without it) and CI's
`contracts` job resolves the `datasets` group, so the skip is for a contributor's bare checkout and
not for the definition of done.

## Consequences

- **The measurement is not in CI's `native` job.** That job has no `setup-uv` step, so this test
  skips there — which is a line of YAML away from being fixed and is not fixed here, because adding
  a Python toolchain to the native job is a change to how CI is shaped and deserves its own decision.
  Until then the number in `docs/06-roadmap.md` is one a contributor reproduces locally with the
  whole gate, not one CI defends.
- **The test is slow by this repository's standards** — about four to six seconds a detector, most of
  it rendering. Three detectors is fifteen seconds on top of a native suite that otherwise runs in
  seven. Accepted: it is one test, it is the one the phase exits on, and the alternative is not
  measuring.
- A temporary directory per run, removed by a destructor that runs on every path out including a
  failed assertion. The same shape `Scratch` uses in the loader's tests, for the same reason.
- **The dataset's parameters are in the test rather than in a file**, so changing them is a code
  change that shows up in a diff. Twelve frames at 640x480 through a 66 by 50 degree lens: enough
  overlap at 30-degree steps for every consecutive pair to share most of a frame, and small enough
  to render in about a second.

## Rejected alternative

***A ctest fixture that renders once for the whole suite.*** Faster — one render instead of three —
and rejected because it couples the three detector cases through a shared directory that outlives
them. The failure it invites is the one this repository has hit repeatedly in other forms: a case
that passes because of what another case left behind. Fifteen seconds is a cheap price for three
independent runs, and if it stops being cheap the fix is to render once *per test binary invocation*
with the directory owned by the fixture class, not to share it across cases.

***Commit a bigger dataset and keep the test hermetic.*** This is the option ADR 0053 already
rejected, and nothing about needing a measurement changes the argument: a committed measurement
dataset is a snapshot of a generator that will move, and the C++ would go on passing against bytes
no writer produces any more. The committed fixture stays what it is — a format specimen, four frames,
22,570 bytes — and the measurement renders fresh.
