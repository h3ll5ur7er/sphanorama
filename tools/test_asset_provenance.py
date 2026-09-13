#!/usr/bin/env python3
"""Tests for the asset provenance check.

The failure this exists to catch is silent by construction: a binary file lands in the repository,
nobody can say where it came from, and nothing goes red. Months later it is either unremovable or
unlicensed, and the only person who knew is gone. So the cases below are about the two ways a
record stops describing the bytes — a file nobody recorded, and a file that has changed since
somebody did.
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import asset_provenance  # noqa: E402

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

    def __init__(self, root: Path, content: bytes = b"pixels, notionally"):
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
        self.tree.write("stowaway.bin", b"where did this come from")
        self.assertIn("stowaway.bin", " ".join(self.tree.problems()))

    def test_a_file_that_changed_since_it_was_recorded_is_reported(self):
        # The point of recording a digest rather than a filename. A swapped file keeps the name,
        # the licence and the URL of the one that was cleared, and reads as provenance it does not
        # have.
        self.tree.write("photo.bin", b"different pixels entirely")
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

    def test_a_missing_field_is_reported(self):
        for field in ("licence", "author", "source_repository", "retrieved"):
            with self.subTest(field=field):
                entries = self.tree.entries()
                del entries[0][field]
                self.tree.record({"assets": entries})
                self.assertIn(field, " ".join(self.tree.problems()))

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
        (self.tree.assets / "more" / "hidden.bin").write_bytes(b"out of sight")
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
            tree.write("stowaway.bin", b"unrecorded")
            self.assertEqual(asset_provenance.main(["asset_provenance", directory]), 1)


if __name__ == "__main__":
    unittest.main()
