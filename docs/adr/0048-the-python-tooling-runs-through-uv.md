# 0048 — The Python tooling runs through uv

**Status:** accepted

## Context

Everything under `tools/` is standard library only, deliberately: the checkers run on every CI job,
and a dependency there is a dependency on every build. That has held, and it should keep holding for
the checkers.

It did not make the tooling reproducible, because it said nothing about the interpreter. `tools/`
was invoked as `python3`, which is whatever the machine happens to have; `ci.yml` pinned 3.11 in one
job with `actions/setup-python`; the `wasm` job ran `python3 tools/size_budget.py` with no Python
setup at all and relied on the runner having one; and nothing tied any of that to a developer's
machine. Three answers to one question, and the third was an accident.

The question stops being theoretical in Phase 2. The synthetic-dataset generator is the first tool
here that will genuinely need a third-party package, and "how do we add a dependency" has had no
answer that was not `pip install` into an ambient interpreter.

## Decision

**The Python tooling runs through `uv`.** `pyproject.toml` declares the project and
`requires-python`, `uv.lock` is committed, and every invocation is `uv run tools/…` — in
`tools/gate.sh`, in `ci.yml` and in `deploy.yml`. CI installs it with `astral-sh/setup-uv` in each
job that runs the tooling, replacing the one `setup-python` and the two jobs that had nothing.

Dependencies are added with `uv add`, which writes both files. Not `pip install`, and not
`uv pip install` — those reach an ambient environment and leave the lock file describing something
else.

`tools/gate.sh` checks for `uv` up front and says so once, rather than letting fourteen checkers fail
one at a time with a confusing message.

## Consequences

- The interpreter version is a fact about the repository — carried by **`.python-version`**, which is
  the file that actually pins it. `requires-python = ">=3.11"` is a floor and `uv.lock` constrains
  packages, not the interpreter: a reviewer deleted `.python-version` and uv cheerfully used 3.12.
  This ADR credited the wrong two files at first. Every CI job that runs the tooling now installs uv,
  including the `wasm` job that previously had no Python setup at all and was passing by luck.

- **The lock is enforced, not merely committed.** `uv run` silently rewrites a lock that disagrees
  with `pyproject.toml` and exits 0, so a dependency added without committing the regenerated lock
  would have gone green while CI resolved whatever the index served — the exact failure the lock
  exists to prevent. Every invocation is `uv run --locked`, which exits 2 instead. There is nothing
  to resolve today (`dependencies = []`), which is why it was cheap to close before there was.
- Adding a dependency is now a two-file diff a reviewer can read, rather than an instruction in a
  README that a machine may or may not have followed.
- The checkers stay standard-library-only. That is a separate rule and this ADR does not relax it;
  what changes is that breaking it would now be visible in a lock file.
- Cost: `uv` becomes a prerequisite for running the gate, and `.venv/` appears in the working tree
  (already ignored). The first `uv run` on a machine resolves and caches an interpreter. That
  prerequisite was discoverable only by running the gate and reading the failure, which is a poor
  way to learn it; the README now lists it with the other three.

## Rejected

***`actions/setup-python` everywhere, plus a `requirements.txt`.*** It would have fixed the two jobs
with no Python setup, and it still leaves the developer's machine out of the arrangement — nothing
in the repository would say which interpreter a contributor should be running, and `pip install -r`
into an ambient environment is not a lock file.

***A container image for the tooling.*** Genuinely reproducible and far heavier than the problem: the
checkers are stdlib scripts that run in milliseconds, and putting a Docker pull in front of
`tools/gate.sh` would make the fast half of the gate the slow half.

***Leaving it alone.*** Defensible right up until the dataset generator needs numpy, at which point
the choice gets made in a hurry inside a commit that is about something else. It is cheaper to make
it now, against tooling that has no dependencies to migrate.
