#!/usr/bin/env python3
"""Regression tests for process cleanup and experiment validity, using test fixtures."""

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from experiment_utils import read_ready_line, stop_server, trial_succeeded
from run_experiments import matrix, trial


def valid_record():
    # Synthetic records are test fixtures, never benchmark evidence.
    return {
        "client_returncode": 0,
        "server_returncode": 0,
        "server_summary": {"event": "summary", "errors": 0},
        "measurement": {
            "status": "ok",
            "completed_messages": 2,
            "rtt_samples": 2,
            "errors": 0,
            "timeouts": 0,
        },
    }


class ExperimentTests(unittest.TestCase):
    def test_requires_completed_samples_and_server_summary(self):
        record = valid_record()
        self.assertTrue(trial_succeeded(record))
        for field in ("completed_messages", "rtt_samples"):
            invalid = copy.deepcopy(record)
            invalid["measurement"][field] = 0
            self.assertFalse(trial_succeeded(invalid))
        del record["server_summary"]
        self.assertFalse(trial_succeeded(record))

    def test_error_paths_are_not_successful(self):
        for updates in (
            {"client_returncode": 2},
            {"server_returncode": -9},
            {"runner_error": "test timeout"},
            {"measurement": None},
        ):
            self.assertFalse(trial_succeeded(valid_record() | updates))

    def test_partial_readiness_line_has_a_deadline(self):
        process = subprocess.Popen(
            [
                sys.executable,
                "-u",
                "-c",
                "import time; print('partial', end='', flush=True); time.sleep(10)",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            with self.assertRaisesRegex(RuntimeError, "readiness timeout"):
                read_ready_line(process, timeout=0.1)
        finally:
            stop_server(process)
        self.assertIsNotNone(process.poll())

    def test_readiness_preserves_following_output(self):
        process = subprocess.Popen(
            [sys.executable, "-u", "-c", "print('ready'); print('summary')"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertEqual(read_ready_line(process), "ready\n")
            process.wait(timeout=2)
        finally:
            result = stop_server(process)
        self.assertEqual(result["server_stdout"], "summary\n")

    def test_missing_executable_becomes_failed_record(self):
        with tempfile.TemporaryDirectory() as directory:
            record = trial(Path(directory), {"protocol": "tcp"}, 0.1, 100, 1)
        self.assertIn("runner_error", record)
        self.assertFalse(trial_succeeded(record))

    def test_matrix_sizes_and_bounds(self):
        expected = {"baseline": 19, "comparison": 24, "survey": 28, "confirmation": 8}
        for profile, count in expected.items():
            cases = matrix(profile)
            self.assertEqual(len(cases), count)
            self.assertTrue(all(1 <= config["clients"] <= 32 for _, config in cases))


class AnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import analyze
        except ModuleNotFoundError as error:
            if error.name == "matplotlib":
                raise unittest.SkipTest("analysis tests require matplotlib") from error
            raise
        cls.analysis = analyze

    def row(self, **changes):
        return {
            "valid": True,
            "profile": "comparison",
            "dataset": "fixture",
            "group": "optimization",
            "config": {
                "duration": 0.6,
                "rate": 10000,
                "mbps": 100,
                "messages": 100000,
                "timeout_ms": 500,
                "clients": 1,
                "io": "blocking",
                "sndbuf": 0,
                "rcvbuf": 0,
                "payload": 64,
                "nodelay": 0,
                "write": "split",
                "buffering": "allocate",
                **changes,
            },
        }

    def test_different_workloads_cannot_be_pooled(self):
        for changes in ({"duration": 3}, {"mbps": 200}, {"rate": 5000}, {"clients": 4}):
            with self.assertRaisesRegex(ValueError, "cannot compare"):
                self.analysis.validate_plot_controls([self.row(), self.row(**changes)])

    def test_optimization_factors_can_vary(self):
        self.analysis.validate_plot_controls(
            [
                self.row(),
                self.row(write="coalesced", buffering="reuse", nodelay=1, payload=1048576),
            ]
        )

    def test_failed_records_remain_in_aggregate_counts(self):
        row = self.row()
        summary = self.analysis.aggregate([row | {"valid": False}])
        self.assertEqual(summary[0]["trials"], 1)
        self.assertEqual(summary[0]["failed_trials"], 1)
        self.assertEqual(summary[0]["metrics"], {})

    def test_failed_confirmation_variant_can_still_be_plotted(self):
        row = self.row() | {"profile": "confirmation", "rtt_mean_us": 10}
        with tempfile.TemporaryDirectory() as directory:
            self.analysis.plot_confirmation([row], Path(directory))
            self.assertTrue((Path(directory) / "confirmation.png").is_file())

    def test_output_tables_count_runner_failures(self):
        row = self.row() | {"valid": False}
        with tempfile.TemporaryDirectory() as directory:
            counts = self.analysis.write_tables([row], Path(directory))
            saved = json.loads((Path(directory) / "counts.json").read_text())
        self.assertEqual(saved, counts)
        self.assertEqual(counts["fixture"], {"total": 1, "failed": 1})


if __name__ == "__main__":
    unittest.main()
