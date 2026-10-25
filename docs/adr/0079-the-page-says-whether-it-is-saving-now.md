# 0079 — The page says whether it is saving now

**Status:** accepted; amends [0078](0078-a-failed-read-is-not-an-absent-session.md), whose open
item on a store that stops saving mid-session closes

## Context

ADR 0078 put "not saving projects" on the page for a document store that would not load, and
recorded what it left open: the indicator said what the boot found. A store that loads and then
fails every write, from a full disk or site data cleared mid-session, said nothing. The document
host kept the failure in `lastPersistError()` (ADR 0014), the worker sent it back in every `flush`
reply, and the page read none of them. The persist that fails is one nobody asks for, since the
host persists behind every write on a timer, so asking would not find it in time either.

`lastPersistError()` is also the wrong question. It holds on to the last failure for good, which
answers "did anything ever fail to save" and not "is this being kept now".

## Decision

- **The host tracks whether persisting is failing now.** `persistFailing()` is set when a persist
  fails and cleared when one goes through, and `onPersistChange` hears each change. A store that
  loses its database (`DocumentStore.onLost`) sets it for good, and the loss stays its reason: a
  persist that goes through after the loss, even one with nothing in it, says nothing about
  documents whose database is gone, and one that fails against it says less about why.
  `lastPersistError()` keeps its meaning, the history. After a failed load nothing is persisted,
  so nothing fails, and the load is the boot's to say as before.
- **The worker pushes each change** as a `persisting` message, and `RemoteCore.onPersistChange`
  hands it to the page, replaying the latest to a handler that registers after it. A worker that
  dies is reported as not saving, since the documents it had not persisted went with it.
- **`flush` answers the same question.** Its reply carries the load's error, or the failure of
  the persist that is failing now, and null once everything committed, where it used to carry
  `lastPersistError()`. A flush drains writes that arrive while it runs and stops only at a
  persist that failed.
- **The page says "not saving" while either is true**: the store would not load, or persisting is
  failing now. Both places it is said are redrawn on each change, the capabilities row and the
  folded panel toggle, and they say "saving" again once a persist goes through.

## Consequences

- A capture whose store fills partway through is told so at the next persist after the store
  refuses, which follows each write within the host's debounce, and is told again when it
  recovers. The writes the host could not persist are kept and retried, as before (ADR 0014).
- A store whose site data is cleared mid-session does not recover. The clear closes the database
  under the host, and the store hears the close and says so at once (`DocumentStore.onLost`),
  since a capture under review writes nothing and no persist would find out. Nothing reconnects,
  so "not saving" stays up for the rest of the page. That is deliberate: the host holds only the
  writes since its last persist, so a reconnect that sent them would say "saving" of a disk that
  has lost everything before, and sending every resident document instead would undo a clear
  that was as often the user deleting them. A browser test clears the origin's database and
  requires the indicator to appear before anything is written, and to stay.
- The stage line is not rewritten for it. It is the capture's own line while one runs, and the two
  indicators are the ones that stay up, which is why ADR 0078 put them there.
- A browser test shrinks the origin's storage quota through the DevTools protocol, so the
  database's writes are refused for real, and requires the indicator to appear and then clear.

## Rejected alternative

**Read `flush`'s reply on the page.** It is already sent, so it would cost no protocol. But `flush`
runs on `pagehide` and when the page is hidden, after the capture it would warn. When this was
decided it also carried `lastPersistError()`, which never clears; it now says what is true at the
flush, which leaves the timing as the reason.
