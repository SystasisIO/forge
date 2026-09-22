"""Adversarial receipts, not evidence of a live exchange."""

import copy
import unittest

from mdns_evidence import PUBLIC_SERVICE, SCHEMA
from mdns_staged_evidence import churn_sequence, lifecycle_receipt, quiet_receipt


def quiet(implementation="forge"):
    ready = dict(schema=SCHEMA, status="ready", role="listener", implementation=implementation,
                 local_peer_id="peer-A", service_name=PUBLIC_SERVICE, outcome="quiet", ready_at_unix_ms=1000)
    value = dict(ready, status="ok", capture_phase="after_stop", stop_reason="stop_file",
                 cleanup_complete=True, observed_milliseconds=3200, stop_requested_at_unix_ms=4200,
                 authenticated_connections=0, echo_streams=0, application_bytes_sent=0, application_bytes_received=0)
    if implementation == "forge":
        value.update(basis="runtime_metrics_and_gater_with_discovery_snapshot",
                     service_errors="unavailable_via_public_api", mdns_observations_snapshot=0,
                     peer_dial_gate_calls=0, inbound_gate_calls=0, direct_attempts=0, handshakes_completed=0)
    else:
        value.update(basis="lifetime_notifier_and_network_notifications",
                     discovery_callback_scope="entered_callbacks_through_final_snapshot",
                     donor_callback_join="unavailable_native_detached_notifications",
                     cleanup_scope="mdns_service_host_and_admitted_echo_handlers",
                     discovery_overflow=False, discovery_callbacks=0, discovered_peers=0, dial_attempts=0)
    return value, ready


def lifecycle(epoch=1, count=1):
    return dict(schema="forge.mdns.lifecycle.v1", epoch=epoch, pid=123, local_peer_id="peer-A",
                request="snapshot", capture_phase="after_echo_before_stop", capture_error="",
                service_errors="unavailable_via_public_api", topology_phase="idle", mdns_observations=count,
                active_operations=0, memory=0, file_descriptors=2, active_dials=0, inbound_connections=0,
                outbound_connections=1, inbound_streams=0, outbound_streams=0, sessions_opened=1,
                sessions_closed=0, failed_refreshes=0)


class EvidenceTests(unittest.TestCase):
    def test_quiet_requires_explicit_success_and_exact_zero_counters(self):
        for impl in ("forge", "go"):
            value, ready = quiet(impl)
            terminal = dict(exit_code=0, termination="graceful")
            quiet_receipt(value, ready, impl, "listener", PUBLIC_SERVICE, terminal)
            mutations = dict(failure={}, error="late failure", status="error", cleanup_complete=False,
                             observed_milliseconds=2999, stop_reason="deadline", authenticated_connections=1,
                             echo_streams=True, ready_at_unix_ms=999, local_peer_id="changed")
            mutations["mdns_observations_snapshot" if impl == "forge" else "discovery_callbacks"] = 1
            for key, changed in mutations.items():
                with self.subTest(impl=impl, key=key), self.assertRaises(ValueError):
                    quiet_receipt(dict(value, **{key: changed}), ready, impl, "listener", PUBLIC_SERVICE, terminal)
            with self.assertRaises(ValueError):
                quiet_receipt(value, ready, impl, "listener", PUBLIC_SERVICE, dict(exit_code=0, termination="terminated"))

    def test_lifecycle_rejects_pid_epoch_errors_and_counter_coercion(self):
        original = lifecycle()
        lifecycle_receipt(original, epoch=1, pid=123, peer="peer-A")
        for key, value in dict(epoch=True, pid=124, local_peer_id="other", capture_error="bad",
                               failed_refreshes=1, memory=-1, mdns_observations=True,
                               capture_phase="quiet_before_stop", failure={},
                               sessions_opened=2, sessions_closed=1).items():
            with self.subTest(key=key), self.assertRaises(ValueError):
                lifecycle_receipt(dict(original, **{key: value}), epoch=1, pid=123, peer="peer-A")

    def test_chronology_requires_actual_transition_boundaries(self):
        samples = [dict(epoch=i, stage=stage, requested_at=i * 3, received_at=i * 3 + 1,
                        receipt=lifecycle(i, count))
                   for i, (stage, count) in enumerate((("initial", 1), ("down", 0), ("up", 1)), 1)]
        transitions = [dict(up=False, checked=True, began_at=4.5, finished_at=5),
                       dict(up=True, checked=True, began_at=7.5, finished_at=8)]
        churn_sequence(samples, transitions, pid=123, peer="peer-A")
        for index, key, value in ((1, "epoch", 1), (2, "stage", "initial"), (1, "requested_at", 4)):
            changed = copy.deepcopy(samples)
            changed[index][key] = value
            with self.assertRaises(ValueError):
                churn_sequence(changed, transitions, pid=123, peer="peer-A")
        with self.assertRaises(ValueError):
            churn_sequence(samples, [dict(transitions[0], checked=False), transitions[1]], pid=123, peer="peer-A")

    def test_churn_rejects_closed_original_session_and_reconnection(self):
        samples = [dict(epoch=i, stage=stage, requested_at=i * 3, received_at=i * 3 + 1,
                        receipt=lifecycle(i, count))
                   for i, (stage, count) in enumerate((("initial", 1), ("down", 0), ("up", 1)), 1)]
        transitions = [dict(up=False, checked=True, began_at=4.5, finished_at=5),
                       dict(up=True, checked=True, began_at=7.5, finished_at=8)]
        for stage, opened, closed in ((1, 1, 1), (2, 2, 1)):
            changed = copy.deepcopy(samples)
            changed[stage]["receipt"].update(sessions_opened=opened, sessions_closed=closed)
            with self.subTest(stage=changed[stage]["stage"]), self.assertRaises(ValueError):
                churn_sequence(changed, transitions, pid=123, peer="peer-A")


if __name__ == "__main__":
    unittest.main()
