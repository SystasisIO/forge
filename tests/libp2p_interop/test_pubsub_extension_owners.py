"""Synthetic owner-checker regressions only, not extension compatibility evidence."""

import copy
import unittest

from pubsub_evidence import _events, _owner, _rpc_peer, _terminal_owners
from pubsub_wire import validate_rpc_receipt
from test_pubsub_evidence import synthetic_case


class ExtensionOwnerTests(unittest.TestCase):
    def test_native_owner_rules_apply_to_all_supported_versions(self):
        for version in ("1.0", "1.1", "1.2", "1.3"):
            for quic in (True, False):
                with self.subTest(version=version, quic=quic):
                    case = synthetic_case(lower_quic=quic, version=version)
                    protocol = "/meshsub/" + version + ".0"
                    for name, raw in case["raw"].items():
                        events = _events(raw, raw["implementation"], case["case_token"], name)
                        _terminal_owners(raw)
                        for event in events:
                            if event["kind"] != "rpc":
                                continue
                            _owner(events, event, _rpc_peer(event), protocol, "quic" if quic else "tcp", None)
                            validate_rpc_receipt(event["receipt"], protocol, event["direction"])

    def test_new_versions_cannot_borrow_another_selected_protocol(self):
        for version in ("1.2", "1.3"):
            case = synthetic_case(version=version)
            raw = case["raw"]["replacement"]
            event = next(row for row in raw["events"] if row["kind"] == "rpc")
            for protocol in ("/meshsub/1.1.0", "/meshsub/1.4.0"):
                with self.subTest(version=version, protocol=protocol), self.assertRaises(ValueError):
                    _owner(raw["events"], event, _rpc_peer(event), protocol, "quic", None)

    def test_new_versions_retain_receipt_and_shutdown_guards(self):
        for version in ("1.2", "1.3"):
            case = synthetic_case(version=version)
            original = case["raw"]["replacement"]
            failed = copy.deepcopy(original)
            failed["joined"] = False
            with self.subTest(version=version), self.assertRaises(ValueError):
                _events(failed, "go", case["case_token"], "replacement")
            event = copy.deepcopy(next(row for row in original["events"] if row["kind"] == "rpc"))
            event["receipt"][event["direction"]]["framed_sha256"] = "0" * 64
            with self.subTest(version=version), self.assertRaises(ValueError):
                validate_rpc_receipt(event["receipt"], "/meshsub/" + version + ".0", event["direction"])

    def test_unknown_version_does_not_gain_native_operation_authority(self):
        case = synthetic_case(version="1.4")
        with self.assertRaises(ValueError):
            raw = case["raw"]["replacement"]
            _events(raw, "go", case["case_token"], "replacement")


if __name__ == "__main__":
    unittest.main()
