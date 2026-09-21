#!/usr/bin/env python3

from pathlib import Path
import re
import sys


def require(text: str, needle: str, path: Path) -> None:
    if needle not in text:
        raise SystemExit(f"{path}: missing portable verified-client surface: {needle}")


def forbid(text: str, needle: str, path: Path) -> None:
    if needle in text:
        raise SystemExit(f"{path}: forbidden portability dependency: {needle}")


def production_sources(*roots: Path) -> tuple[Path, ...]:
    return tuple(sorted(path for root in roots for path in root.rglob("*")
                        if path.suffix in (".cpp", ".cppm", ".hpp", ".hxx")))


def reject_imports(text: str, path: Path) -> None:
    import_patterns = (
        re.compile(r'^\s*#\s*include\s*[<"][^>"]*(?:filesystem|json)[^>"]*[>"]', re.IGNORECASE),
        re.compile(r'^\s*(?:export\s+)?import\s+[^;]*(?:filesystem|json)[^;]*;', re.IGNORECASE),
    )
    for line_number, line in enumerate(text.splitlines(), start=1):
        if any(pattern.search(line) for pattern in import_patterns):
            raise SystemExit(f"{path}:{line_number}: JSON/filesystem import is outside the portable closure")


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    savanna = root / "libraries/chain/savanna"
    api = root / "libraries/chain/api"
    protocol = root / "libraries/chain/protocol"
    kernel_interfaces = (
        "exceptions.cppm", "types.cppm", "policy.cppm", "finality_core.cppm", "qc.cppm", "rank.cppm",
        "validation.cppm", "vote.cppm", "vote_accumulator.cppm", "finalizer_safety.cppm",
    )
    kernel_implementations = (
        "types.cpp", "policy.cpp", "finality_core.cpp", "qc.cpp", "rank.cpp", "validation.cpp",
        "vote.cpp", "vote_accumulator.cpp", "vote_accumulator_impl.cpp", "finalizer_safety.cpp",
        "details/vote_accumulator_impl.hxx",
    )
    sources = (
        *production_sources(savanna),
        *(api / name for name in (
            "include/forge/chain/api/savanna_finality_verifier.cppm",
            "savanna_finality_verifier.cpp",
            "details/savanna_finality_trust_store.hxx",
            "savanna_finality_trust_store.cpp",
            "include/forge/chain/api/contract_table_projection_verifier.cppm",
            "contract_table_projection_verifier.cpp",
            "include/forge/chain/api/verified_client_factory.cppm",
            "verified_client_factory.cpp",
        )),
        *(protocol / name for name in (
            "include/forge/chain/protocol/contract_commitment.cppm",
            "contract_commitment.cpp",
        )),
    )
    if not sources:
        raise SystemExit("portable verified-client production closure is empty")
    for path in sources:
        text = path.read_text(encoding="utf-8")
        portable_text = text.replace('"spine.savanna.finality"', "")
        if re.search(r"(?:spine|storlane|chain_plugin|state_api_impl|block_api_impl|chainbase)",
                     portable_text, re.IGNORECASE):
            raise SystemExit(f"{path}: forbidden Spine/product vocabulary in production")
        reject_imports(text, path)

    wire_scheme = savanna / "include/forge/chain/savanna/finality_witness.cppm"
    wire_text = wire_scheme.read_text(encoding="utf-8")
    require(wire_text, '"spine.savanna.finality"', wire_scheme)
    for needle in ("struct finality_trust_advance", "advance_finality_trust(",
                   "advance_finality_trust_with_replay("):
        require(wire_text, needle, wire_scheme)

    savanna_verifier = api / "include/forge/chain/api/savanna_finality_verifier.cppm"
    savanna_verifier_text = savanna_verifier.read_text(encoding="utf-8")
    for needle in ("class savanna_finality_verifier final : public finality_verifier",
                   "preferred_trust() const", "replay_state(", "verified_replay(",
                   "std::shared_ptr<savanna_finality_verifier>"):
        require(savanna_verifier_text, needle, savanna_verifier)
    forbid(savanna_verifier_text, "replay_savanna_finality_state", savanna_verifier)

    savanna_verifier_impl = api / "savanna_finality_verifier.cpp"
    savanna_verifier_impl_text = savanna_verifier_impl.read_text(encoding="utf-8")
    replay_start = savanna_verifier_impl_text.index("savanna_finality_verifier::replay_state(")
    replay_translator = savanna_verifier_impl_text.index("return translate_finality_failure([&] {", replay_start)
    replay_prefix = savanna_verifier_impl_text[replay_start:replay_translator]
    for needle in ("sha256::hash", "cached_replay_state"):
        forbid(replay_prefix, needle, savanna_verifier_impl)
    require(savanna_verifier_impl_text,
            "return translate_finality_failure([&] {\n      const auto proof_digest = forge::crypto::digest::sha256::hash(proof);\n"
            "      const auto limits = impl_->trust_store.witness_limits();\n"
            "      if (const auto cached = impl_->trust_store.cached_replay_state(expected, proof_digest))",
            savanna_verifier_impl)
    require(savanna_verifier_impl_text,
            "if (const auto cached = impl_->trust_store.cached_replay(expected, proof_digest))",
            savanna_verifier_impl)
    require(savanna_verifier_impl_text,
            "savanna::advance_finality_trust_with_replay(*snapshot.source_trust, witness, expected, limits).replay",
            savanna_verifier_impl)

    verify_start = savanna_verifier_impl_text.index("void savanna_finality_verifier::verify(")
    verify_translator = savanna_verifier_impl_text.index("auto verified = translate_finality_failure([&] {", verify_start)
    forbid(savanna_verifier_impl_text[verify_start:verify_translator], "sha256::hash", savanna_verifier_impl)
    require(savanna_verifier_impl_text,
            "auto verified = translate_finality_failure([&] {\n"
            "      auto proof_digest = forge::crypto::digest::sha256::hash(proof);\n"
            "      const auto limits = impl_->trust_store.witness_limits();",
            savanna_verifier_impl)

    trust_store = (
        api / "details/savanna_finality_trust_store.hxx",
        api / "savanna_finality_trust_store.cpp",
    )
    for path in trust_store:
        text = path.read_text(encoding="utf-8")
        if re.search(r"(?:persistence|filesystem|json|spine|storlane|chain_plugin|state_api_impl|block_api_impl|chainbase)",
                     text, re.IGNORECASE):
            raise SystemExit(f"{path}: trust store contains a forbidden portability dependency")

    verified_factory = api / "include/forge/chain/api/verified_client_factory.cppm"
    verified_factory_text = verified_factory.read_text(encoding="utf-8")
    require(verified_factory_text, "std::shared_ptr<finality_verifier> finality;", verified_factory)
    for needle in ("savanna::finality_trust trust", "additional_trusts"):
        forbid(verified_factory_text, needle, verified_factory)

    kernel = (
        *(savanna / "include/forge/chain/savanna" / name for name in kernel_interfaces),
        *(savanna / name for name in kernel_implementations),
    )
    for path in kernel:
        text = path.read_text(encoding="utf-8")
        if re.search(r"forge\.chain\.protocol|forge::chain::protocol|forge/chain/protocol", text):
            raise SystemExit(f"{path}: neutral Savanna kernel imports Chain Protocol")

    policy_state = savanna / "include/forge/chain/savanna/policy_state.cppm"
    require(policy_state.read_text(encoding="utf-8"), "struct finalizer_policy_state", policy_state)
    genesis = savanna / "include/forge/chain/savanna/genesis.cppm"
    genesis_text = genesis.read_text(encoding="utf-8")
    for needle in ("struct genesis",):
        require(genesis_text, needle, genesis)
    for needle in ("filesystem", "load_genesis", "save_genesis"):
        forbid(genesis_text, needle, genesis)

    commitment = protocol / "include/forge/chain/protocol/contract_commitment.cppm"
    commitment_text = commitment.read_text(encoding="utf-8")
    for needle in ("struct contract_table_location", "struct table_value", "struct primary_value",
                   "struct secondary_value", "contract_table_key", "contract_index_prefix",
                   "contract_primary_key", "contract_secondary_key"):
        require(commitment_text, needle, commitment)
    api_cmake = api / "CMakeLists.txt"
    api_cmake_text = api_cmake.read_text(encoding="utf-8")
    require(api_cmake_text, "forge_chain_savanna", api_cmake)
    for cmake in (api_cmake, protocol / "CMakeLists.txt", savanna / "CMakeLists.txt"):
        text = cmake.read_text(encoding="utf-8")
        if re.search(r"(?:spine|storlane)[_:]", text, re.IGNORECASE):
            raise SystemExit(f"{cmake}: portable production closure must not link a product target")
        forbid(text, "forge_codec_json", cmake)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
