#pragma once

#include "parsed_policy.hxx"

namespace forge::plugins::net::p2p::node {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
[[nodiscard]] parsed_policy parse_policy(const config& config);
[[nodiscard]] std::vector<forge::net::p2p::endpoint> parse_endpoint_list(const std::vector<std::string>& values);
void apply_connections(forge::net::p2p::node::limits& limits, const connection_config& value);
void apply_identify(forge::net::p2p::node::options& options, const identify_config& value);
void apply_peer_state(forge::net::p2p::peer_store::options& options, const peer_state_config& value);
void apply_tcp(forge::net::p2p::node::options& options, const tcp_config& value);
void apply_quic(forge::net::p2p::node::options& options, const quic_config& value);
void apply_private_network(forge::net::p2p::node::options& options, const config& value);
void apply_config(plugin::impl& state, const config& config);

} // namespace forge::plugins::net::p2p::node
