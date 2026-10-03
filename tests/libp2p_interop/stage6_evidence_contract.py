"""Closed identifiers and native PR9 profiles for Stage 6 evidence contracts.

This module intentionally has no checker imports: both source gates use the
same exact identifier rule without creating an import cycle.
"""

EVIDENCE_CONTRACT_PREFIX = "forge.p2p.evidence."
EVIDENCE_CONTRACT_SUFFIX = ".v1"

# Closed native execution profiles, independently checked against the PR9 suite.
# Lifecycle directions describe Forge HOP reservations observed by native relays,
# not bilateral compatibility of host-local AutoRelay orchestration.
AUTORELAY_NATIVE_PROFILES = {
    "autorelay_lifecycle": ("relay.autorelay_lifecycle", "lifecycle", "quic", "quic_stage6"),
    "autorelay_lifecycle_native_tcp_yamux": (
        "relay.autorelay_lifecycle", "lifecycle", "tcp", "tcp_stage6"
    ),
    "autorelay_lifecycle_native_tcp_tls_yamux": (
        "relay.autorelay_lifecycle", "lifecycle", "tcp-tls", "tcp_tls_stage6"
    ),
    "relay_v2_service": ("relay.circuit_v2_service", "service", "quic", "quic_stage6"),
    "relay_v2_service_native_tcp_yamux": (
        "relay.circuit_v2_service", "service", "tcp", "tcp_stage6"
    ),
    "relay_v2_service_native_tcp_tls_yamux": (
        "relay.circuit_v2_service", "service", "tcp-tls", "tcp_tls_stage6"
    ),
}
AUTORELAY_NATIVE_DIRECTIONS = {
    "lifecycle": {"forge_to_go", "forge_to_rust"},
    "service": {"go_to_forge", "rust_to_forge"},
}
AUTORELAY_NATIVE_SOURCE_CASES = {
    "lifecycle": "autorelay.discovery_reservation_lifecycle",
    "service": "interop.live_ping_identify_relay",
}


def evidence_contract_for(scenario_id: str) -> str:
    return f"{EVIDENCE_CONTRACT_PREFIX}{scenario_id}{EVIDENCE_CONTRACT_SUFFIX}"
