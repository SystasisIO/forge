#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "parsed_policy.hxx"

namespace forge::plugins::net::p2p::node {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
[[nodiscard]] std::chrono::seconds to_seconds(std::uint64_t value);
void apply_size(std::size_t& target, const std::optional<std::uint64_t>& value, std::string_view field);
void apply_time(std::chrono::milliseconds& target, const std::optional<std::uint64_t>& value);
void apply_bool(bool& target, const std::optional<bool>& value);
[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
[[nodiscard]] parsed_policy parse_policy(const config& config);
[[nodiscard]] std::vector<forge::net::p2p::endpoint> parse_endpoint_list(const std::vector<std::string>& values);
void apply_connections(forge::net::p2p::node::limits& limits, const connection_config& value);
void apply_identify(forge::net::p2p::node::options& options, const identify_config& value);
void apply_peer_state(forge::net::p2p::peer_store::options& options, const peer_state_config& value);
void apply_tcp(forge::net::p2p::node::options& options, const tcp_config& value);
void apply_quic(forge::net::p2p::node::options& options, const quic_config& value);
void apply_private_network(forge::net::p2p::node::options& options, const config& value);
void apply_dns(forge::net::p2p::node::options& options, const dns_config& value);
void apply_dial(forge::net::p2p::node::options& options, const dial_config& value);
void apply_path(forge::net::p2p::path::policy& policy, const std::optional<std::uint64_t>& max_direct_endpoints);
void apply_reachability(forge::net::p2p::node::options& options, const reachability_config& value);
void apply_mdns(forge::net::p2p::node::options& options, const mdns_config& value);
void apply_relay(forge::net::p2p::node::options& options, const relay_config& value, const parsed_policy& policy);
void apply_lifecycle(forge::net::p2p::node::options& options, const config& value);
void apply_topology(forge::net::p2p::node::options& options, const config& value, bool dht_enabled);
void apply_dht_profiles(forge::net::p2p::node::options& options, const std::vector<dht_profile_config>& value);
void apply_config(plugin::impl& state, const config& config);

} // namespace forge::plugins::net::p2p::node
