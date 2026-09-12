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
SIFT 0.124°, with every detector registering all eleven consecutive pairs. Those came from a harness
that handed `EstimatePairwise` the *exact truth* of each step as its sensor prior. The fit seeds its
search with the prior, so the answer was correct before a pixel was read.

It was found by sabotage during review round 1 of the branch that produced it: a reviewer replaced
the whole estimator with `return the prior` and every detector passed at `median = 0.0000`, scoring
*better* than the real implementation. Instrumenting the search showed that on 31 of 33 steps it had
never beaten the prior's inlier count. The numbers were real measurements of the wrong thing — they
measured how good a phone's orientation is, which was already known and is not what the engine does.

**AKAZE's 0.063° survives the correction unchanged, and that is not a copy-paste.** It is the one
detector whose median the perturbation did not move at three significant figures: AKAZE was the
detector that least needed the prior, so taking the truth away cost it least. ORB and SIFT both
moved, and ORB's registered-pairs count moved from eleven to eight — which is where the artefact was
hiding, since a step the estimator declines chains truth forward and enters the sample as an exact
zero. A harness fed truth cannot produce a declined step at all.

## Decision

**A retracted measurement is recorded in an ADR. The roadmap carries the current figures and a
pointer.**

`docs/06-roadmap.md` keeps the harness description, the table, and the sentence relating the table to
the 0.5-degree threshold. Where it previously explained the retraction, it now names this file in one
clause. The retracted numbers, how they were produced, and how they were caught are above.

The current figures replacing them come from a harness that perturbs the prior by three degrees,
which is the order a fused phone orientation is out by when it is working. They are in the roadmap,
not here: this ADR is the record of what was withdrawn, and duplicating the live table into it would
create exactly the second copy that drifts.

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

It loses on what happens next. The roadmap already carries two "an earlier version of this
claimed" corrections in its body, and a document that accumulates its own revision history inline
is the failure mode `docs/adr/README.md` names when it refuses to let an ADR's body be edited into
agreement with the present. Putting the *table* in the roadmap and the *withdrawal* in the record
keeps each document doing one job.

That README's banner mechanism exists for precisely the "a reader would otherwise meet this and
believe it" problem, and it is what would ordinarily be reached for here. It did not apply, because
a banner needs an ADR to sit on and no ADR carried the wrong figures — the roadmap did. This file
is the ADR that was missing.
