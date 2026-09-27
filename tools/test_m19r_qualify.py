#!/usr/bin/env python3
"""Negative tests for candidate binding and immutable qualification emission."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from m19r_qualify import (canonical, digest, events, must_candidate,
                          observed_counts, require_passed, write_immutable_receipt)


class QualificationTests(unittest.TestCase):
    def test_candidate_mismatch_and_dirty_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            (repo / "source.txt").write_text("candidate\n")
            subprocess.run(["git", "add", "source.txt"], cwd=repo, check=True)
            subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-qm", "candidate"], cwd=repo, check=True)
            sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            with self.assertRaises(RuntimeError):
                must_candidate(repo, "0" * 40)
            with self.assertRaises(RuntimeError):
                must_candidate(repo, sha[:12])
            must_candidate(repo, sha)
            (repo / "source.txt").write_text("dirty\n")
            with self.assertRaises(RuntimeError):
                must_candidate(repo, sha)

    def test_exclusive_receipt_and_canonical_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            body = {"count": 2, "candidate": "a" * 40}
            h = digest(canonical(body))
            receipt = dict(body, receipt_digest=h)
            path = Path(tmp) / "M19R" / f"{h}.json"
            write_immutable_receipt(path, receipt)
            original = path.read_bytes()
            with self.assertRaises(FileExistsError):
                write_immutable_receipt(path, receipt)
            self.assertEqual(path.read_bytes(), original)

    def test_failed_gate_and_derived_final_count(self):
        seen = events("  [PASS] GATE_ONE\n  [FAIL] GATE_TWO\n", "suite")
        self.assertEqual(observed_counts(seen), {"completed": 2, "passed": 1, "failed": 1})
        with self.assertRaises(RuntimeError):
            require_passed(seen)
        self.assertEqual(observed_counts(seen[:1])["completed"], 1)
        require_passed(seen[:1])
        named = events("SILICON_PASS: FAIL\nOTHER_PASS: measured true\n", "named")
        self.assertEqual([event["status"] for event in named], ["FAIL", "PASS"])
        with self.assertRaises(RuntimeError):
            require_passed(named)


if __name__ == "__main__":
    unittest.main()
