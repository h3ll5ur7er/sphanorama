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
                self.assertIn(field, " ".join(self.tree.problems()))

    def test_an_entry_with_no_file_is_reported_before_its_fields_are_read(self):
        entry = dict(self.tree.entries()[0])
        del entry["file"]
        self.tree.record({"assets": [entry]})
        self.assertIn("names no `file`", " ".join(self.tree.problems()))

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
        (self.tree.assets / "sources.json").write_text("{ this is not json")
        self.assertIn("sources.json", " ".join(self.tree.problems()))

    def test_a_record_with_no_assets_list_is_reported(self):
        self.tree.record({"why": "explained at length, and nothing recorded"})
        self.assertIn("assets", " ".join(self.tree.problems()))

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
                self.assertIn(field, " ".join(self.tree.problems()))

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
        self.elsewhere("smuggled.jpg", NOT_TEXT)
        self.assertIn("smuggled.jpg", " ".join(self.tree.problems()))

    def test_a_picture_that_happens_to_be_text_is_still_an_asset(self):
        # The half a bytes-only rule misses, and it was missing a real one: `shell/public/icon.svg`
        # had been in this repository unrecorded since the PWA shell landed, invisible because an
        # SVG decodes.
        self.elsewhere("logo.svg", b"<svg xmlns='http://www.w3.org/2000/svg'/>\n")
        self.assertIn("logo.svg", " ".join(self.tree.problems()))

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
        self.inner = self.tree.assets / "inner"
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

    def test_the_inner_record_answers_and_the_outer_one_is_not_asked(self):
        self.record_inner()
        self.assertEqual(self.tree.problems(), [])

    def test_a_file_the_inner_record_misses_is_reported_against_the_inner_one(self):
        self.record_inner(name="something-else.bin")
        problems = " ".join(self.tree.problems())
        self.assertIn("deep.bin", problems)
        self.assertIn("inner/sources.json", problems)


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
