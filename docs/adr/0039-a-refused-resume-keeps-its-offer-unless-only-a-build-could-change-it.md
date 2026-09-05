# ADR 0039 — A refused resume keeps its offer unless only a new build could change it

## Context

ADR 0035 and ADR 0036 were written in parallel and never reconciled on one point, and the roadmap
has carried it as an open question since.

ADR 0035 made `Resume` refuse a session document whose tier token does not match the tier this
device is holding, and kept the document rather than deleting it. Looking ahead to a page that
would one day offer a resume, it predicted that what such a page would need was "a project that
stops being offered rather than a document that has been destroyed".

ADR 0036 then built that page, and it does neither. `ProjectSummary.hasSession` says a session
document exists, the page offers the newest project that has one, and a refusal puts the reason on
the stage line and leaves a fresh capture one press away. The offer is rebuilt from `hasSession` on
every load, so the same project is offered again next time, and refused again.

The question left between them: should a refusal that is *not* about the tier — a document shape
this build cannot read — stop the offer, while a tier mismatch leaves it?

Three things make it a real question rather than a detail.

- The refusals are not alike. `Resume` can say no for reasons that are about *this attempt* (the
  tier this device currently holds, a store that would not take the frames back, a camera another
  tab is using) and for reasons that are about *this build* (a document shape it does not parse, a
  document naming a cell the stored spec no longer plans). Pressing again can answer the first
  differently. Nothing the user does answers the second differently.
- An offer that always fails is a bad offer. A user who presses resume, reads a refusal, reloads
  and is offered the same resume again has been told nothing except that the app is broken.
- But an offer withdrawn wrongly is worse than a bad one. The frames are still on disk and the
  document still names them; the only thing standing between the user and their sphere is an offer
  they can press.

## Decision

**The offer comes back for every refusal except `Unsupported`, and it comes back for this tab
only.**

**Read off the code, not the component.** `Unsupported` is the core's word for "this build does not
read that" wherever it appears. The one component that means something else by it — the camera
adapter, where it is the secure-origin refusal — belongs on the same side of the line anyway: the
origin is the page's own, and no press changes it either.

**One classification produces both the sentence and the button.** `describeResumeRefusal` returns
`{ message, offerAgain }` rather than a string the caller pairs with a second judgement of its own.
A line reading "try again" beside a button that is gone, or a button still up under a line saying
this version cannot do it, is the pair disagreeing — and they can only disagree if something
classifies the refusal twice.

**Nothing is written down.** The withdrawal lives in the DOM of the tab that saw the refusal. The
core is not asked to record it and the page does not persist it.

**`pump` hides the resume offer as well as the fresh-start button.** A refused resume can put its
own offer back, so the page can now reach a state with a live resume offer *and* a live fresh-start
button. Whichever is pressed first starts a render loop, and the other must not still be there to
start a second one over the same session.

**A second press retries the session, not the enabling.** By the time the offer comes back,
`enable` has run: the camera is open and the motion permission has been answered. The retry goes
straight to `beginSession`, because asking for a camera already in hand is at best a wasted round
trip and at worst a second permission prompt on a gesture that has long since been spent.

## Consequences

The load-bearing half of this is that the withdrawal is not persisted, and it is worth stating why
plainly, because "remember the refusal" is the obvious implementation and it is wrong in exactly
the case it exists to serve.

The only thing that turns a document this build cannot read into one it can is a new version of the
app. A refusal recorded anywhere durable — `localStorage`, a flag in the project store, a field on
the summary — survives the update, and would then suppress the offer in the first build able to
honour it. The user would be permanently protected from a resume that now works. A refusal held in
the DOM cannot do that: the tab that saw it is gone by then, and the offer is rebuilt from
`hasSession` on the next load.

That means a permanently unreadable document is still offered once per load. That is the price, and
it is a small one: `Resume` reads the document before it touches anything, so a refusal costs a
document read and a sentence. It cannot start a capture and it cannot lose data. Paying it once per
load buys the property that no capture is ever hidden from the build that could finally open it.

The alternative that would have made the withdrawal durable is the one ADR 0036 ruled out for its
own reasons: `IProjectManager::List` would have to parse the session document to know which kind of
refusal a project is heading for, and it reads a key rather than a format precisely so that V3 does
not learn V1's private shape. Nothing here asks it to.

Two limits are named rather than left to be found. A refusal from the planner — a stored spec this
build no longer tessellates — arrives with whatever code the planner gave it, and only becomes an
`Unsupported` if the planner says so; it is classified by what the core said rather than by where
in `Resume` it happened. And `Unsupported` from `Adopt` is "this store has no spill tier", which is
a fact about this browser rather than about this build: the offer goes away for the tab, and comes
back on the next load, which is the right answer for a browser that might have been updated
underneath it and the same answer this rule gives everywhere else.
