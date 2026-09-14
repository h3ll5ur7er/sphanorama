#!/usr/bin/env python3
"""Tests for the asset provenance check.

The failure this exists to catch is silent by construction: a binary file lands in the repository,
nobody can say where it came from, and nothing goes red. Months later it is either unremovable or
unlicensed, and the only person who knew is gone. So the cases below are about the two ways a
record stops describing the bytes — a file nobody recorded, and a file that has changed since
somebody did.
"""
import ast
import hashlib
import inspect
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import asset_provenance  # noqa: E402

# Bytes that are not valid UTF-8, spelled with a continuation byte that can never start a
# sequence. The first version of this file used `b"\x7fELF not text"` and similar — thirteen ASCII
# bytes, which decode cleanly, so every test that thought it was writing a binary was writing text
# and the rules under test were never reached.
NOT_TEXT = b"\x89PNG\r\n\x1a\n\xff\xfe\x00pixels, notionally"

ENTRY = {
    "file": "photo.bin",
    "sha256": "",
    "bytes": 0,
    "work": "A Work",
    "author": "Somebody",
    "licence": "CC0-1.0",
    "licence_url": "https://creativecommons.org/publicdomain/zero/1.0/",
    "source_repository": "https://github.com/example/example",
    "source_path": "assets/photo.bin",
    "retrieved": "2026-01-01",
}


class Tree:
    """A git repository holding one asset and the record that describes it.

    Real rather than faked, for the reason the conflict-marker check's fixture is: what counts
    as a file in the repository is .gitignore's answer, and a fixture that reimplemented that
    rule would be asserting against a copy of it rather than against the rule.
    """

    def __init__(self, root: Path, content: bytes = NOT_TEXT):
        self.root = root
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        # A repository, and a repository has a licence. Written because records here defer to it —
        # `"licence": "same as this repository"` — and a deferral that points at nothing is refused.
        # Six tests in this file started failing when that rule arrived, every one of them for the
        # right reason: their fixture was a repository with no licence in it.
        #
        # Added, not merely written. The deferral asks what a reader's checkout has, and for nine
        # tests this file was answering it from a file git had never been told about — so every
        # green deferral here was green because the rule was the wrong one. Adding it in the
        # fixture makes those nine assert the rule they name; the one test about an unadded licence
        # takes it back out of the index.
        (root / "LICENSE").write_text("MIT, for the purposes of this fixture.\n")
        self.track("LICENSE")
        self.assets = root / "assets"
        self.assets.mkdir(parents=True)
        self.write("photo.bin", content)
        entry = dict(ENTRY)
        entry["sha256"] = asset_provenance.digest(self.assets / "photo.bin")
        entry["bytes"] = len(content)
        self.record({"assets": [entry]})

    def track(self, name: str) -> None:
        """Put a path in the index, which is what a fresh checkout of this tree would contain."""
        subprocess.run(["git", "add", "--", name], cwd=self.root, check=True)

    def forget(self, name: str) -> None:
        """Take a path out of the index and off the disk, so no reading of "present" finds it."""
        subprocess.run(["git", "rm", "-q", "--cached", "--", name], cwd=self.root, check=True)
        (self.root / name).unlink()

    def write(self, name: str, content: bytes) -> Path:
        path = self.assets / name
        path.write_bytes(content)
        return path

    def record(self, document: dict) -> None:
        (self.assets / "sources.json").write_text(json.dumps(document, indent=2))
        # Added, like the licence. A record is a claim about the committed tree, so an untracked one
        # accounts for nothing in anybody's checkout — and until the rule for that existed, every
        # test in this file was asserting against a record git had never been told about. The second
        # time today the fixture turned out to be the thing hiding the rule.
        self.track("assets/sources.json")

    def entries(self) -> list[dict]:
        return json.loads((self.assets / "sources.json").read_text())["assets"]

    def problems(self) -> list[str]:
        return [str(problem) for problem in asset_provenance.check(self.root)]


class AssetProvenance(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tree = Tree(Path(self.directory.name))
        # Captured here, not read back per call: `record_ours` rewrites the record with an `ours`
        # half and no `assets` one, so a second read of it finds no `assets` key at all. A subtest
        # loop makes exactly that call.
        self.base = {field: self.tree.entries()[0][field]
                     for field in ("file", "sha256", "bytes", "author")}

    def test_a_directory_whose_every_file_is_recorded_is_clean(self):
        self.assertEqual(self.tree.problems(), [])

    def test_a_file_nobody_recorded_is_reported(self):
        self.tree.write("stowaway.bin", NOT_TEXT + b"\xfe where did this come from")
        self.assertIn("stowaway.bin", " ".join(self.tree.problems()))

    def test_a_file_that_changed_since_it_was_recorded_is_reported(self):
        # The point of recording a digest rather than a filename. A swapped file keeps the name,
        # the licence and the URL of the one that was cleared, and reads as provenance it does not
        # have.
        self.tree.write("photo.bin", NOT_TEXT + b"\xfe different pixels entirely")
        self.assertIn("sha256", " ".join(self.tree.problems()))

    def test_a_recorded_size_that_no_longer_matches_is_reported(self):
        entries = self.tree.entries()
        entries[0]["bytes"] = entries[0]["bytes"] + 1
        self.tree.record({"assets": entries})
        self.assertIn("bytes", " ".join(self.tree.problems()))

    def test_an_entry_naming_no_file_is_reported(self):
        entries = self.tree.entries()
        entries.append(dict(entries[0], file="gone.bin"))
        self.tree.record({"assets": entries})
        self.assertIn("gone.bin", " ".join(self.tree.problems()))

    def test_two_entries_for_one_file_are_reported(self):
        # Two records for one file means one of them is describing something else, and which is
        # which cannot be recovered from the file.
        entries = self.tree.entries()
        self.tree.record({"assets": entries + [dict(entries[0], licence="MIT")]})
        self.assertIn("twice", " ".join(self.tree.problems()))

    # Spelled out rather than read from `asset_provenance.REQUIRED`, which was the obvious fix and
    # is not one: a loop over the constant shrinks with it, so deleting a field from the schema
    # leaves the suite green — measured, not assumed. Three fields were unpinned before this list
    # existed, `work`, `licence_url` and `source_path` among them.
    FIELDS = ("sha256", "bytes", "work", "author", "licence", "licence_url", "source_repository",
              "source_path", "retrieved")

    # The same, for our own work. Two fields are cheap to eyeball today and a third added later
    # would be unpinned exactly the way three of `REQUIRED`'s were.
    OURS_FIELDS = ("sha256", "bytes", "author", "licence")

    def test_the_required_schema_is_the_one_these_tests_pin(self):
        # `file` is not in either tuple because an entry without it is refused earlier, by name.
        self.assertEqual(sorted(asset_provenance.REQUIRED), sorted(("file",) + self.FIELDS))
        self.assertEqual(sorted(asset_provenance.REQUIRED_OURS),
                         sorted(("file",) + self.OURS_FIELDS))

    def test_a_missing_field_is_reported(self):
        honest = self.tree.entries()[0]
        for field in self.FIELDS:
            with self.subTest(field=field):
                # A fresh entry each time: deleting from the record the previous subtest wrote
                # accumulates, and the first field whose absence is refused early would take every
                # later subtest down with it.
                entry = dict(honest)
                del entry[field]
                self.tree.record({"assets": [entry]})
                # The sentence, not the field name: deleting `sha256` or `bytes` also produces the
                # digest and size complaints, each of which names the field, so asserting the name
                # alone let those two subtests pass with the required-field loop skipping them.
                self.assertIn(f"`{field}` is missing", " ".join(self.tree.problems()))

    def test_an_entry_with_no_file_is_reported_before_its_fields_are_read(self):
        entry = dict(self.tree.entries()[0])
        del entry["file"]
        self.tree.record({"assets": [entry]})
        self.assertIn("names no `file`", " ".join(self.tree.problems()))

    def test_a_field_that_answers_nothing_is_reported_whatever_shape_it_takes(self):
        # JSON has more empty shapes than `None` and the blank string, and the check used to see
        # only those two: a record whose every prose field was `false` produced no problems at all.
        for value in (False, True, 0, [], {}, 7):
            with self.subTest(value=value):
                entries = self.tree.entries()
                entries[0]["licence"] = value
                self.tree.record({"assets": entries})
                self.assertIn("`licence`", " ".join(self.tree.problems()))

    def test_a_volunteered_answer_is_held_to_the_same_rule_as_a_required_one(self):
        # The required list says which questions must be answered. It said nothing about whether an
        # answer volunteered to a question nobody asked has to be an answer, so every optional field
        # — `produced_by`, `width`, `projection`, `notes` — could be `false`, `[]` or `{}` and clear
        # the build. `produced_by` is the one that bites: a blank command is carried, never run, and
        # the record reads as though it had been.
        # **The reason, not just the field name.** `("projection", {})` asserted only that
        # "`projection`" appeared somewhere, and a later round added a *second* refusal that names
        # the same field — so the subtest passed with the type rule it exists for switched off. A
        # field name is not a discriminator when two rules can print it.
        #
        # **The program-read fields are driven from the tuple, not listed here.** The hand-written
        # list is what let two of them through: it stopped one rung short of `source_blob`, whose
        # list spelling then sailed past `isinstance(recorded_blob, str)` and was never compared,
        # and of `licence`, whose list spelling was never equal to the deferral sentinel. A field
        # that joins `READ_BY_A_PROGRAM` now cannot join it without a case.
        # `file` is not driven here even though it is read by a program: it has its own guard
        # earlier in `check`, which names the entry by position and skips the rest of it, so it
        # never reaches `unusable` at all. `test_an_entry_that_names_no_file_is_reported` covers it.
        rows = [(field, value, "read by a program, so it is a single string")
                for field in asset_provenance.READ_BY_A_PROGRAM if field != "file"
                for value in (["a line", "another"], [], False)]
        # Each case starts from the pristine record, so the table is order-independent. It is not
        # load-bearing today — remove it and all 54 still pass — because the one field that made
        # the record structurally invalid when damaged, `file`, is excluded above. It stays because
        # the table is generated from a tuple that will grow, and the failure it prevents is the
        # quiet kind: the first case to break the record silences every later one while reporting
        # as a pass. A reviewer measured that it is currently inert, which is worth saying rather
        # than implying otherwise.
        pristine = json.dumps({"assets": self.tree.entries()})
        for field, value, because in rows + [
                ("produced_by", "", "is blank"),
                ("width", True, "width is a whole number of pixels"),
                ("height", "512", "height is a whole number of pixels"),
                ("notes", [""], "is a list of nothing"),
                ("notes", ["fine", 7], "answers a question a reader asks in words")]:
            with self.subTest(field=field, value=value):
                entries = json.loads(pristine)["assets"]
                entries[0][field] = value
                self.tree.record({"assets": entries})
                named = [p for p in self.tree.problems() if f"`{field}`" in p]
                self.assertTrue(named, f"nothing reported `{field}` at all")
                self.assertTrue(any(because in p for p in named),
                                f"`{field}` was reported, but not for being unusable: {named}")

    def record_ours(self, **fields) -> None:
        """Record the tree's one file as this repository's own work.

        `ours` and not `assets`, which is the fixture's default. The deferral is about *our* work,
        and an `assets` entry carries a `licence_url` whatever its `licence` says — so a rule about
        licences with nothing behind them cannot be exercised on one. Two tests for the near-miss
        rule were written on the default half, where the rule they name never ran.
        """
        self.tree.record({"ours": [dict(self.base, **fields)]})

    def defer(self):
        """Make the tree's one record defer its licence to the repository, as our own work does."""
        self.record_ours(licence=asset_provenance.DEFERS_TO_THIS_REPOSITORY)

    def test_a_licence_that_is_tracked_and_gone_is_reported_rather_than_raised(self):
        # `git ls-files --cached` answers about the index, not the disk. A LICENSE that is tracked
        # and then deleted made the checker raise `FileNotFoundError` — a traceback instead of a
        # sentence, which is the outcome its docstring exists to prevent. It was unreachable from
        # the other fixtures when this was written, none of them running `git add`; the fixture
        # tracks its licence and its record now, so what makes this case its own is the deletion.
        self.defer()
        subprocess.run(["git", "add", "LICENSE"], cwd=self.tree.root, check=True)
        (self.tree.root / "LICENSE").unlink()
        named = [p for p in self.tree.problems() if "`licence`" in p]
        self.assertTrue(named, "a tracked-but-deleted licence answered the deferral")

    def test_a_deferral_with_an_unusual_space_in_it_still_has_to_resolve(self):
        # Round 12 normalised case and surrounding space and left the inside alone, so one
        # non-breaking space — invisible in every editor — made the string not the sentinel, and it
        # cleared a repository with no licence file at all.
        # And the invisible characters that are not whitespace at all. `split()` splits on
        # `str.isspace()`, so round 13's fix reached every space and no format character: one
        # U+200B inside the sentinel made it not the sentinel, and the six records this repository
        # actually ships cleared a tree with no LICENSE in it. Same rule as `legible()` now.
        for spelling in ("same as this\u00a0repository", "same\u2009as this repository",
                         "  Same\u202fAs This Repository  ", "same  as  this  repository",
                         "same as this\u200b repository", "same as\u2060 this repository",
                         "\ufeffsame as this repository", "same\u00ad as this repository",
                         "same\u200b \u200cas this\u200d repository",
                         # And the categories a hand-written list of them left out: a control
                         # character, a DEL, a lone surrogate and a private-use codepoint each made
                         # this not the sentinel, which is the zero-width space again one category
                         # over — found in the commit that was written to end that.
                         "same as this\u0001 repository", "same as this\u007f repository",
                         "same as this\udce9 repository", "same as this\ue000 repository"):
            with self.subTest(spelling=spelling):
                entries = self.tree.entries()
                entries[0]["licence"] = spelling
                self.tree.record({"assets": entries})
                (self.tree.root / "LICENSE").unlink()
                named = [p for p in self.tree.problems() if "`licence`" in p]
                (self.tree.root / "LICENSE").write_text("MIT\n")
                self.assertTrue(named, f"{spelling!r} cleared a repository with no licence")

    def test_an_invisible_character_that_joins_two_words_does_not_make_a_deferral(self):
        # `visible` deletes rather than substitutes, and this is what that choice decides. A reader
        # of `"same as this\u200brepository"` sees `same as thisrepository` — no gap, because the
        # character is zero-width — so the checker must read it the same way. Substituting a space
        # would make the sentinel match anything with the right letters and a separator anywhere,
        # which is the loose direction that lets a record clear the build.
        entries = self.tree.entries()
        entries[0]["licence"] = "same as this\u200brepository"
        self.tree.record({"assets": entries})
        (self.tree.root / "LICENSE").unlink()
        self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [],
                         "a joined-up word was read as the deferral")

    def test_a_field_answered_with_something_invisible_is_blank(self):
        # `strip()` removes whitespace; the format characters are category `Cf` and are not
        # whitespace, so a field answered with one zero-width space was a non-blank string to every
        # check here and blank to every human who would open the file.
        # The fillers and the braille blank are not `Cf` — they are `Lo` and `So` — so the
        # category rule alone let `"work": "\u3164"` answer a required field. There is no closed
        # set of characters that render blank, which is why `BLANK` is a short named tuple and the
        # docstring claims what the rule does rather than what a font does.
        for invisible in ("\u200b", "\u200d", "\u2060", "\ufeff", " \u200b \ufeff ",
                          "\u3164", "\u115f", "\u1160", "\uffa0", "\u2800", " \u2800\u3164 ",
                          "\u0001", "\u007f", "\udce9", "\ue000", " \u0001 \ue000 "):
            with self.subTest(invisible=repr(invisible)):
                entries = self.tree.entries()
                entries[0]["work"] = invisible
                self.tree.record({"assets": entries})
                named = [p for p in self.tree.problems() if "`work`" in p]
                self.assertTrue(named, f"{invisible!r} answered a required field")
                self.assertTrue(any("is blank" in p for p in named), named)

    def test_a_raster_under_a_name_nobody_recognises_is_still_recorded(self):
        # The bounded half of the extension rule, pinned because a reviewer's finding turns on which
        # half it is. A PNG committed as `frame.dat` escapes the *shape* rule — nothing asks it for
        # `width` and `height` — but it cannot escape being recorded, because `why_asset` reads the
        # bytes rather than the name. Under-described, not unaccounted for.
        (self.tree.assets / "frame.dat").write_bytes(NOT_TEXT)
        self.tree.track("assets/frame.dat")
        problems = self.tree.problems()
        self.assertTrue(any("frame.dat" in p for p in problems), problems)
        self.assertTrue(any("not valid UTF-8" in p for p in problems), problems)

    def test_a_file_git_lists_but_this_filesystem_cannot_answer_about_is_a_sentence(self):
        # Three call sites ask `is_file()` on a path git listed — the entry walk and the two orphan
        # sweeps — and round 15 wrapped one of them. A name past `NAME_MAX` can be put in the index
        # with `update-index --cacheinfo`, at which point the two unwrapped ones raised
        # `OSError: [Errno 36]` out of `check()`. The refusal names the path, which is the whole of
        # what this module promises.
        # **All three, because naming three and driving one is this file's standing defect.** Where
        # the path sits decides which call site sees it: inside a recorded directory it reaches the
        # inner sweep, outside every record the outer one, and named by a record the entry walk.
        # Putting it under `assets/` only, as the first version did, left the other two guards
        # deletable with the suite green.
        blob = subprocess.run(["git", "hash-object", "-w", "--stdin"], cwd=self.tree.root,
                              input=b"x", capture_output=True, check=True).stdout.decode().strip()
        long_name = f"{'n' * 300}.bin"
        for where in (f"assets/{long_name}", long_name):
            with self.subTest(sweep=where):
                subprocess.run(["git", "update-index", "--add", "--cacheinfo",
                                f"100644,{blob},{where}"], cwd=self.tree.root, check=True)
                try:
                    problems = self.tree.problems()
                    self.assertTrue(any("cannot be read" in p for p in problems), problems)
                finally:
                    # Unconditionally, because a subtest that fails still has to leave the index as
                    # it found it: without this, one failure here put the long name in front of
                    # every later case and reported three failures for one defect.
                    subprocess.run(["git", "update-index", "--force-remove", where],
                                   cwd=self.tree.root, check=True)
        with self.subTest(sweep="named by a record"):
            entries = self.tree.entries()
            entries[0]["file"] = long_name
            self.tree.record({"assets": entries})
            problems = self.tree.problems()
            self.assertTrue(any("cannot be asked about" in p for p in problems), problems)

    def test_a_file_that_leaves_the_records_own_directory_is_refused(self):
        # `directory / name` accepts `../…` and an absolute path, and `is_file()` and `digest()`
        # follow symlinks — so a record could account for a file it does not own, one outside the
        # repository, or one git ignores, and the checker would report that file's digest and call
        # the directory accounted for.
        outside = self.tree.root / "elsewhere.bin"
        outside.write_bytes(NOT_TEXT)
        (self.tree.assets / "linked.bin").symlink_to(outside)
        for name in ("../elsewhere.bin", str(outside), "linked.bin", "sub/../../elsewhere.bin"):
            with self.subTest(name=name):
                entry = dict(self.tree.entries()[0])
                entry["file"] = name
                self.tree.record({"assets": [entry]})
                named = [p for p in self.tree.problems() if "`file`" in p]
                self.assertTrue(named, f"{name!r} was accepted as this record's own file")

    def test_a_deferral_spelled_with_different_capitals_still_has_to_resolve(self):
        # The rule was an exact string match, so `"Same as this repository"` — one capital — meant
        # nothing to it and cleared a repository with no licence file at all. A sentinel that one
        # keystroke defeats is not a sentinel. `projection` has the same shape and answers it with
        # a closed set; a licence cannot have one, so the deferral is recognised loosely instead.
        # **Asserting the sentence and not the field name**, because two other rules print
        # `` `licence` `` — the near-miss hint and the "exact words or a `licence_url`" refusal —
        # so making `defers_to_this_repository` case-sensitive left this green: the capitalised
        # deferral was refused, just for a different reason than the one under test. A field name
        # is not a discriminator when three rules can print it, which is the lesson the
        # volunteered-answer table learnt one round earlier and this test did not inherit.
        for spelling in ("Same as this repository", "SAME AS THIS REPOSITORY",
                         "  same as this repository  ", "Same As This Repository"):
            with self.subTest(spelling=spelling):
                self.record_ours(licence=spelling)
                self.tree.forget("LICENSE")
                named = [p for p in self.tree.problems() if "`licence`" in p]
                self.assertTrue(any("no licence file for it to mean" in p for p in named),
                                f"{spelling!r} was not read as the deferral: {named}")
                (self.tree.root / "LICENSE").write_text("MIT\n")
                self.tree.track("LICENSE")

    def test_a_capitalised_deferral_resolves_against_a_licence_that_is_there(self):
        # The other half, and the one nothing covered: making the sentinel case-sensitive should
        # also break a capitalised deferral in a tree that *has* a licence. Without this, the rule
        # could be narrowed to an exact match and only the negative test above would notice — and
        # it was green under exactly that change.
        for spelling in ("Same as this repository", "SAME AS THIS REPOSITORY"):
            with self.subTest(spelling=spelling):
                self.record_ours(licence=spelling)
                self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [])

    def test_a_record_that_is_only_on_disk_accounts_for_nothing_in_a_checkout(self):
        # `records` asks `tracked_files`, so a record written and not added is still read — which is
        # right, since waiting for `git add` to notice it would mean reporting after the push. What
        # was wrong is that it also *cleared* the assets beside it: green here, red in a fresh clone
        # of the same commit, which is the split `indexed_files` was introduced to close for the
        # licence turning up again at the record itself.
        subprocess.run(["git", "rm", "--cached", "-q", "--", "assets/sources.json"],
                       cwd=self.tree.root, check=True)
        problems = self.tree.problems()
        self.assertTrue(any("is not in the index" in p for p in problems), problems)
        # And the asset is *not* additionally reported as unrecorded, which is the failure mode of
        # the other fix — skipping the record would send a reader to write one that already exists.
        self.assertFalse(any("no directory with a sources.json" in p for p in problems), problems)

    def test_a_licence_file_that_is_only_on_disk_does_not_answer_the_deferral(self):
        # `tracked_files` is `--cached --others --exclude-standard`: every path git would *let* you
        # commit, which includes one nobody has added. So "tracked" here meant "not gitignored",
        # and a LICENSE that exists only in the working tree answered the deferral on the machine
        # that wrote it and not on the one that checks it out — verbatim the failure the comment
        # beside this rule says it closed. Every green deferral test in this file was answered by
        # an untracked licence until this arrived, the fixture never having run `git add`.
        self.defer()
        self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [],
                         "the fixture's tracked licence does not answer the deferral")
        subprocess.run(["git", "rm", "--cached", "-q", "--", "LICENSE"],
                       cwd=self.tree.root, check=True)
        self.assertTrue((self.tree.root / "LICENSE").is_file(), "the file itself must stay")
        named = [p for p in self.tree.problems() if "`licence`" in p]
        self.assertTrue(named, "a licence git has never been told about answered a deferral")

    def test_a_file_that_cannot_be_read_is_reported_rather_than_raised(self):
        # Round 14 guarded the call that names the file and left the three that open it bare, so a
        # mode-000 asset came out of `check()` as a `PermissionError` traceback — the one answer
        # this module's docstring says it will not give.
        #
        # Driven by making each call raise rather than by `chmod`, because the checker's own CI runs
        # as root and root reads a mode-000 file: a permissions fixture here would skip in the one
        # place the guard has to hold, which is a test that cannot fail wearing a skip. What the
        # guard promises is that an `OSError` out of these calls becomes a sentence, and where the
        # error came from is not part of that promise.
        #
        # **Both openers, and that is the point.** The first version patched `digest` alone, which
        # is the *first* call in the guarded block — so moving `git_blob` back outside the `try`,
        # verbatim the round-14 shape this guard was written to end, left all 77 tests green. A
        # guard over a block needs an arrow at its far end, not only at its near one.
        #
        # `stat` is deliberately not a third case, and the reason is worth having: it is the same
        # syscall as the `is_file()` in the `file` guard further up, so anything that makes it fail
        # is caught there first and reported as "cannot be asked about". It cannot be driven from
        # here without patching so broadly that the orphan sweeps answer instead. That also means
        # the real-world case — a mode-000 file — never reaches it: `chmod 000` permits `stat` and
        # refuses `open`, so the two calls that matter are the two tested.
        def refuse(*args, **kwargs):
            raise PermissionError(13, "Permission denied")

        # `source_blob` so `git_blob` is actually reached: it is computed only where a record
        # carries one, which is the shipped shape of the panorama's record and of nothing else here.
        entries = self.tree.entries()
        entries[0]["source_blob"] = asset_provenance.git_blob(self.tree.assets / "photo.bin")
        self.tree.record({"assets": entries})

        for target, patch in (("digest", lambda: self.patch_module("digest", refuse)),
                              ("git_blob", lambda: self.patch_module("git_blob", refuse))):
            with self.subTest(raised_by=target):
                undo = patch()
                try:
                    problems = self.tree.problems()
                finally:
                    undo()
                named = [p for p in problems if "[photo.bin]" in p]
                # `[photo.bin]` — the entry walk's `where`, not the orphan sweep's. Both are guarded
                # now, and a bare "cannot be read" assertion would be satisfied by either, which is
                # the same "passes for a reason you did not intend" this whole test exists to stop.
                self.assertTrue(any("cannot be read: Permission denied" in p for p in named),
                                problems)
                # And the entry is abandoned rather than carried on with, so nothing downstream
                # compares a digest or a size that was never computed.
                self.assertFalse(any("sha256" in p or "bytes and holds" in p for p in problems),
                                 problems)

    def patch_module(self, name, replacement):
        """Swap a module-level function for the duration of one subtest, and hand back the undo."""
        held = getattr(asset_provenance, name)
        setattr(asset_provenance, name, replacement)
        return lambda: setattr(asset_provenance, name, held)

    def test_a_file_too_long_for_the_filesystem_is_reported_rather_than_raised(self):
        # `Path.is_symlink()` swallows `ENOENT`, `ENOTDIR`, `EBADF` and `ELOOP` and nothing else, so
        # a `file` whose last component is at least 256 bytes raised `OSError: [Errno 36]` out of
        # `check()` — a traceback instead of a sentence, about a record a reader could have fixed.
        # 255 is the limit on every filesystem this runs on; the guard is written against the
        # error rather than the number, because the number is a mount option.
        # Each length asserts the sentence it actually earns. `"a" * 32` appears in the `where` of
        # every refusal about this entry, so it could not tell the guard's answer from any other —
        # it was green with the message replaced by "names a path outside the record's own
        # directory". And 255 is *legal*: it never reaches the guard at all, which is the point of
        # having it here, so it must not assert the guard's words either.
        for length, expected in ((255, "names a file that is not here"),
                                 (256, "cannot be asked about"),
                                 (4096, "cannot be asked about")):
            with self.subTest(length=length):
                entries = self.tree.entries()
                entries[0]["file"] = "a" * length
                self.tree.record({"assets": entries})
                problems = self.tree.problems()
                self.assertTrue(any(expected in problem for problem in problems), problems)

    def test_our_own_work_without_a_licence_url_is_refused_whatever_its_licence_says(self):
        # The sentinel absorbed exactly one spelling, so every near-miss meant the deferral to a
        # reader and a licence name to the checker — and a licence name needs nothing to exist.
        # These five cleared a tree with no LICENSE in it.
        # **Named for what it holds, which is not what it was named for.** Every spelling here is
        # refused by the one field `record_ours` omits, and `"MIT"` earns the identical sentence —
        # so "refused rather than read as a name" was a property nothing in the loop distinguished.
        # The hint is tested as a predicate instead, below.
        #
        # The ten stay, because they guard the thing no other test does: widening
        # `DEFERS_TO_THIS_REPOSITORY` to swallow one of them makes that spelling a deferral, which
        # skips this rule entirely and resolves against the fixture's tracked `LICENSE` — so the
        # subtest goes green-to-red the moment somebody tries the widening that four rounds tried.
        # That is why the list is ten phrases and not one.
        for spelling in ("Same as this repo", "same as this repository.", "as in this repository",
                         "same licence as this repo, see LICENSE", "This repository's licence",
                         "same as the repository", "same as this project", "see LICENSE",
                         "same as the top-level LICENSE", "this project's licence"):
            with self.subTest(spelling=spelling):
                self.record_ours(licence=spelling)
                named = [p for p in self.tree.problems() if "`licence`" in p]
                self.assertTrue(named, f"{spelling!r} was read as a licence name")
                self.assertTrue(any("names a licence with a `licence_url`" in p for p in named),
                                named)

    def test_the_spellings_that_name_no_repo_are_the_ones_the_hint_cannot_guess(self):
        # The hint is allowed to be incomplete and the guard is not, so the line between them is
        # worth pinning: without it, widening `NEARLY_DEFERS` until it covered everything would look
        # like progress rather than the open-ended rule it was demoted for being.
        for spelling in ("Same as this repo", "same licence as this repo, see LICENSE"):
            with self.subTest(guessed=spelling):
                self.record_ours(licence=spelling)
                self.assertTrue(any("reads as the deferral" in p for p in self.tree.problems()))
        for spelling in ("same as the repository", "same as this project", "see LICENSE",
                         "same as the top-level LICENSE", "this project's licence"):
            with self.subTest(unguessed=spelling):
                self.record_ours(licence=spelling)
                problems = self.tree.problems()
                self.assertTrue(any("`licence`" in p for p in problems), problems)
                self.assertFalse(any("reads as the deferral" in p for p in problems), problems)

    def test_a_licence_url_spelled_as_a_list_does_not_satisfy_the_ours_rule(self):
        # `licence_url` became read-by-a-program the moment `check` began branching on it, and the
        # tuple and the rule were edited by different hands — so a list of prose satisfied the prose
        # rule and cleared the very rule the field was added to serve. Fourth field to do this.
        for url in (["nonsense", "not a url"], [], False, "", "\u200b", "\u034f"):
            with self.subTest(url=url):
                self.record_ours(licence="Proprietary, all rights reserved", licence_url=url)
                # The `ours` rule's own sentence. The filter here was ``"`licence" in p`` — no
                # closing backtick — which the *generic* volunteered-answer refusal satisfies, so
                # widening the rule to `entry.get("licence_url") is None` (the hole this test's own
                # example is about) left the suite green. Whatever `unusable` says about the field,
                # what must happen is that our own work is refused for having nothing behind it.
                named = [p for p in self.tree.problems()
                         if "names a licence with a `licence_url`" in p]
                self.assertTrue(named, f"a licence_url of {url!r} answered for our own work")

    def test_the_hint_names_a_deferral_and_not_an_ordinary_licence(self):
        # The hint tested as a predicate, because through `check` it cannot be: it only ever appends
        # to a refusal that a `licence_url` suppresses, so a test that supplies one never consults
        # it — `test_..._is_not_a_near_miss` was green with this function replaced by `return True`.
        # A rule reachable only under a condition the test removes is a rule the test cannot see.
        for spelling in ("Same as this repo", "same licence as this repo, see LICENSE"):
            with self.subTest(names_this_repository=spelling):
                self.assertTrue(asset_provenance.nearly_defers_to_this_repository(spelling))
        for spelling in ("MIT", "CC-BY-4.0", "Apache-2.0, see the upstream NOTICE",
                         "same as the upstream project", "same as this repository"):
            with self.subTest(does_not=spelling):
                self.assertFalse(asset_provenance.nearly_defers_to_this_repository(spelling))

    def test_our_own_work_naming_a_licence_with_a_url_is_accepted(self):
        # The second arm of the `ours` rule, which is the only way to be accepted without the exact
        # sentinel. A rule that refuses ordinary answers would be worse than the hole it closed.
        for spelling in ("MIT", "CC-BY-4.0", "Apache-2.0, see the upstream NOTICE",
                         "CC0-1.0 (public domain dedication)", "same as the upstream project"):
            with self.subTest(spelling=spelling):
                self.record_ours(licence=spelling,
                                 licence_url="https://spdx.org/licenses/MIT.html")
                self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [])

    def test_a_gitignored_licence_does_not_answer_the_deferral(self):
        # A gitignored *and unadded* LICENSE, which is one arrangement of the rule above rather
        # than a second rule — and saying so is the point of keeping it.
        #
        # This was written believing `--exclude-standard` was what refused it. It is not:
        # `LS_FILES_INDEXED` has no `--exclude-standard`, because that flag applies to the untracked
        # half and there is no `--others` here for it to reach. A gitignored file somebody added
        # anyway is in the index and will be in the clone, which is exactly what this listing asks.
        # So the `.gitignore` below changes nothing, and a reader who "matched the other listing" by
        # adding `--exclude-standard` would be loosening the rule rather than tightening it.
        self.defer()
        subprocess.run(["git", "rm", "--cached", "-q", "--", "LICENSE"],
                       cwd=self.tree.root, check=True)
        (self.tree.root / ".gitignore").write_text("LICENSE\n")
        self.tree.track(".gitignore")
        named = [p for p in self.tree.problems() if "`licence`" in p]
        self.assertTrue(any("no licence file for it to mean" in p for p in named), named)
        # And the `.gitignore` is not what did it: adding the file back answers the deferral while
        # git is still ignoring it. `-f`, because git refuses to add an ignored path without it —
        # which is the whole distinction, since a `-f` add is exactly how such a file ends up in
        # somebody's clone. Without this assertion the case reads as covering a rule that does not
        # exist.
        subprocess.run(["git", "add", "-f", "--", "LICENSE"], cwd=self.tree.root, check=True)
        self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [])

    def test_a_refused_open_leaves_no_descriptor_behind(self):
        # `open_regular` holds a bare descriptor between `os.open` and `os.fdopen`, and every way
        # out of that window but the happy one is an exception — a FIFO, a directory, `fdopen`
        # itself failing. Without the `except BaseException: os.close(handle)` the integer leaks,
        # and a leak is invisible: the suite stays green and the checker runs out of descriptors
        # only on a tree big enough to matter.
        #
        # Counted rather than warned about, because `ResourceWarning` is not raised for a bare
        # descriptor — nothing owns it to have a finaliser.
        def open_descriptors():
            return len(os.listdir("/proc/self/fd"))

        fifo = self.tree.root / "pipe"
        os.mkfifo(fifo)
        self.addCleanup(fifo.unlink)
        before = open_descriptors()
        for _ in range(64):
            with self.assertRaises(OSError):
                asset_provenance.open_regular(fifo)
        self.assertLessEqual(open_descriptors(), before,
                             "a refused open left its descriptor behind")

    def test_a_licence_or_a_record_that_is_a_fifo_does_not_hang_the_build(self):
        # **A ceiling on the read does not bound the `open`.** A FIFO blocks in the kernel until a
        # writer appears, so `mkfifo LICENSE` made this checker never return — the bound added to
        # stop unbounded work, defeated by the call before the one it bounds. Both paths come from
        # the git index, which says what was committed and nothing about what is on disk now.
        #
        # The alarm is the assertion: a hang has no other symptom, and a test that waits for one is
        # the only kind that can fail on it.
        # Deferring first, so the licence is a question this run actually asks.
        self.defer()
        for where, expected in (("LICENSE", "cannot be read"),
                                ("assets/sources.json", "could not be read as JSON")):
            with self.subTest(fifo=where):
                target = self.tree.root / where
                target.unlink()
                os.mkfifo(target)
                self.addCleanup(lambda t=target: t.exists() and t.unlink())
                signal.signal(signal.SIGALRM, self.impatient)
                signal.alarm(10)
                try:
                    problems = self.tree.problems()
                finally:
                    signal.alarm(0)
                self.assertTrue(any(expected in p for p in problems), problems)

    @staticmethod
    def impatient(number, frame):
        raise AssertionError("the checker did not return: a fifo is blocking it in open()")

    def test_one_unreadable_spelling_does_not_hide_a_good_one(self):
        # Hoisting the licence question out of the entry loop changed its answer, which is the kind
        # of thing hoisting is supposed not to do: the `any(...)` it replaced swallowed an `OSError`
        # and tried the next spelling, and returning on the first one stopped the search. A
        # `LICENSE` that is a FIFO beside a perfectly good tracked `COPYING` was refused.
        #
        # Both orders, because the rule is about the set and not about which file comes first in
        # `LICENCE_FILES`.
        self.defer()
        for broken, good in (("LICENSE", "COPYING"), ("COPYING", "LICENSE")):
            with self.subTest(broken=broken):
                for spelling in ("LICENSE", "COPYING"):
                    written = self.tree.root / spelling
                    if written.exists():
                        written.unlink()
                # Tracked as ordinary files and *then* replaced on disk, because `git add` refuses
                # a FIFO — which is the real arrangement anyway: the index says what was committed
                # and the working tree is what the checker opens.
                for spelling in (good, broken):
                    (self.tree.root / spelling).write_text("MIT, and readable.\n")
                    self.tree.track(spelling)
                (self.tree.root / broken).unlink()
                os.mkfifo(self.tree.root / broken)
                try:
                    self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [],
                                     f"an unreadable {broken} hid a good {good}")
                finally:
                    (self.tree.root / broken).unlink()

    def test_a_licence_that_cannot_be_read_is_not_reported_as_one_that_is_not_there(self):
        # "This repository has no licence file" about a file that is right there sends a reader to
        # write one that exists. `says_something` swallowed the `OSError` and returned False, which
        # is the reason-losing choice `why_asset` refuses to make in the same file.
        # The licence only. `read_record` opens through the same helper, so refusing everything
        # makes the record unreadable and the entry loop never runs — the refusal under test then
        # cannot be reached, and `named` comes back empty for the wrong reason.
        held = asset_provenance.open_regular

        def refuse(path):
            if path.name == "LICENSE":
                raise PermissionError(13, "Permission denied")
            return held(path)

        asset_provenance.open_regular = refuse
        self.addCleanup(setattr, asset_provenance, "open_regular", held)
        self.defer()
        named = [p for p in self.tree.problems() if "`licence`" in p]
        self.assertTrue(any("LICENSE cannot be read: Permission denied" in p for p in named), named)
        self.assertFalse(any("no licence file for it to mean" in p for p in named), named)

    def test_a_record_too_large_to_be_a_record_is_refused_rather_than_read(self):
        # The last unbounded read in this file. A 400 MB `sources.json` took peak memory to 779 MiB,
        # and one symlinked to `/dev/zero` never returned — a build that hangs rather than fails.
        # Refused by length and not streamed, because unlike an asset a record has no honest large
        # form: `json.loads` needs the whole document anyway.
        held = asset_provenance.RECORD_CEILING
        asset_provenance.RECORD_CEILING = 64
        self.addCleanup(setattr, asset_provenance, "RECORD_CEILING", held)
        (self.tree.assets / "sources.json").write_text(" " * 80 + "{}")
        problems = self.tree.problems()
        self.assertTrue(any("is not a record" in p for p in problems), problems)

    def test_a_record_nested_past_what_json_will_parse_is_a_sentence(self):
        # `json.loads` raises `RecursionError` on deeply nested input, and that is a `RuntimeError` —
        # so it walked past an arm naming only `OSError` and `ValueError`, straight out of `check()`.
        (self.tree.assets / "sources.json").write_text("[" * 2000)
        problems = self.tree.problems()
        self.assertTrue(any("could not be read as JSON" in p for p in problems), problems)

    def test_a_half_that_is_not_a_list_is_reported_rather_than_crashing_its_consumer(self):
        # `{"ours": {...}}` keyed by filename reads as a record and is not one. The checker used to
        # say nothing about it and `tools/test_synth_dataset.py` died on the shape with a bare
        # `TypeError` naming a line of test code — round 10's defect one level above the fields it
        # was about.
        for half, held in (("assets", {"photo.bin": {}}), ("ours", {"photo.bin": {}}),
                           ("assets", "photo.bin"), ("ours", 7)):
            with self.subTest(half=half, held=held):
                self.tree.record({half: held})
                problems = self.tree.problems()
                self.assertTrue(any(f"`{half}`" in p and "list of entries" in p for p in problems),
                                problems)

    def test_a_licence_that_defers_to_a_repository_with_no_licence_is_refused(self):
        # `"licence": "same as this repository"` is the right way to write our own work down: it is
        # a derivation, and a derivation cannot drift the way nine copies of "MIT" can. What it has
        # to do is resolve, and for the whole life of these records it did not — there was no
        # LICENSE file at all, and nothing said so.
        self.defer()
        self.tree.forget("LICENSE")
        named = [p for p in self.tree.problems() if "`licence`" in p]
        self.assertTrue(named, "a licence deferring to a repository with no licence was accepted")
        self.assertTrue(any("no licence file for it to mean" in p for p in named), named)

    def test_a_deferred_licence_is_answered_by_any_of_the_usual_spellings(self):
        # The rule is about the licence existing, not about what it is called. `COPYING` is the
        # GNU spelling and is as much an answer as `LICENSE`, and `LICENCE` is the one this module
        # uses for the field itself — a checker written in British English was telling a repository
        # that spells its file the same way that it had no licence at all. A pin asserts what is in
        # a tuple and can say nothing about what is missing from it, which is how that survived.
        self.defer()
        # The tuple itself, because a loop over it shrinks with it: `LICENCE_FILES = ("LICENSE",)`
        # left all of these green with three spellings asserted by nothing.
        self.assertEqual(asset_provenance.LICENCE_FILES,
                         ("LICENSE", "LICENSE.md", "LICENSE.txt", "LICENCE", "LICENCE.md",
                          "LICENCE.txt", "COPYING"))
        self.tree.forget("LICENSE")
        for spelling in asset_provenance.LICENCE_FILES:
            with self.subTest(spelling=spelling):
                written = self.tree.root / spelling
                written.write_text("A licence, of some kind.\n")
                self.tree.track(spelling)
                self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [])
                self.tree.forget(spelling)

    def test_a_deferred_licence_is_not_answered_by_a_file_that_says_nothing(self):
        # A zero-byte LICENSE is the shape a half-finished `touch` leaves behind, and it answers the
        # question no better than no file at all. `st_size > 0` was the whole rule, so the four
        # below it — a newline, spaces, a zero-width space, a combining mark — answered it: a
        # pointer resolving to a file with nothing in it resolves to nothing.
        self.defer()
        for content in ("", "\n", "   ", "\u200b", "\u034f", "\n\n  \t\n"):
            with self.subTest(content=content):
                (self.tree.root / "LICENSE").write_text(content)
                self.assertTrue([p for p in self.tree.problems() if "`licence`" in p],
                                f"a licence of {content!r} answered a deferral")
        (self.tree.root / "LICENSE").write_text("MIT\n")
        self.assertEqual([p for p in self.tree.problems() if "`licence`" in p], [])

    def test_a_source_blob_that_is_not_these_bytes_is_refused(self):
        # The upstream git object name was the one recorded fact nothing derived, which matters
        # because it goes stale on exactly the change ADR 0059 forbids: re-encode the file and
        # `sha256` and `bytes` both shout, while this one quietly went on naming an object whose
        # contents are no longer here.
        entries = self.tree.entries()
        entries[0]["source_blob"] = "0" * 40
        self.tree.record({"assets": entries})
        named = [p for p in self.tree.problems() if "`source_blob`" in p]
        self.assertTrue(named, "a blob hash naming different bytes was accepted")
        # What it must *not* say is the local object name. Pasting that in converts an upstream
        # fact into a local one that can never be checked against anything again — the only repair
        # this field's refusal can suggest that is always wrong.
        self.assertTrue(any("re-record `source_blob` from upstream" in p for p in named), named)
        self.assertFalse(any(asset_provenance.git_blob(self.tree.assets / "photo.bin") in p
                             for p in named), named)

    def test_the_bytes_are_reported_before_the_object_name_that_follows_from_them(self):
        # All three refusals fire for one cause — changed bytes — and the reader acts on the first
        # one they meet. `sha256` and `bytes` both invite the paste that repairs them; this one
        # never does, so it goes last.
        entries = self.tree.entries()
        entries[0]["source_blob"] = "0" * 40
        self.tree.record({"assets": entries})
        (self.tree.assets / "photo.bin").write_bytes(NOT_TEXT + b"more")
        problems = self.tree.problems()
        said = [i for i, p in enumerate(problems) if "sha256" in p or "bytes and holds" in p]
        blob = [i for i, p in enumerate(problems) if "`source_blob`" in p]
        self.assertTrue(said and blob, problems)
        self.assertLess(max(said), min(blob), problems)

    def test_a_digest_recorded_in_upper_case_is_not_called_a_change_in_the_bytes(self):
        # A hex digest has no meaningful case, so "has moved on from its recorded sha256" sends a
        # reader to re-hash a file that was never wrong. The record is still refused — one spelling,
        # so nothing downstream needs a rule about case — but for what is actually wrong with it.
        entries = self.tree.entries()
        entries[0]["sha256"] = entries[0]["sha256"].upper()
        self.tree.record({"assets": entries})
        named = [p for p in self.tree.problems() if "sha256" in p]
        self.assertTrue(named, "a digest in the wrong case was accepted")
        self.assertTrue(any("wrong case" in p for p in named), named)
        self.assertFalse(any("has moved on" in p for p in named), named)

    def test_a_raster_nought_pixels_wide_is_not_a_raster(self):
        # `value < 0` admitted zero, so `"width": 0, "height": 0` cleared the checker. `width` and
        # `height` are the two facts only a decoder can confirm, which is the reason the arm that
        # needs no decoder must not be the looser of the two.
        for field in ("width", "height"):
            with self.subTest(field=field):
                entries = self.tree.entries()
                entries[0]["file"] = "photo.png"
                (self.tree.assets / "photo.png").write_bytes((self.tree.assets / "photo.bin")
                                                             .read_bytes())
                entries[0]["width"] = 4
                entries[0]["height"] = 4
                entries[0][field] = 0
                self.tree.record({"assets": entries})
                named = [p for p in self.tree.problems() if f"`{field}`" in p]
                self.assertTrue(named, f"`{field}: 0` was accepted")
                self.assertTrue(any("at least one" in p for p in named), named)

    def test_a_source_blob_git_itself_computes_is_accepted(self):
        # Against `git hash-object` rather than against this checker's own arithmetic, which would
        # be asserting that the function agrees with itself.
        blob = subprocess.run(["git", "hash-object", str(self.tree.assets / "photo.bin")],
                              cwd=self.tree.root, check=True, capture_output=True,
                              text=True).stdout.strip()
        entries = self.tree.entries()
        entries[0]["source_blob"] = blob
        self.tree.record({"assets": entries})
        self.assertEqual([p for p in self.tree.problems() if "`source_blob`" in p], [])

    def test_a_command_spelled_as_a_list_of_lines_is_refused(self):
        # The prose rule accepts a list of lines, which is how every long answer in this tree is
        # written and is right for all of them but this one: `produced_by` is *run*, and
        # `tools/test_synth_dataset.py` puts it in a set. Spelled as a list it cleared the checker
        # and died there with `TypeError: unhashable type: 'list'` — a checker that says nothing
        # and a test that crashes instead of reporting.
        entries = self.tree.entries()
        entries[0]["produced_by"] = ["uv run --group datasets tools/synth_dataset.py",
                                     "  --out core/test/data/synthetic-ring-4"]
        self.tree.record({"assets": entries})
        named = [p for p in self.tree.problems() if "`produced_by`" in p]
        self.assertTrue(named, "a command written as two lines was accepted")
        self.assertTrue(any("read by a program, so it is a single string" in p for p in named),
                        named)

    def test_every_media_extension_is_shaped_or_says_why_not(self):
        # `SHAPED` used to be a second hand-written list beside `MEDIA`, and it drifted: `.avif`,
        # `.ico`, `.hdr` and `.exr` were rasters in one and not the other, and `.tif` was in
        # neither — so an entry for a `.avif` could record any dimensions it liked, or none.
        # No "in exactly one of the two" loop here. `SHAPED` is *derived* as `MEDIA - UNSHAPED`, so
        # that property holds by construction and asserting it is a tautology — which is what stood
        # here, in the test written to catch tautologies.
        #
        # **`MEDIA` is pinned as well, and that is the half the last round missed.** An extension
        # leaves `SHAPED` two ways: by joining `UNSHAPED`, which the pin below catches, or by
        # leaving `MEDIA`, which nothing caught. Deleting `.ppm` from `MEDIA` and stripping
        # `width`/`height` from the four committed frame records left the checker at exit 0 and all
        # of these green — `.jpg` is protected by the projection assertion downstream and a frame
        # claims no projection, so nothing else was watching.
        self.assertEqual(sorted(asset_provenance.MEDIA), sorted((
            ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".bmp", ".ico", ".tiff", ".tif",
            ".svg", ".ppm", ".pgm", ".pnm", ".hdr", ".exr", ".mp3", ".wav", ".ogg", ".flac",
            ".mp4", ".webm", ".mov", ".ttf", ".otf", ".woff", ".woff2", ".pdf", ".stl", ".obj",
            ".glb", ".gltf", ".blend", ".psd", ".zip")),
            "an extension left or joined MEDIA, which decides both what counts as an asset at all "
            "and — through SHAPED — whether an entry for one must record its pixel shape")
        for suffix in asset_provenance.UNSHAPED:
            self.assertIn(suffix, asset_provenance.MEDIA,
                          f"{suffix} is exempted from a rule it was never subject to")
        # **Which side, not only that there is a side.** Totality alone left the hand list free to
        # move: adding `.ppm` to `UNSHAPED` and deleting `width`/`height` from the four committed
        # frame records left every test here and in the dataset suite green. Whether an extension
        # is a raster is not derivable from anything in this repository — there is no decoder here
        # by design — so it is pinned by hand, which is what `REQUIRED` does for the same reason.
        self.assertEqual(sorted(asset_provenance.UNSHAPED), sorted((
            ".svg", ".mp3", ".wav", ".ogg", ".flac", ".mp4", ".webm", ".mov", ".ttf", ".otf",
            ".woff", ".woff2", ".pdf", ".stl", ".obj", ".glb", ".gltf", ".blend", ".psd", ".zip")),
            "an extension changed sides, and only a decoder could say whether it should have")

    def test_a_raster_that_used_to_escape_the_shape_rule_no_longer_does(self):
        # The concrete half of the test above, through the checker rather than the tuples: an
        # `.avif` with no `width` was accepted before, because `.avif` was in `MEDIA` and not in
        # `SHAPED`.
        content = b"\x00\x01\x02not text at all\xff"
        self.tree.write("photo.avif", content)
        entry = dict(self.tree.entries()[0])
        entry["file"] = "photo.avif"
        entry["sha256"] = asset_provenance.digest(self.tree.assets / "photo.avif")
        entry["bytes"] = len(content)
        entry.pop("width", None)
        entry.pop("height", None)
        self.tree.record({"assets": [entry]})
        problems = self.tree.problems()
        self.assertTrue(any("`width`" in p for p in problems), problems)

    def test_a_projection_nobody_recognises_is_refused(self):
        # `projection` is read by a test rather than by a person — `tools/test_synth_dataset.py`
        # asserts the 2:1 rule on an entry claiming to be equirectangular — so it has to be a token
        # from a closed set. The rule was added after a record spelled it as a sentence
        # ("equirectangular, 360 by 180 degrees") and silently disabled that assertion for every
        # record in the tree; it went in with no test of its own, which is how it was found.
        for value in ("equirectangular, 360 by 180 degrees", "Equirectangular", "cubemap", ""):
            with self.subTest(value=value):
                entries = self.tree.entries()
                entries[0]["projection"] = value
                self.tree.record({"assets": entries})
                reported = " ".join(self.tree.problems())
                self.assertIn("`projection`", reported)
                self.assertIn("the projections this repository knows", reported)

    def test_the_projection_this_repository_does_know_is_accepted(self):
        # The guard on the rule above: a closed set that refused its own only member would fail the
        # tree it ships with, and the tree is the thing it is meant to let through.
        entries = self.tree.entries()
        entries[0]["projection"] = "equirectangular"
        self.tree.record({"assets": entries})
        self.assertEqual(self.tree.problems(), [])

    def test_a_raster_has_to_say_what_shape_it_is(self):
        # `width` and `height` were optional, so the one fact this checker cannot verify itself —
        # it has no decoder — could be deleted from a record with nothing going red. The suite that
        # does verify them, over in `tools/test_synth_dataset.py` where Pillow is present, counted
        # how many it had checked and asserted only that the count was not zero; with one shaped
        # entry in the tree that number is 1 whether the fact is there or not.
        #
        # Required by extension, because deciding what is a raster from the bytes would put an
        # image library in front of every build, which is the thing this checker refuses to be.
        entry = dict(self.tree.entries()[0])
        entry["file"] = "photo.png"
        self.tree.write("photo.png", NOT_TEXT)
        entry["sha256"] = asset_provenance.digest(self.tree.assets / "photo.png")
        entry["bytes"] = len(NOT_TEXT)
        self.tree.record({"assets": [entry, self.tree.entries()[0]]})
        reported = " ".join(self.tree.problems())
        self.assertIn("`width`", reported)
        self.assertIn("`height`", reported)

    def test_a_file_that_is_not_a_raster_is_not_asked_for_a_shape(self):
        # The guard on the rule above. `shell/public/icon.svg` is somebody's work and has no shape
        # a decoder could confirm, and the four committed `.ppm` frames do — so the rule has to
        # split on which, or it either exempts every asset or demands the impossible of an SVG.
        entry = dict(self.tree.entries()[0])
        entry["file"] = "drawing.svg"
        self.tree.write("drawing.svg", b"<svg xmlns='http://www.w3.org/2000/svg'/>")
        entry["sha256"] = asset_provenance.digest(self.tree.assets / "drawing.svg")
        entry["bytes"] = (self.tree.assets / "drawing.svg").stat().st_size
        self.tree.record({"assets": [entry, self.tree.entries()[0]]})
        self.assertEqual(self.tree.problems(), [])

    def test_an_answer_spelled_across_lines_is_an_answer(self):
        # The guard on the test above: `licence_evidence` and `notes` are written as a list of lines
        # in the record this checker was built for, so a rule that refused lists outright would
        # refuse the tree it ships with.
        entries = self.tree.entries()
        entries[0]["notes"] = ["the first line", "and the second"]
        entries[0]["width"] = 1024
        self.tree.record({"assets": entries})
        self.assertEqual(self.tree.problems(), [])

    def test_a_size_that_is_not_a_size_is_reported(self):
        # `True == 1` in Python, so `"bytes": true` satisfied the size comparison for a one-byte
        # file — the record agreeing with itself rather than with the bytes.
        for value in (True, "29", 1.5, -1, [29]):
            with self.subTest(value=value):
                entries = self.tree.entries()
                entries[0]["bytes"] = value
                self.tree.record({"assets": entries})
                self.assertIn("`bytes`", " ".join(self.tree.problems()))

    def test_a_field_left_blank_is_reported(self):
        # An empty string satisfies "the key is there" and answers nothing, which is the shape a
        # record takes when somebody fills in the form to make the check pass.
        entries = self.tree.entries()
        entries[0]["licence"] = "   "
        self.tree.record({"assets": entries})
        self.assertIn("licence", " ".join(self.tree.problems()))

    def test_a_file_one_level_down_is_reported_too(self):
        # A folder would otherwise be a way past the check. This asserts the filename rather than
        # the directory's: with the directory's, the test passed just as well when the rule that
        # catches it was removed, because the message names the whole path either way.
        (self.tree.assets / "more").mkdir()
        (self.tree.assets / "more" / "hidden.bin").write_bytes(NOT_TEXT + b"\xfd out of sight")
        self.assertIn("hidden.bin", " ".join(self.tree.problems()))

    def test_a_record_that_is_not_json_is_reported_rather_than_raised(self):
        # The message, not the path: every problem from this record begins with its path, so
        # asserting that pins where the complaint came from and nothing about what it said.
        (self.tree.assets / "sources.json").write_text("{ this is not json")
        self.assertIn("could not be read as JSON", " ".join(self.tree.problems()))

    def test_a_record_with_no_entries_is_reported(self):
        # Asserted on the sentence rather than on the word `assets`, which is also this fixture's
        # directory name: deleting the refusal outright left this green, because the file then fell
        # through to "photo.bin is here and is recorded nowhere" and that path says `assets` too.
        self.tree.record({"why": "explained at length, and nothing recorded"})
        self.assertIn("records nothing", " ".join(self.tree.problems()))

    def test_a_tree_with_no_records_at_all_is_clean(self):
        # Most of this repository has no assets, and a checker that had to be told where to look
        # would go stale the first time a directory moved.
        with tempfile.TemporaryDirectory() as empty:
            subprocess.run(["git", "init", "-q"], cwd=empty, check=True)
            (Path(empty) / "src").mkdir()
            (Path(empty) / "src" / "thing.txt").write_text("no assets here")
            self.assertEqual(asset_provenance.check(Path(empty)), [])


class AFileThisRepositoryMadeItself(unittest.TestCase):
    """`ours` entries: no upstream trail, but the same author, licence and digest."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tree = Tree(Path(self.directory.name))
        self.rendered = NOT_TEXT + b"\xfb output of a tool in this tree"
        self.tree.write("rendered.bin", self.rendered)
        self.ours = {
            "file": "rendered.bin",
            "sha256": hashlib.sha256(self.rendered).hexdigest(),
            "bytes": len(self.rendered),
            "author": "this repository",
            "licence": "same as this repository",
            "produced_by": "uv run tools/whatever.py",
        }

    def record(self, entry=None):
        self.tree.record({"assets": self.tree.entries(),
                          "ours": [entry if entry is not None else self.ours]})

    def test_our_own_work_needs_no_upstream_trail(self):
        self.record()
        self.assertEqual(self.tree.problems(), [])

    def test_our_own_work_still_has_to_answer_the_licence_question(self):
        # The hole this shape had in its first version: it asked only for a command, so a
        # third-party file was one sentence away from cleared. No checker can stop a false licence,
        # which is a lie; leaving the question unasked is the part a checker can refuse.
        for field in AssetProvenance.OURS_FIELDS:
            with self.subTest(field=field):
                entry = dict(self.ours)
                del entry[field]
                self.record(entry)
                self.assertIn(f"`{field}` is missing", " ".join(self.tree.problems()))

    def test_our_own_work_still_carries_a_digest(self):
        entry = dict(self.ours, sha256="0" * 64)
        self.record(entry)
        self.assertIn("sha256", " ".join(self.tree.problems()))

    def test_a_command_is_optional_because_some_of_it_is_drawn_by_hand(self):
        entry = dict(self.ours)
        del entry["produced_by"]
        self.record(entry)
        self.assertEqual(self.tree.problems(), [])

    def test_an_entry_naming_no_file_is_reported(self):
        self.record(dict(self.ours, file="absent.bin"))
        self.assertIn("absent.bin", " ".join(self.tree.problems()))

    def test_a_record_holding_only_our_own_work_is_a_record(self):
        (self.tree.assets / "photo.bin").unlink()
        self.tree.record({"ours": [self.ours]})
        self.assertEqual(self.tree.problems(), [])


class ReadingAFileInBlocks(unittest.TestCase):
    """Every loop that reads a file a block at a time, driven across more than one block.

    `BLOCK` is a megabyte, and nothing either suite writes is a megabyte — so every such loop ran
    exactly once in every test, and three separate mistakes were invisible: a decoder that restarts
    per block, a decoder never flushed at end of file, and a digest that hashes the first block and
    stops. Each leaves 41 tests green and the checker at exit 0.

    So the block size is lowered here rather than the files made huge. The property under test is
    "more than one block", not "many megabytes", and a fixture that spends a second writing 2 MiB to
    assert it is a fixture nobody runs.

    The set is named and derived rather than counted, because a count is a copy of a fact: a
    function that arrives here without a case leaves its loop able to stop after one block with
    both checkers at exit 0, and a class docstring saying "the two loops" cannot notice a third.
    `test_every_loop…` fails when the derived set stops matching `DRIVEN`, and
    `test_block_is_the_only_read_size…` holds the assumption that derivation rests on.
    """

    # Function name → the test here that drives it past one block. Checked against the module, so
    # a new block-reading function cannot arrive without either a case or a deliberate line here.
    DRIVEN = {
        "digest": "test_a_digest_covers_every_block_and_not_just_the_first",
        "git_blob": "test_a_blob_name_covers_every_block_and_not_just_the_first",
        "why_asset": "test_a_character_split_across_a_block_boundary_is_still_one_character",
        "says_something": "test_a_licence_whose_first_legible_byte_is_past_one_block_reads_as_blank",
    }

    def test_every_loop_that_reads_in_blocks_has_a_case_in_this_class(self):
        # Derived from the module rather than listed beside it: every function that *reads* the name
        # `BLOCK`, which is what "reads a file a block at a time" is spelled as here. The failure it
        # produces names the function, which is the whole of what a reader needs.
        #
        # From the parse tree, not the text. `"BLOCK" in source` is a substring test, and
        # `O_NONBLOCK` contains `BLOCK` — so a function that opens a file without reading one block
        # of it joined the set and demanded a case. A comment mentioning the constant would have
        # done the same. `ast.Name` is the question actually being asked.
        reading = {name for name, value in vars(asset_provenance).items()
                   if inspect.isfunction(value)
                   and any(isinstance(node, ast.Name) and node.id == "BLOCK"
                           for node in ast.walk(ast.parse(
                               textwrap.dedent(inspect.getsource(value)))))}
        self.assertEqual(reading, set(self.DRIVEN),
                         "a function reads in blocks with no case here, or a case names a "
                         "function that no longer does")
        for function, case in self.DRIVEN.items():
            # `hasattr` was the whole of this and it accepted an empty method: emptying the digest
            # case *and* truncating `digest` to its first block left the suite green, which is the
            # docstring's own "a digest that hashes the first block and stops". A case has to
            # *call* the function it claims to drive.
            #
            # The qualified name, because the bare one is in the case's own signature:
            # `test_a_digest_covers_every_block_and_not_just_the_first` contains "digest", so
            # `assertIn(function, source)` was a tautology for exactly the function the class
            # docstring names — the fix and the defect it fixed, in one line.
            #
            # **Run, not read.** This assertion has had three spellings — `hasattr`, then the
            # qualified name as a substring, then an `ast.Call` — and every one of them asked a
            # question about the *text* of the case. Each was defeated in the round after it: an
            # empty method, a comment, and then dead code after a `return`. A parser strictly
            # stricter than the last is still not the question, which is whether the case *calls*
            # the function when it runs.
            #
            # So the case is invoked with the function wrapped, in a fresh instance of this class so
            # its own `setUp` gives it the fixture it expects and nothing here is disturbed. `if
            # False:`, an uninvoked lambda and a call inside `assertRaises` all now fail, because
            # none of them reaches the wrapper.
            self.assertTrue(hasattr(self, case), f"{function}'s case {case} does not exist")
            self.assertTrue(self.case_calls(case, function),
                            f"{case} is named as {function}'s cover and never calls it")

    def case_calls(self, case, function):
        """Whether running `case` actually calls `asset_provenance.<function>`."""
        called = []
        held = getattr(asset_provenance, function)

        def watched(*arguments, **named):
            called.append(arguments)
            return held(*arguments, **named)

        instance = type(self)(case)
        setattr(asset_provenance, function, watched)
        try:
            instance.setUp()
            try:
                getattr(instance, case)()
            finally:
                instance.doCleanups()
        finally:
            setattr(asset_provenance, function, held)
        return bool(called)

    def test_no_function_here_reads_an_asset_other_than_in_blocks_of_block(self):
        # The hole the derivation above cannot see. It finds functions that mention `BLOCK`, so a
        # function reading in blocks through a *differently named* constant is invisible to it and
        # would arrive with no case and nothing red. Rather than guess at what such a constant
        # might be called, this pins the two things that make the derivation sound.
        #
        # **Parsed, not grepped.** The first version was a regex over the module text, which could
        # not see `read_bytes()` — so replacing `why_asset`'s incremental decoder with
        # `path.read_bytes().decode()`, the 32 MiB to 1105 MiB defect that function's own docstring
        # is about, left the suite green. It also matched read sizes quoted *in comments*, which is
        # this module's house style, so it could fail for no reason at all. `ast` sees calls.
        # Every way `io` hands bytes over, not the three that came to mind: `readlines()` was
        # missing, so replacing the block loop with `b"".join(handle.readlines())` — the 32 MiB to
        # 1105 MiB defect this class exists for — left the suite green. A list of method names is a
        # copy of `io`'s surface, which is the shape this file keeps being caught by, so the
        # iteration case below is asserted separately rather than added to it.
        reads = {"read", "read1", "readinto", "readinto1", "readline", "readlines",
                 "read_bytes", "read_text"}
        tree = ast.parse(inspect.getsource(asset_provenance))
        sized, whole = [], []
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
                continue
            if node.func.attr not in reads:
                continue
            (sized if node.args else whole).append(node)
        # Two sizes and no more: `BLOCK` for anything streamed, and the record ceiling for the one
        # read that is bounded rather than streamed. Keeping the set closed is what makes
        # "reads in blocks" and "mentions BLOCK" the same set, so the derivation above is complete.
        self.assertEqual({ast.unparse(node.args[0]) for node in sized},
                         {"BLOCK", "RECORD_CEILING + 1"},
                         [ast.unparse(node) for node in sized])
        # And **nothing here is read whole any more.** The record was the last one — `read_text()`
        # with nothing between it and the disk — which took peak memory to 779 MiB on a 400 MB
        # `sources.json` and never returned at all on one symlinked to `/dev/zero`. Every read in
        # this file now asks for a number.
        self.assertEqual([ast.unparse(node) for node in whole], [],
                         "something is read whole again")
        # And a handle is only ever *called on*, never used as a value — which is the one shape no
        # list of method names can see. `for line in handle` reads the file a line at a time with no
        # call to enumerate, and `b"".join(handle)` does it with no loop either.
        #
        # Every `with` that binds a name, not only `… .open(…)`. The first version keyed on the
        # attribute `open`, so `with open_regular(path) as handle` — which is how *both* of the
        # reads this whole exercise is about are opened — contributed nothing to the set, and the
        # check was correct only because the other two `with` statements happened to bind the same
        # name. A test right by name collision is a test waiting for a rename.
        # Per function, because a name means different things in different ones: `open_regular`
        # binds `handle` to an *integer* file descriptor and passes it to `os.fstat` and `os.close`,
        # which is exactly right and is not a read at all. A module-wide set of names would refuse
        # that, and asking each function about its own bindings is the question anyway.
        loose = []
        for value in vars(asset_provenance).values():
            if not inspect.isfunction(value):
                continue
            body = ast.parse(textwrap.dedent(inspect.getsource(value)))
            handles = {item.optional_vars.id
                       for node in ast.walk(body) if isinstance(node, ast.With)
                       for item in node.items if isinstance(item.optional_vars, ast.Name)}
            parents = {child: parent for parent in ast.walk(body)
                       for child in ast.iter_child_nodes(parent)}
            loose += [f"{value.__name__}: {ast.unparse(parents.get(node, node))}"
                      for node in ast.walk(body)
                      if isinstance(node, ast.Name) and node.id in handles
                      and isinstance(node.ctx, ast.Load)
                      and not (isinstance(parents.get(node), ast.Attribute)
                               and parents[node].attr in reads)]
        self.assertEqual(loose, [], "a file handle is used as a value rather than read from")

    def test_a_licence_whose_first_legible_byte_is_past_one_block_reads_as_blank(self):
        # `says_something` reads *one* block on purpose — a checker asked about a path from the
        # index must not be the thing that reads an arbitrary file whole — so the bound is real and
        # is pinned here rather than left to be discovered. Anything legible is in the first block
        # of a licence somebody wrote; a file that hides its first word past a megabyte of spaces is
        # not one, and reading it as blank is the answer this accepts.
        path = self.write("LICENSE", b" " * (asset_provenance.BLOCK + 4) + b"MIT")
        self.assertFalse(asset_provenance.says_something(path))
        self.assertTrue(asset_provenance.says_something(self.write("ok", b"   MIT\n")))

    def test_a_blob_name_covers_every_block_and_not_just_the_first(self):
        # Git's own answer, not ours, so the comparison is against `git hash-object` rather than
        # against a second implementation of the same header. Sizes around the boundary because the
        # mistakes here are off-by-one-block: an empty file, a block short, exactly a block, a block
        # and one byte, and several blocks with a remainder.
        block = asset_provenance.BLOCK
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        for size in (0, block - 1, block, block + 1, 3 * block + 5):
            with self.subTest(size=size):
                path = self.write("blob.bin", bytes(range(256)) * (size // 256 + 1))
                path.write_bytes(path.read_bytes()[:size])
                theirs = subprocess.run(["git", "hash-object", "--", str(path)], cwd=self.root,
                                        capture_output=True, text=True, check=True).stdout.strip()
                self.assertEqual(asset_provenance.git_blob(path), theirs)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        held = asset_provenance.BLOCK
        asset_provenance.BLOCK = 8
        self.addCleanup(setattr, asset_provenance, "BLOCK", held)

    def write(self, name: str, content: bytes) -> Path:
        path = self.root / name
        path.write_bytes(content)
        return path

    def test_a_digest_covers_every_block_and_not_just_the_first(self):
        # Hashing one block and stopping gives a digest that is stable, plausible and wrong past the
        # first 8 bytes here — a megabyte in the shipped constant. The record exists to catch a file
        # swapped for another; two files sharing a first block would swap freely.
        content = b"the first block!" + b"and everything after it" * 4
        path = self.write("long.bin", content)
        self.assertGreater(len(content), asset_provenance.BLOCK * 3, "one block would prove nothing")
        self.assertEqual(asset_provenance.digest(path), hashlib.sha256(content).hexdigest())

    def test_a_character_split_across_a_block_boundary_is_still_one_character(self):
        # The reason this cannot be `chunk.decode()` in a loop. A multi-byte character that straddles
        # the boundary decodes as two invalid halves, so a perfectly good source file is reported as
        # an asset nobody recorded — a refusal a reader cannot act on, about a file that is fine.
        for pad in range(asset_provenance.BLOCK):
            with self.subTest(pad=pad):
                content = ("a" * pad + "\u00e9" + "b" * 20).encode()
                path = self.write("text.txt", content)
                self.assertIsNone(asset_provenance.why_asset(path),
                                  f"valid UTF-8 with a character at byte {pad} was called an asset")

    def test_a_character_cut_off_at_the_end_of_the_file_is_not_valid_utf8(self):
        # The flush, and the whole of what it is for. An incremental decoder holds a partial sequence
        # waiting for the rest; without `decode(b"", final=True)` the file simply ends and nobody
        # asks, so bytes that are not UTF-8 read as source — and an unrecorded binary walks past the
        # rule this checker exists to enforce.
        path = self.write("truncated.bin", b"hello \xc3")
        why = asset_provenance.why_asset(path)
        self.assertIsNotNone(why, "a truncated multi-byte sequence was read as source")
        self.assertIn("not valid UTF-8", why)


class AFileNobodyCouldRead(unittest.TestCase):
    """What counts as an asset, which is the rule that stops the check being opted into."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.tree = Tree(self.root)

    def elsewhere(self, name: str, content: bytes) -> Path:
        (self.root / "elsewhere").mkdir(exist_ok=True)
        path = self.root / "elsewhere" / name
        path.write_bytes(content)
        return path

    def test_bytes_that_are_not_text_are_an_asset_wherever_they_are(self):
        # A name the extension rule knows nothing about, so the bytes are the only thing that can
        # report it. With a `.jpg` here — which is what this test used to write — narrowing the
        # orphan sweep to `MEDIA` alone left the suite green, and the rule round 1 was closed with
        # was asserted by nothing.
        self.elsewhere("smuggled.dat", NOT_TEXT)
        problems = " ".join(self.tree.problems())
        self.assertIn("smuggled.dat", problems)
        self.assertIn("not valid UTF-8", problems)

    def test_a_picture_that_happens_to_be_text_is_still_an_asset(self):
        # The half a bytes-only rule misses, and it was missing a real one: `shell/public/icon.svg`
        # had been in this repository unrecorded since the PWA shell landed, invisible because an
        # SVG decodes.
        self.elsewhere("logo.svg", b"<svg xmlns='http://www.w3.org/2000/svg'/>\n")
        problems = " ".join(self.tree.problems())
        self.assertIn("logo.svg", problems)
        self.assertIn("somebody's work", problems)

    def test_source_is_not_an_asset(self):
        # A repository is mostly source, and a rule that asked every `.ts` file for a licence would
        # be turned off within a week.
        self.elsewhere("code.ts", b"export const x = 1;\n")
        self.assertEqual(self.tree.problems(), [])

    def test_an_asset_git_is_ignoring_is_not_reported(self):
        # An ignored file cannot become a commit, which is the whole set this checker is about.
        (self.root / ".gitignore").write_text("build/\n")
        (self.root / "build").mkdir()
        (self.root / "build" / "artefact.o").write_bytes(NOT_TEXT)
        self.assertEqual(self.tree.problems(), [])

    def test_source_beside_a_record_is_not_the_records_business(self):
        # A record in a directory answers for the assets there, not for everything there. Without
        # this, putting a record next to an icon demands a licence for the service worker.
        (self.tree.assets / "helper.ts").write_text("export const y = 2;\n")
        self.assertEqual(self.tree.problems(), [])


class RecordsInsideRecords(unittest.TestCase):
    """The nearest record owns the file, which is what stops two of them deadlocking over it."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.tree = Tree(self.root)
        # `zoom`, not `inner`: `records()` returns paths in git's sorted order, and with a name that
        # sorts *before* `sources.json` the first match is also the nearest one, so replacing
        # `owner_of` with first-match-wins left both these tests green. A name that sorts after it
        # separates the two rules.
        self.inner = self.tree.assets / "zoom"
        self.inner.mkdir()
        self.content = NOT_TEXT + b"\xfa inner"
        (self.inner / "deep.bin").write_bytes(self.content)

    def record_inner(self, name: str = "deep.bin") -> None:
        (self.inner / "sources.json").write_text(json.dumps({"ours": [{
            "file": name,
            "sha256": hashlib.sha256(self.content).hexdigest(),
            "bytes": len(self.content),
            "author": "this repository",
            "licence": "same as this repository",
        }]}, indent=2))
        self.tree.track(self.inner.relative_to(self.tree.root).as_posix() + "/sources.json")

    def test_the_nearest_record_owns_the_file_whatever_order_they_arrive_in(self):
        # Asserted on the function rather than through a fixture, because a fixture cannot help
        # depending on the order `records()` happens to return: with a nested directory named
        # `inner`, "assets/inner/sources.json" sorts *before* "assets/sources.json", so first-match
        # and nearest-match were the same string and replacing one rule with the other left the
        # suite green. Here both orders are asserted, so neither can flatter the rule.
        for order in (["assets/", "assets/zoom/"], ["assets/zoom/", "assets/"]):
            with self.subTest(order=order):
                self.assertEqual(asset_provenance.owner_of("assets/zoom/deep.bin", order),
                                 "assets/zoom/")
        self.assertEqual(asset_provenance.owner_of("elsewhere/x.bin", ["assets/"]), None)

    def test_the_inner_record_answers_and_the_outer_one_is_not_asked(self):
        self.record_inner()
        self.assertEqual(self.tree.problems(), [])

    def test_a_file_the_inner_record_misses_is_reported_against_the_inner_one(self):
        # One problem, and it is the inner record's. Two separate `assertIn`s over the joined text
        # were satisfied by two *unrelated* problems — the outer record reporting the file, and the
        # inner entry naming one that is not there — so the attribution this test is named for went
        # unchecked.
        self.record_inner(name="something-else.bin")
        problems = self.tree.problems()
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(all(p.startswith("assets/zoom/sources.json") for p in problems), problems)
        self.assertIn("deep.bin is here and is recorded nowhere", " ".join(problems))


class ARecordOrAFileThatCannotBeRead(unittest.TestCase):
    """Every way of not being readable, because each used to arrive as a traceback or as silence."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.tree = Tree(self.root)

    def test_a_record_whose_bytes_are_not_utf8_is_reported(self):
        # `read_text` decodes before `json.loads` is reached, so this raises `UnicodeDecodeError`
        # and never becomes a `JSONDecodeError`.
        (self.tree.assets / "sources.json").write_bytes(NOT_TEXT)
        self.assertIn("could not be read as JSON", " ".join(self.tree.problems()))

    def test_a_record_that_is_valid_json_but_not_an_object_is_reported(self):
        for document in ("[]", '"a record"', "7"):
            with self.subTest(document=document):
                (self.tree.assets / "sources.json").write_text(document)
                self.assertIn("a record is an object", " ".join(self.tree.problems()))

    def test_a_file_that_cannot_be_read_is_not_thereby_source(self):
        # The one answer that cannot be right about a file nobody can read is "this is fine".
        #
        # `why_asset` is called directly rather than through `check`, which guards with `is_file()`:
        # in a whole tree this arm is reachable only in a race, and a permissions fixture would skip
        # whenever the tests run as root — which is every CI runner here. A directory reaches the
        # same `OSError` deterministically and for the same reason, which is that the bytes did not
        # arrive.
        self.assertIn("could not be read at all", asset_provenance.why_asset(self.tree.assets) or "")


class WhenGitWillNotAnswer(unittest.TestCase):
    """A tree this cannot enumerate must not read as a clean one."""

    def test_a_directory_that_is_not_a_repository_is_an_error_and_not_an_empty_answer(self):
        # Returning "no files" from a git that would not answer makes an unrunnable check
        # indistinguishable from a clean tree — and `main` would then exit 0 over a tree holding
        # unrecorded binaries. Reachable in life: an export or a checkout with no `.git`.
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "blob.bin").write_bytes(NOT_TEXT)
            with self.assertRaises(RuntimeError) as refusal:
                asset_provenance.check(Path(directory))
            self.assertIn("could not list this tree", str(refusal.exception))


class ThisRepository(unittest.TestCase):
    def test_the_committed_assets_are_recorded(self):
        root = Path(__file__).resolve().parents[1]
        problems = asset_provenance.check(root)
        self.assertEqual([str(problem) for problem in problems], [])

    def test_the_checker_finds_the_committed_record(self):
        # Without this, the test above passes just as well when the scan finds nothing at all —
        # which is how a checker that has stopped looking reads from the outside.
        root = Path(__file__).resolve().parents[1]
        self.assertIn(root / "core" / "test" / "data" / "panoramas" / "sources.json",
                      asset_provenance.records(root))


class Entrypoint(unittest.TestCase):
    def test_a_clean_tree_exits_zero_and_a_dirty_one_does_not(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Tree(Path(directory))
            self.assertEqual(asset_provenance.main(["asset_provenance", directory]), 0)
            tree.write("stowaway.bin", NOT_TEXT + b"\xfc unrecorded")
            self.assertEqual(asset_provenance.main(["asset_provenance", directory]), 1)


if __name__ == "__main__":
    unittest.main()
