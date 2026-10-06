"""Exact PR10 private/inline registration; registration is not live acceptance."""

from private_profile_cases import Case, case_specs
from private_profile_evidence import CONTRACTS, PRIVATE_CONTRACTS, INLINE_CONTRACTS
from stage6_evidence_contract import evidence_contract_for

PRIVATE_OWNERS = dict(zip(PRIVATE_CONTRACTS, (
    "transport.tcp_yamux", "negotiation.multistream_select",
    "security.noise_tls_identity", "security.noise_tls_identity",
    "protocol.ping", "protocol.identify", "routing.kademlia_amino", "discovery.rendezvous",
)))
SCENARIOS = PRIVATE_OWNERS | {name: "connections.inlined_muxer_negotiation" for name in INLINE_CONTRACTS}
EVIDENCE_CONTRACTS = {evidence_contract_for(name): name for name in CONTRACTS}


def expected_contract(name):
    spec = Case(name, "forge", "rust" if "rust" in name else "go")
    directions = {f"{s.dialer}_to_{s.listener}" for s in case_specs(
        "private-profile" if name in PRIVATE_CONTRACTS else "inline-muxer") if s.contract == name}
    return (directions, "limited" if name.startswith("inline_muxer_rust_") else "passed",
            "private_network" if spec.private else "native",
            ("tcp", "pnet", "yamux") if spec.private else ("tcp", "yamux"),
            spec.runner_id, ("security.private_network_psk",) if spec.private else (),
            evidence_contract_for(name))


def validate_requirements(required, suite):
    errors = []
    for group, names in (("private-profile", PRIVATE_CONTRACTS), ("inline-muxer", INLINE_CONTRACTS)):
        selected = {key: value for key, value in required.items() if key[1] in names}
        if suite == group or selected:
            if {name for _, name in selected} != set(names):
                errors.append(f"{group} suite requires all eight registered contracts")
            for (owner, name), value in selected.items():
                if owner != SCENARIOS[name] or value != expected_contract(name):
                    errors.append(f"{name}: exact owner/directions/profile/status contract mismatch")
    return errors
