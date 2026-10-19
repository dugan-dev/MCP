"""Regression checks for the strict official-result collector."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("conformance_runner", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Results(unittest.TestCase):
    def record(self, root, name, checks):
        path = root / f"{name}-2026-10-19T03-00-00-000Z" / "checks.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(checks), encoding="utf-8")

    def test_similarly_named_scenarios_are_not_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("auth/iss-supported", "auth/iss-supported-missing"):
                self.record(root, name, [{"id": "proof", "status": "SUCCESS"}])
            results = runner.inspect_results(root, "client", ["auth/iss-supported", "auth/iss-supported-missing"])
            self.assertTrue(all(item["passed"] for item in results))

    def test_missing_and_empty_results_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.record(root, "empty", [])
            results = runner.inspect_results(root, "client", ["empty", "missing"])
            self.assertFalse(any(item["passed"] for item in results))

    def test_no_vacuous_pass_from_only_skipped_or_info(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, status in (("skipped", "SKIPPED"), ("info", "INFO")):
                self.record(root, name, [{"id": "proof", "status": status}])
            results = runner.inspect_results(root, "client", ["skipped", "info"])
            self.assertFalse(any(item["passed"] for item in results))

    def test_a_single_failure_fails_the_scenario(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.record(root, "server-tools-list", [{"id": "a", "status": "SUCCESS"}, {"id": "b", "status": "FAILURE"}])
            result = runner.inspect_results(root, "server", ["tools-list"])[0]
            self.assertFalse(result["passed"])
            self.assertEqual(len(result["failures"]), 1)


if __name__ == "__main__":
    unittest.main()
