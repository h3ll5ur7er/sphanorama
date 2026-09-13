---
name: sphanorama-review
description: Review a change to this repository by spawning reviewer subagents, one per lens, each publishing its findings to the pull request, then answering them there. Use on every PR before merging, in place of an external review bot. Also use when asked to review a diff, a PR, or a branch here, or when a change is finished and needs scrutiny it cannot give itself.
---

# Reviewing a change

A review here is run by spawning subagents, not by asking a service. This file says what to point
them at, what they must hand back, and what to do with it.

`sphanorama-engineering/SKILL.md` is the standard being reviewed *against* — layers, contract
discipline, the definition of done. This file is about finding where a change fails it.

## Why subagents rather than one more careful read

The author of a change cannot review it. Not for want of care: the reasoning that produced the
code is the same reasoning that would check it, so a wrong assumption is invisible from inside. A
subagent starts cold, reads the diff without the author's intent, and has to reconstruct why each
line is there — which is exactly the pass that finds a guard that cannot fire, a sentinel that
absorbs too much, or a promise the header makes and the code does not keep.

Self-review still happens first and is not replaced by this. Sabotage every new test, reread the
diff adversarially, run the whole gate. This is the pass *after* that one.

## How to run it

1. **Open the PR first.** The review is published on it, so it has to exist. A draft is fine.
2. **Get the diff.** `git diff origin/main...HEAD` — the three-dot form, so it is the branch's own
   work rather than everything main has done since.
3. **Pick the lenses** from the table below that the diff actually touches. A C++-only change does
   not need the shell-ordering lens; a docs-only change needs none of them. Two to four is usual.
   One lens per subagent: a reviewer given six things to look for finds the easy ones.
4. **Spawn them in parallel, in the background.** They are independent, and each posts its own
   review to the PR when it is done — which works only if each obeys the publishing order below,
   because the pending review is a per-user lock and they all share one account.
5. **Reconcile in public.** Reply to each finding on its own thread with what you did about it.
   Every finding is yours to verify before you act on it — the subagent's confidence is not
   evidence.

Each subagent gets: the lens, the PR number, the diff (or the branch to diff itself), a pointer to
`.claude/skills/sphanorama-engineering/SKILL.md`, and the reporting contract below.

**Point them at a copy of this file that exists on the branch they will check out.** A reviewer
works in a worktree of the branch under review, so a skill that has not merged to `main` yet is
simply not there — the first four rounds this skill ran, every reviewer was told to read it and
only the fourth said it could not find it. Until it is on `main`, name the branch in the prompt:
`git show origin/<this branch>:.claude/skills/sphanorama-review/SKILL.md`. The same goes for any
lens or rule that lives on an unmerged branch.

**Sweep the worktrees between rounds.** Each reviewer builds OpenCV from source in its own
worktree — 1.3 to 2.7 GB apiece — and those survive the agent that made them. By round 3 that had
filled the disk twice: one reviewer could not configure a build at all and fell back to proving the
shared tree was honest by hashing every tracked file against it, and another had to delete 8.5 GB of
earlier rounds' scratch before it could link. Both said so in their reports rather than quietly
measuring something less trustworthy, which is the only reason it is known. Delete the finished
agents' `build/` directories before starting the next round, and leave the live one alone.

**They review and publish. They do not fix.** No edits to tracked files, no commits, no pushes, no
approving, no merging. A reviewer that fixes what it finds has stopped being able to tell you what
it found, and two agents editing one branch is how a green tree becomes a mystery. Building and
running tests to confirm a finding is expected; scratch files go in `/tmp`.

## Publishing the review

**Every reviewer posts to the pull request, and so does every answer.** Not because a report needs
an audience, but because the reasoning is the artefact. A repository records what was decided; the
threads record *why*, and why the alternative was rejected — which is the thing nobody can
reconstruct later from a diff, and the thing a human collaborator arriving cold most needs.

It also puts agent and human on the same footing. A person reviewing this PR reads the same threads,
answers in the same place, and can disagree with a finding or with the answer to one. A review that
lived only in a transcript would make them a spectator.

A reviewer posts once, as a single review carrying all of its findings:

1. `mcp__github__pull_request_review_write` with `method: "create"` opens a pending review.
2. `mcp__github__add_comment_to_pending_review` adds each finding, anchored to its file and line.
   The anchor is what makes a finding actionable, so use it rather than describing the location in
   prose.
3. `mcp__github__pull_request_review_write` with `method: "submit_pending"` and `event: "COMMENT"`
   submits it. **Never `APPROVE` or `REQUEST_CHANGES`** — a reviewer here informs, it does not gate.

**Finish every scrap of investigation before step 1, and run steps 1 to 3 back to back.** This is
not tidiness, it is the difference between a review and a deadlock. GitHub allows **one pending
review per user per pull request**, and every reviewer here authenticates as the same account — so
a pending review opened at the start of the work is a lock held over the other reviewers for as
long as the investigation takes. Four lenses in parallel produced exactly that: one held the lock,
three sat retrying, and nothing was published at all. Drafting first shrinks the window from twenty
minutes to seconds.

So: write the findings to a scratch file as they are found, and treat the three calls above as one
indivisible step at the very end. Two rules follow from the same fact:

- If `create` fails because a pending review already exists, **that is another reviewer mid-publish,
  not stale state.** Wait a minute and retry, up to ten times. Never delete a pending review that is
  not yours — `delete_pending` would destroy their work, and it is not recoverable.
- If ten retries still fail, publish the findings as one ordinary PR comment with
  `mcp__github__add_issue_comment`, labelled with the lens and with `file:line` written into the
  text. Say in the summary that this happened. An unanchored review is worth much less than an
  anchored one and infinitely more than a lost one.

**Budget the API too.** Four reviewers investigating a PR can exhaust the hourly REST limit between
them, which then blocks the *answers* as well as the reviews — reading a diff through the API in a
loop is the usual culprit, and reading it from the worktree with `git` costs nothing. Prefer git
over API calls for anything that is in the checkout.

The review body says which lens it is and what was examined. **A lens that found nothing still
posts**, saying what it looked at and found clean: a review nobody can see is indistinguishable
from one nobody ran, and "these files, this lens, nothing" is a fact worth having on the record
when the same question comes up in three months.

**The posted review is the deliverable. What a reviewer hands back to the caller is a receipt for
it, not a substitute.** A reviewer that ends with its findings written out beautifully in a
transcript and nothing on the pull request has not reviewed anything, however good the findings
are — this happened on the first run of this skill, with one lens out of three, and the whole
trail for one commit's reasoning had to be reconstructed and published afterwards by hand. So:
submit the review *before* writing the summary, and make the last line of the summary the review's
URL. A summary with no URL in it is a reviewer reporting that it did not finish.

Every comment ends with the attribution footer, so a reader knows what wrote it:

```

---
_Generated by [Claude Code](https://claude.ai/code)_
```

## What each reviewer must hand back

For every finding:

- **Where** — file and line.
- **What breaks** — concrete inputs or state, and the wrong output or crash they produce. Not "this
  could be unsafe".
- **Verdict** — `CONFIRMED` if it was reproduced (a test written, a value printed, a sanitizer run,
  a log read) or `REASONED` if it is an argument from the code that was not executed. Both are
  worth having; conflating them is not. A `REASONED` finding about arithmetic is usually cheap to
  promote — say what would settle it.
- **Why it is not already handled** — the check it walks past, or the caller it reaches through.
  Most wrong findings die here, and asking for it up front kills them before they cost anything.

And once at the end: **what was looked at and found clean.** A review that lists only problems
cannot be told apart from one that stopped early.

And the URL of the submitted review, as the last line. See "Publishing the review": the summary is
the receipt, and this is what it is a receipt for.

## The lenses

| Lens | What it looks for |
| ---- | ----------------- |
| **The boundary and its arithmetic** | Every number arriving from JavaScript is a double. Casts to narrow integers (`static_cast<int32_t>` of a NaN, an infinity or 1e300 is undefined — `wire::GetInteger<T>` exists for this). Products that overflow before the check that would have refused them — ask by division. `size_t` is 32 bits on wasm32, so a count times an element size wraps. Length prefixes and counts bounded against the bytes actually present |
| **Pixels and spans** | A `FrameRef` is a plain value a caller passes in; the store's entry is the only thing that knows the real allocation. Stride against a row's own width, dimensions against the bytes pinned, a sampling window against the frame it samples. Accumulator width against the largest sum a loop can reach. Every `Pin` released on every path out, failures included |
| **Contract promises** | Read the header comment and then the implementation, and ask whether the second does what the first says. "Whatever residency a frame had before this call, it has after it" was kept by coincidence for one tier. A comment that is aspirational is a defect in the same way a wrong line is |
| **Sentinels and second copies** | A value meaning "none" only means it if nothing else can produce it: check the writer cannot store one, the reader cannot invent one from a failure, and an unset argument is refused rather than answered. Separately: any fact held in two places will drift — a flag beside the thing it describes, a client cache mirroring what the core knows. Prefer deriving it; if it must be copied, find what invalidates the copy |
| **Ownership and lifetime** | Who owns a frame, and what a refusal leaves behind. `Forget` can fail and keep accounting for the bytes, so dropping the handle orphans them. An offered frame belongs to its caller. Caches bounded in *every* dimension they can grow in — across cells and within one |
| **Ordering in the shell** | Every call crosses a worker, so several are in flight at once. For each guard, ask what question it answers: a per-render ticket and a per-cell identity are different questions, and using one for the other's job is a race that only shows under fast input. Late answers must not paint, and must not haul the user back |
| **Tests that cannot fail** | For each new test, what would make it fail? A test that passes because something refused the input earlier proves only that. Watch for: an arrangement whose oscillation never reaches the condition; an assertion satisfied by a default (`toBeHidden` on an element hidden by something else); a guard asserted against a fake that cannot produce the state. The build's own version of this used to belong here — `npm run build` only *stages* a prebuilt wasm, so a contract change ran the browser tests against the previous core — and `tools/check_dist_fresh.mjs` now refuses that as Playwright's `globalSetup`, over both wasm builds, the glue, the C++ sources and the build files. It has its own tests. What is left for a reviewer is whether it has grown a hole, not whether the trap is open |
| **Docs and ADRs** | Did the change invalidate a sentence somewhere? `docs/06-roadmap.md`, the volatility map, the contract READMEs and the engineering skill all make claims the code has to keep. An ADR is required for everything on `docs/00-principles.md` § "When an ADR is required" — read it rather than this row, which used to restate it and fell two triggers behind — and its *consequences* section is where the costs it accepted belong |

## Handling what comes back

Verify before acting. A confirmed finding is a bug report; a reasoned one is a hypothesis, and both
can be wrong about this codebase.

**Answer every finding on its own thread, and resolve the ones you addressed.** The answer is part
of the record: the commit that fixed it, or the argument that it is not a defect. A finding left
without a reply reads as one nobody looked at.

- **Reproduce first, then fix.** The test comes before the change, as always. A finding you could
  not reproduce is a finding you do not yet understand.
- **Declining is a legitimate outcome**, with the evidence, posted where the finding is. Run the
  proposed change as a sabotage: if implementing it fails a test, that test is the argument. Say
  which one. A declined finding with its reasoning on the thread is worth more to the next reader
  than a quietly closed one, because it says what was considered and why it lost.
- **Say when a finding changed your mind about something else.** Three separate rounds on one PR
  each closed a different leak into the same sentinel; none was visible from the decision itself,
  and the pattern only became sayable once all three were written down together.
- **A fix is new code**, subject to everything above. The worst defect on a recent PR was introduced
  *by* a fix, and caught only because the existing test for the opposite case was still there and
  the whole suite was run.
- **Re-run the review when a round found something real.** A round that found nothing is where it
  stops. Findings that keep arriving on your own fixes mean the root cause is still there.

## What this does not do

It does not approve, and it does not merge. Green, mergeable and reviewed is the finish line;
merging is the maintainer's call unless they have said otherwise for that branch.

It also does not replace a human reading the PR. What it does is make sure that when one arrives,
the reasoning is already there in the threads rather than locked in somebody's session.
