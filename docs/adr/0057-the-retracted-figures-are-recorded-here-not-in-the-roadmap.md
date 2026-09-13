# 0057 — The first accuracy figures measured the prior, and the retraction lives here

**Status:** accepted

## Context

Phase 2 exits on a median angular registration error, and `docs/06-roadmap.md` is where that number
is published. The first figures put there were wrong in a way that is worth recording rather than
quietly replacing, so the roadmap grew a retraction paragraph immediately above the corrected
table — a paragraph explaining a table that no longer exists, in a document whose job is to say
what is built and what is next.

That is the wrong place for it. A roadmap is read forwards by somebody deciding what to do; a
retraction is read backwards by somebody checking whether a number can be trusted. Leaving the two
interleaved means every future correction to a measured figure grows the roadmap by a paragraph that
is dead weight to its actual readers — and this project has already published one wrong measurement,
so there is no reason to think this is the last.

**What was retracted.** A table stood in the roadmap giving medians of AKAZE 0.063°, ORB 0.099° and
SIFT 0.124°. It had no registered-pairs column, and could not have had a useful one: `accepted` was
`true` on every returned result under the code of the day, and on this dataset every consecutive
pair cleared the refusal that precedes it, so all eleven of every detector entered the chain. (The
second half of that is measurement, not construction — a pair *can* be refused outright before
`accepted` is reached, and on these twelve frames none was.) The figures came from a harness that
handed
`EstimatePairwise` the *exact truth* of each step as its sensor prior, and the fit seeds its search
with the prior, so the answer was correct before a pixel was read.

It was found by sabotage during review round 1 of the branch that produced it: a reviewer replaced
the whole estimator with `return the prior` and every detector passed at `median = 0.0000`, scoring
*better* than the real implementation. Instrumenting the search showed that on 31 of 33 steps it had
never beaten the prior's inlier count. The numbers were real measurements of the wrong thing — they
measured how good a phone's orientation is, which was already known and is not what the engine does.

**AKAZE's median came back at the same three significant figures after the correction, and that is
not a copy-paste.** It is the one detector the perturbation did not move at that precision: AKAZE
needed the prior least, so taking the truth away cost it least. ORB and SIFT both moved. (The
figures as they stand are in the roadmap and will move again; what is recorded here is that one of
the three was unmoved *by this correction*.)

**ORB's registered-pairs count went from eleven to eight, and the perturbation is not why — a
reviewer had to correct this ADR on its own branch.** The first version of this section offered the
eleven-to-eight move as where the artefact was hiding, on the reasoning that a harness fed truth
cannot produce a declined step. That reasoning is wrong twice over. Under the code of the day
`accepted` was `bestInliers.size() >= kMinimumCorrespondences && median <= kInlierPx`, and **both**
conjuncts were vacuous on a returned result: the count is the condition the early return above has
already enforced, and the median is taken over precisely the rows that count selected, so it cannot
exceed the radius that selected them. Injected noise from zero to four pixels produced
`accepted = true` at every level. So it was `true` on everything returned and nothing could be
declined whatever the prior was — a fact about the gate, not about being fed truth. (Quoting only
the first conjunct, as the first version of this paragraph did, made the argument look like it
turned on the obvious half. It turns on both, and the second is the one a reader would not guess;
`feature_registration_engine.cpp` records it beside the gate that replaced it.) And under the gate
as it stands, feeding truth would decline the same three: `accepted` now also requires
`agreeing >= kInlierFraction` at 0.2, and the truth rotation's own support on those pairs is 11 of
128, 19 of 141 and 13 of 154 — 0.086, 0.135 and 0.084, every one below the gate (ADR 0056 records
the counts).

So the eleven-to-eight move is the `accepted` redefinition, which landed in the same work as the
perturbation and is documented in ADR 0056. It is not evidence for the artefact. The evidence for
the artefact is the sabotage above: a `return the prior` stub beating the real implementation.
Keeping the two apart matters because the accuracy test's registered-pairs conjunct exists to stop
a detector chaining truth forward and scoring a free zero — a real hazard, and one the perturbation
alone does not create.

## Decision

**A retracted measurement is recorded in an ADR. The roadmap carries the current figures and a
pointer.**

`docs/06-roadmap.md` keeps the harness description, the table, and the sentence relating the table to
the 0.5-degree threshold. Where it previously explained the retraction, it now names this file in one
clause. The retracted numbers, how they were produced, and how they were caught are above.

The current figures replacing them are in the roadmap, not here: this ADR is the record of what was
withdrawn, and duplicating the live table into it would create exactly the second copy that drifts.
The roadmap says what the replacing harness does and why three degrees; that sentence is not
repeated here for the same reason.

## Consequences

- The roadmap gets shorter when a figure is corrected instead of longer, and a reader who only wants
  to know what the current number is no longer has to read past what it is not.
- The retraction is now two clicks from the figure it concerns rather than one paragraph above it.
  That is the cost, and it is the right way round: the people who need the retraction are auditing a
  claim and will follow a link, while the people reading the roadmap are not.
- This is the first ADR here whose *subject* is a withdrawn number rather than a design choice.
  Other ADRs carry measurements — 0046's iteration budget, 0049's Jacobi-versus-power-iteration
  sweep counts — but they carry them in support of a decision. This one's decision is small (where a
  retraction goes) and its Context does the work, which is the right way round: the Context section
  is exactly where "what we believed at the time" belongs. A future retraction follows this shape
  rather than inventing another.
- ADR 0055 and ADR 0056 both describe parts of the same episode and are not superseded by this: 0055
  is about where the dataset comes from, 0056 is about what `accepted` means, and this is about which
  numbers were withdrawn. Three files, because they will be invalidated by different things.

## Rejected alternative

**Leave the retraction in the roadmap and accept the length.** It has one real advantage: a reader
cannot miss it. Somebody who quotes the AKAZE figure out of the table has, in the current layout,
just read the paragraph saying an earlier version of that table was an artefact — and that is a
useful thing to have read.

It loses on what happens next. The roadmap already corrects itself inline in five places, two of
them spelled "an earlier version of this claimed", and a document that accumulates its own revision history inline
is the failure mode `docs/adr/README.md` names when it refuses to let an ADR's body be edited into
agreement with the present. Putting the *table* in the roadmap and the *withdrawal* in the record
keeps each document doing one job.

That README's banner mechanism exists for precisely the "a reader would otherwise meet this and
believe it" problem, and it is what would ordinarily be reached for here. It did not apply, because
a banner needs an ADR to sit on and no ADR carried the wrong figures — the roadmap did. This file
is the ADR that was missing.
