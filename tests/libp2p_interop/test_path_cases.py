import json
import copy
from dataclasses import asdict
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from path_cases import _await, _go_cancel_observation, _go_cancel_pending, _has, _read, _write
from path_evidence import native_terminal, rust_source_wave_case, rust_source_wave_failure, rust_source_wave_pending
from test_path_evidence import (TOKEN, captured_079_go_protocol_error_prefix, case_specs,
                                unit_cancellation, unit_go_retry_cancellation, unit_source_wave)


class PathCasesTests(unittest.TestCase):
    def test_control_is_atomic_and_not_a_metric_release(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "control"
            _write(path, {"sequence": "1", "action": "bind", "case-token": "0" * 32})
            self.assertEqual(path.read_text(), "sequence=1\naction=bind\ncase-token=" + "0" * 32 + "\n")
            self.assertFalse(path.with_suffix(".tmp").exists())
            for bad in ("connect\nsequence=2", "connect\x00", 1):
                with self.assertRaises(ValueError):
                    _write(path, {"action": bad})

    def test_wait_never_accepts_exited_process_or_status_without_event(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result"
            path.write_text(json.dumps({"status": "passed", "relay_echo": True, "events": []}))
            self.assertFalse(_has(_read(path), "echo", phase="relay_before"))
            owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: 0))
            with self.assertRaises(RuntimeError):
                _await(owner, path, lambda _: True, time.monotonic() + 1)

    def test_await_deadline_is_bounded(self):
        owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
        with self.assertRaises(TimeoutError):
            _await(owner, Path("missing"), lambda _: True, time.monotonic())

    def test_go_cancel_wait_rejects_captured_protocol_error_without_end(self):
        prefix = captured_079_go_protocol_error_prefix()
        peer = prefix["events"][-1]["remote_peer_id"]
        owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result"
            path.write_text(json.dumps(prefix))
            with self.assertRaisesRegex(RuntimeError, "ProtocolError completed before cancellation"):
                _await(owner, path, lambda v: _go_cancel_pending(v, prefix["case_token"], peer, "destination"),
                       time.monotonic() + 1)
        with self.assertRaisesRegex(RuntimeError, "completed before request"):
            _go_cancel_observation(prefix, prefix, prefix["case_token"], peer, "destination")

    def test_go_cancel_wait_can_observe_new_native_attempt_after_failed_end(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        record = unit_go_retry_cancellation(spec)
        pending = record["cancellation"]["before_request"]
        completed = copy.deepcopy(pending)
        first_end = next(e for e in completed["events"] if e.get("native_type") == "EndHolePunch")
        completed["events"] = completed["events"][:first_end["sequence"]]
        self.assertFalse(_go_cancel_pending(completed, TOKEN, "forge_peer", "destination"))
        self.assertTrue(native_terminal(pending, "go", "forge_peer", "failed"))
        owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result"
            path.write_text(json.dumps(completed))
            with patch("path_cases._read", side_effect=[completed, pending]):
                observed = _await(owner, path, lambda v: _go_cancel_pending(v, TOKEN, "forge_peer", "destination"),
                                  time.monotonic() + 1)
        self.assertEqual(observed, pending)
        cancellation = _go_cancel_observation(observed, copy.deepcopy(pending), TOKEN, "forge_peer", "destination")
        self.assertIs(cancellation["before_request"], cancellation["wire_before_request"])
        self.assertEqual(cancellation["before_request"], pending)

    def test_go_cancel_wait_does_not_accept_connect_or_labels_as_native_activity(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        prefix = unit_cancellation(spec)["cancellation"]["before_request"]
        prefix["events"] = prefix["events"][:-1]
        prefix.update(status="passed", observed_active_phase=True)
        self.assertFalse(_go_cancel_pending(prefix, TOKEN, "forge_peer", "destination"))

    def test_go_cancel_refresh_rejects_completed_or_replaced_observed_attempt(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        record = unit_cancellation(spec)
        observed = record["cancellation"]["before_request"]
        current = copy.deepcopy(observed)
        end = copy.deepcopy(next(e for e in record["raw"]["destination"]["result"]["events"]
                                 if e.get("native_type") == "EndHolePunch"))
        end.update(sequence=len(current["events"]) + 1, mono_ns=current["events"][-1]["mono_ns"] + 1)
        current["events"].append(end)
        replacement = unit_go_retry_cancellation(spec)["cancellation"]["before_request"]
        for prefix in (current, replacement):
            with self.subTest(state=prefix["events"][-1]["native_type"]):
                with self.assertRaisesRegex(RuntimeError, "changed or completed before request"):
                    _go_cancel_observation(observed, prefix, TOKEN, "forge_peer", "destination")

    def test_wave_runner_scope_is_only_the_two_rust_source_negative_cases(self):
        specs = [s for s in case_specs() if rust_source_wave_case(asdict(s))]
        self.assertEqual({(s.source, s.destination, s.outcome) for s in specs},
                         {("rust", "forge", "failed"), ("rust", "forge", "cancelled")})
        for key, wrong in (("source", "go"), ("source", "forge"), ("destination", "rust"),
                           ("transport", "tcp"), ("profile", "private_network"), ("outcome", "success")):
            value = asdict(specs[0]); value[key] = wrong
            self.assertFalse(rust_source_wave_case(value))

    def test_wait_uses_actual_full_pending_wave_and_separate_typed_failure(self):
        spec = next(s for s in case_specs() if s.source == "rust" and s.outcome == "cancelled")
        record = unit_source_wave(spec)
        peer = record["raw"]["destination"]["ready"]["peer_id"]
        pending, failed = record["cancellation"]["wire_before_request"], record["native_wave_failure"]
        owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result"
            path.write_text(json.dumps(pending))
            captured = _await(owner, path, lambda v: rust_source_wave_pending(v, TOKEN, peer), time.monotonic() + 1)
            self.assertEqual(captured, pending)
            self.assertFalse(rust_source_wave_failure(captured, TOKEN, peer))
            path.write_text(json.dumps(failed))
            captured = _await(owner, path, lambda v: rust_source_wave_failure(v, TOKEN, peer), time.monotonic() + 1)
            self.assertEqual(captured, failed)
            self.assertFalse(native_terminal(captured, "rust", peer, "failed"))
            self.assertFalse(native_terminal(captured, "rust", peer, "success"))
            self.assertFalse(captured["joined"])
            self.assertFalse(captured["finalized"])

    def test_wait_rejects_raw_error_before_failure_summary_and_host_finalization(self):
        spec = next(s for s in case_specs() if s.source == "rust" and s.outcome == "cancelled")
        record = unit_source_wave(spec)
        peer = record["raw"]["destination"]["ready"]["peer_id"]
        owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
        raw_error = copy.deepcopy(record["native_wave_failure"])
        raw_error["events"] = raw_error["events"][:-1]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result"
            for value in (raw_error, record["native_wave_failure"]):
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    _await(owner, path, lambda v: rust_source_wave_pending(v, TOKEN, peer), time.monotonic() + 1)
            for key in ("joined", "finalized"):
                value = copy.deepcopy(record["native_wave_failure"])
                value[key] = True
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    _await(owner, path, lambda v: rust_source_wave_failure(v, TOKEN, peer), time.monotonic() + 1)

    def test_pending_wait_requires_native_options_not_just_connect_or_status(self):
        spec = next(s for s in case_specs() if s.source == "rust" and s.outcome == "cancelled")
        record = unit_source_wave(spec)
        peer = record["raw"]["destination"]["ready"]["peer_id"]
        partial = copy.deepcopy(record["cancellation"]["wire_before_request"])
        options = next(i for i, e in enumerate(partial["events"]) if e["kind"] == "native_dcutr_dial_options")
        partial["events"] = partial["events"][:options]
        self.assertFalse(rust_source_wave_pending(partial, TOKEN, peer))
        self.assertFalse(rust_source_wave_failure(partial, TOKEN, peer))
        with self.assertRaises(ValueError):
            rust_source_wave_failure({"status": "passed", "native_wave_failure": True, "events": []}, TOKEN, peer)


if __name__ == "__main__":
    unittest.main()
