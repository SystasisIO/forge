#!/usr/bin/env python3

from pathlib import Path
import re
import sys


def require(text: str, needle: str, source: Path) -> None:
    if needle not in text:
        raise SystemExit(f"{source}: missing required typed Block/Info/Admin surface: {needle}")


def forbid(text: str, needle: str, source: Path) -> None:
    if needle in text:
        raise SystemExit(f"{source}: removed Block/Info/Admin surface returned: {needle}")


def forbid_pattern(text: str, pattern: str, source: Path, label: str) -> None:
    if re.search(pattern, text):
        raise SystemExit(f"{source}: removed Block/Info/Admin surface returned: {label}")


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    protocol_dir = root / "libraries/chain/protocol/include/forge/chain/protocol"
    api_dir = root / "libraries/chain/api/include/forge/chain/api"
    block_protocol_path = protocol_dir / "block_query.cppm"
    producer_rewards_protocol_path = protocol_dir / "producer_rewards.cppm"
    producer_rewards_impl_path = root / "libraries/chain/protocol/producer_rewards.cpp"
    feature_protocol_path = protocol_dir / "protocol_feature.cppm"
    activated_feature_protocol_path = protocol_dir / "activated_protocol_feature.cppm"
    activated_feature_info_protocol_path = protocol_dir / "activated_protocol_feature_info.cppm"
    info_protocol_path = protocol_dir / "info.cppm"
    admin_protocol_path = protocol_dir / "admin.cppm"
    block_api_path = api_dir / "block.cppm"
    info_api_path = api_dir / "info.cppm"
    admin_api_path = api_dir / "admin.cppm"
    schema_path = api_dir / "json_schema.cppm"
    limits_path = root / "libraries/chain/api/limits.cpp"
    raw_codec_path = root / "libraries/raw/include/forge/raw/codec.cppm"
    raw_stream_path = root / "libraries/raw/include/forge/raw/stream.cppm"

    block_protocol = block_protocol_path.read_text(encoding="utf-8")
    producer_rewards_protocol = producer_rewards_protocol_path.read_text(encoding="utf-8")
    producer_rewards_impl = producer_rewards_impl_path.read_text(encoding="utf-8")
    feature_protocol = feature_protocol_path.read_text(encoding="utf-8")
    activated_feature_protocol = activated_feature_protocol_path.read_text(encoding="utf-8")
    activated_feature_info_protocol = activated_feature_info_protocol_path.read_text(encoding="utf-8")
    info_protocol = info_protocol_path.read_text(encoding="utf-8")
    admin_protocol = admin_protocol_path.read_text(encoding="utf-8")
    block_api = block_api_path.read_text(encoding="utf-8")
    info_api = info_api_path.read_text(encoding="utf-8")
    admin_api = admin_api_path.read_text(encoding="utf-8")
    schema = schema_path.read_text(encoding="utf-8")
    limits = limits_path.read_text(encoding="utf-8")
    raw_codec = raw_codec_path.read_text(encoding="utf-8")
    raw_stream = raw_stream_path.read_text(encoding="utf-8")

    for needle in (
        "std::vector<forge::chain::protocol::activated_protocol_feature_info> features;",
        "forge::chain::protocol::chain_config parameters;",
        "std::optional<forge::chain::protocol::wasm_parameters> wasm;",
        "std::optional<account_name> lower_bound;",
        "std::optional<bytes> cursor;",
        "std::vector<forge::chain::protocol::producer_info> rows;",
        "forge::chain::protocol::float64 total_vote_weight;",
        "std::optional<bytes> next;",
        "std::vector<forge::chain::protocol::finalizer_vote_record> last_votes;",
        "BOOST_DESCRIBE_STRUCT(producers_request, (), (lower_bound, limit, cursor, anchor, finality_from, audit))",
        "BOOST_DESCRIBE_STRUCT(producers_response, (audited_response), (rows, total_vote_weight, next))",
        "packed_rows.push_back(forge::raw::pack(row));",
        "forge::raw::unpack_nested_exact<producer_info>(stream, std::span<const std::uint8_t>{row})",
        "forge::raw::pack(stream, value.total_vote_weight);",
        "forge::raw::unpack(stream, value.total_vote_weight);",
    ):
        require(block_protocol, needle, block_protocol_path)

    for removed in (
        "bool json",
        "std::string lower_bound",
        "std::vector<forge::variant> features",
        "std::optional<forge::variant> wasm",
        "std::vector<forge::variant> rows",
        "double total_vote_weight",
        "std::string next",
    ):
        forbid(block_protocol, removed, block_protocol_path)
    forbid(block_protocol, "forge::raw::unpack_exact<producer_info>", block_protocol_path)
    forbid_pattern(block_protocol, r"\bforge::variant\s+\w+\s*;", block_protocol_path, "public variant field")
    forbid_pattern(
        block_protocol,
        r"std::(?:optional<)?(?:std::)?string[^;\n]*\b(?:cursor|next|more)\b",
        block_protocol_path,
        "text cursor or continuation",
    )
    forbid_pattern(
        block_protocol,
        r"\busing\s+(?:protocol_features_response|consensus_parameters_response|producers_request|"
        r"producers_response|finalizer_info_response)\b",
        block_protocol_path,
        "compatibility alias",
    )

    for needle in (
        "struct bpay_reward {",
        "account_name owner;",
        "asset quantity;",
        "struct system_reward {",
        "bool eligible = false;",
        "bool active = false;",
        "std::uint32_t unpaid_blocks = 0;",
        "time_point last_claim_time{};",
        "time_point next_claim_time{};",
        "account_name contract;",
        "action_name claim_action;",
        "struct bpay_claim {",
        "std::optional<asset> claimable;",
        "struct producer_reward {",
        "account_name producer;",
        "system_reward system;",
        "bpay_claim bpay;",
        "std::optional<producer_reward> project_producer_reward(const producer_info& system,",
        "struct producer_rewards_request {",
        "account_name producer;",
        "struct producer_rewards_response : audited_response {",
        "block_header anchor_header;",
        "table_rows_response system;",
        "table_rows_response bpay;",
    ):
        require(producer_rewards_protocol, needle, producer_rewards_protocol_path)
    if producer_rewards_protocol.count("account_name contract;") != 2:
        raise SystemExit(f"{producer_rewards_protocol_path}: both reward sources must expose their contract location")
    if producer_rewards_protocol.count("action_name claim_action;") != 2:
        raise SystemExit(f"{producer_rewards_protocol_path}: both reward sources must expose their claim action name")
    forbid(producer_rewards_protocol, "forge::variant", producer_rewards_protocol_path)
    forbid(producer_rewards_protocol, "core_symbol", producer_rewards_protocol_path)
    forbid(producer_rewards_protocol, "bpay_balance", producer_rewards_protocol_path)
    require(producer_rewards_impl, "return std::nullopt;", producer_rewards_impl_path)
    forbid(producer_rewards_impl, "std::invalid_argument", producer_rewards_impl_path)

    for needle in (
        "remaining_container_elements() const noexcept",
        "consume_cumulative_container_elements(std::size_t count) noexcept",
    ):
        require(raw_stream, needle, raw_stream_path)
    for needle in (
        "unpack_nested_exact(ParentStream& parent, std::span<const std::uint8_t> input)",
        "parent_limits.elements, input.size()",
        "parent.remaining_container_elements()",
        "parent_limits.bytes, input.size()",
        "nested.remaining_container_elements()",
        "parent.consume_cumulative_container_elements(consumed)",
        "std::is_class_v<T> && std::is_aggregate_v<T>",
        "boost::pfr::for_each_field(value, [&](const auto& field) { pack(stream, field); });",
        "boost::pfr::for_each_field(value, [&](auto& field) { unpack(stream, field); });",
    ):
        require(raw_codec, needle, raw_codec_path)

    for needle in (
        "struct protocol_feature_specification {",
        "struct protocol_feature {",
        "digest feature_digest;",
        "digest description_digest;",
        "std::vector<digest> dependencies;",
        "std::string protocol_feature_type;",
        "std::vector<protocol_feature_specification> specification;",
        "BOOST_DESCRIBE_STRUCT(protocol_feature_specification, (), (name, value))",
        "BOOST_DESCRIBE_STRUCT(protocol_feature, (),",
        "(feature_digest, description_digest, dependencies, protocol_feature_type, specification))",
    ):
        require(feature_protocol, needle, feature_protocol_path)
    for removed in (
        "import forge.raw.codec;",
        "raw_pack(",
        "raw_unpack(",
    ):
        forbid(feature_protocol, removed, feature_protocol_path)
    for needle in (
        "struct activated_protocol_feature {",
        "digest feature_digest;",
        "std::uint32_t activation_block_num = 0;",
        "BOOST_DESCRIBE_STRUCT(activated_protocol_feature, (), (feature_digest, activation_block_num))",
    ):
        require(activated_feature_protocol, needle, activated_feature_protocol_path)
    for removed in (
        "struct activated_protocol_feature :",
        "forge.chain.protocol.protocol_feature",
        "activation_ordinal",
        "description_digest",
        "dependencies",
        "protocol_feature_type",
        "specification",
        "subjective_restrictions",
    ):
        forbid(activated_feature_protocol, removed, activated_feature_protocol_path)
    for removed in (
        "import forge.raw.codec;",
        "raw_pack(",
        "raw_unpack(",
    ):
        forbid(activated_feature_protocol, removed, activated_feature_protocol_path)

    for needle in (
        "struct activated_protocol_feature_info : protocol_feature {",
        "std::uint32_t activation_ordinal = 0;",
        "std::uint32_t activation_block_num = 0;",
        "forge::raw::pack(stream, value.feature_digest);",
        "forge::raw::pack(stream, value.activation_ordinal);",
        "forge::raw::pack(stream, value.activation_block_num);",
        "forge::raw::pack(stream, value.description_digest);",
        "forge::raw::pack(stream, value.dependencies);",
        "forge::raw::pack(stream, value.protocol_feature_type);",
        "forge::raw::pack(stream, value.specification);",
        "BOOST_DESCRIBE_STRUCT(activated_protocol_feature_info, (),",
        "(feature_digest, activation_ordinal, activation_block_num, description_digest, dependencies,",
    ):
        require(activated_feature_info_protocol, needle, activated_feature_info_protocol_path)
    forbid(activated_feature_info_protocol, "subjective_restrictions", activated_feature_info_protocol_path)
    raw_order = (
        "value.feature_digest",
        "value.activation_ordinal",
        "value.activation_block_num",
        "value.description_digest",
        "value.dependencies",
        "value.protocol_feature_type",
        "value.specification",
    )
    for operation in ("raw_pack", "raw_unpack"):
        value_type = "activated_protocol_feature_info&" if operation == "raw_unpack" else "const activated_protocol_feature_info&"
        signature = f"template <typename Stream> void {operation}(Stream& stream, {value_type} value) {{"
        body = activated_feature_info_protocol.split(signature, 1)[1].split("}", 1)[0]
        positions = [body.index(field) for field in raw_order]
        if positions != sorted(positions):
            raise SystemExit(f"{activated_feature_info_protocol_path}: Spring {operation} field order changed")

    forbid(admin_protocol, "struct protocol_feature_specification {", admin_protocol_path)
    for duplicate_field in (
        "digest feature_digest;",
        "digest description_digest;",
        "std::vector<digest> dependencies;",
        "std::string protocol_feature_type;",
        "std::vector<protocol_feature_specification> specification;",
    ):
        forbid(activated_feature_info_protocol, duplicate_field, activated_feature_info_protocol_path)
        forbid(admin_protocol, duplicate_field, admin_protocol_path)
    require(admin_protocol, "struct supported_protocol_feature : protocol_feature {", admin_protocol_path)

    for needle in (
        "forge::chain::protocol::resource_limits_config resource_config;",
        "forge::chain::protocol::resource_limits_state resource_state;",
        "earliest_available_block_num, resource_config, resource_state, available,",
    ):
        require(info_protocol, needle, info_protocol_path)
    for removed in (
        "virtual_block_cpu_limit",
        "virtual_block_net_limit",
        "block_cpu_limit",
        "block_net_limit",
        "total_cpu_weight",
        "total_net_weight",
    ):
        forbid(info_protocol, removed, info_protocol_path)

    require(
        admin_protocol,
        "std::vector<forge::chain::protocol::account_ram_correction> rows;",
        admin_protocol_path,
    )
    require(admin_protocol, "struct finalizer_status {", admin_protocol_path)
    require(admin_protocol, "bool enabled = false;", admin_protocol_path)
    require(admin_protocol, "bool policy_member = false;", admin_protocol_path)
    require(admin_protocol, "std::optional<finalizer_vote_record> last_emitted_vote;", admin_protocol_path)
    forbid(admin_protocol, "std::vector<forge::variant> rows", admin_protocol_path)
    forbid_pattern(admin_protocol, r"\bforge::variant\s+\w+\s*;", admin_protocol_path, "public variant field")
    forbid_pattern(
        admin_protocol,
        r"\busing\s+ram_corrections_response\b",
        admin_protocol_path,
        "compatibility alias",
    )

    require(block_api, 'FORGE_API_CONTRACT("forge.chain.api.block", 2, 2)', block_api_path)
    require(info_api, 'FORGE_API_CONTRACT("forge.chain.api.info", 2, 0)', info_api_path)
    require(admin_api, 'FORGE_API_CONTRACT("forge.chain.api.admin", 2, 4)', admin_api_path)
    require(block_api, "FORGE_API_METHOD_TYPED_SINCE(get_producer_rewards", block_api_path)
    require(
        block_api,
        '"/v1/chain/blocks/producer-rewards?producer={producer}&anchor={anchor}&finality_from={finality_from}"',
        block_api_path,
    )
    forbid(block_api, "core_symbol", block_api_path)
    require(admin_api, "FORGE_API_METHOD_TYPED_SINCE(get_finalizer_status", admin_api_path)
    require(admin_api, '"/v1/chain/admin/finalizer"', admin_api_path)
    require(admin_api, "FORGE_API_METHOD_TYPED_SINCE(get_operator_identity", admin_api_path)
    require(admin_api, "FORGE_API_METHOD_TYPED_SINCE(get_node_status", admin_api_path)
    require(admin_api, "FORGE_API_METHOD_TYPED_SINCE(snapshot_status", admin_api_path)
    require(admin_api, "FORGE_API_METHOD_TYPED_SINCE(request_snapshot", admin_api_path)
    require(admin_api, '"/v1/chain/admin/operator-identity"', admin_api_path)
    require(admin_api, '"/v1/chain/admin/node-status"', admin_api_path)
    require(block_api, "FORGE_HTTP_GET(get_producers", block_api_path)
    require(block_api, "cursor={cursor}", block_api_path)
    forbid(block_api, "json={json}", block_api_path)

    require(limits, '"producer cursor must not be empty"', limits_path)
    if len(re.findall(r"require_nonempty_next\(response\.next\);", limits)) < 8:
        raise SystemExit(f"{limits_path}: producer response next validation is missing")

    require(schema, "json_schema_traits<forge::chain::protocol::float64>", schema_path)
    require(schema, "json_schema_traits<forge::crypto::bls::public_key>", schema_path)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
