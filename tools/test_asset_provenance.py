#!/usr/bin/env python3
"""Tests for the asset provenance check.

The failure this exists to catch is silent by construction: a binary file lands in the repository,
nobody can say where it came from, and nothing goes red. Months later it is either unremovable or
unlicensed, and the only person who knew is gone. So the cases below are about the two ways a
record stops describing the bytes — a file nobody recorded, and a file that has changed since
somebody did.
"""
import hashlib
import json
import subprocess
import sys
import tempfile
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
        self.assets = root / "assets"
        self.assets.mkdir(parents=True)
        self.write("photo.bin", content)
        entry = dict(ENTRY)
        entry["sha256"] = asset_provenance.digest(self.assets / "photo.bin")
        entry["bytes"] = len(content)
        self.record({"assets": [entry]})

    def write(self, name: str, content: bytes) -> Path:
        path = self.assets / name
        path.write_bytes(content)
        return path

    def record(self, document: dict) -> None:
        (self.assets / "sources.json").write_text(json.dumps(document, indent=2))

    def entries(self) -> list[dict]:
        return json.loads((self.assets / "sources.json").read_text())["assets"]

    def problems(self) -> list[str]:
        return [str(problem) for problem in asset_provenance.check(self.root)]


class AssetProvenance(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tree = Tree(Path(self.directory.name))

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
        for field, value, because in (
                ("produced_by", "", "is blank"),
                ("produced_by", False, "answers a question a reader asks in words"),
                ("produced_by", [], "is a list of nothing"),
                ("projection", {}, "answers a question a reader asks in words"),
                ("width", True, "width is a whole number of pixels"),
                ("height", "512", "height is a whole number of pixels"),
                ("notes", [""], "is a list of nothing"),
                ("notes", ["fine", 7], "answers a question a reader asks in words")):
            with self.subTest(field=field, value=value):
                entries = self.tree.entries()
                entries[0][field] = value
                self.tree.record({"assets": entries})
                named = [p for p in self.tree.problems() if f"`{field}`" in p]
                self.assertTrue(named, f"nothing reported `{field}` at all")
                self.assertTrue(any(because in p for p in named),
                                f"`{field}` was reported, but not for being unusable: {named}")

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
    """The two loops that read a file a block at a time, driven across more than one block.

    `BLOCK` is a megabyte, and nothing either suite writes is a megabyte — so both loops ran exactly
    once in every test, and three separate mistakes were invisible: a decoder that restarts per
    block, a decoder never flushed at end of file, and a digest that hashes the first block and
    stops. Each leaves 41 tests green and the checker at exit 0.

    So the block size is lowered here rather than the files made huge. The property under test is
    "more than one block", not "many megabytes", and a fixture that spends a second writing 2 MiB to
    assert it is a fixture nobody runs.
    """

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
