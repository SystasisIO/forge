#pragma once

#include "parsed_policy.hxx"

namespace forge::plugins::net::p2p::node {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
[[nodiscard]] parsed_policy parse_policy(const config& config);
[[nodiscard]] std::vector<forge::net::p2p::endpoint> parse_endpoint_list(const std::vector<std::string>& values);
void apply_config(plugin::impl& state, const config& config);

} // namespace forge::plugins::net::p2p::node
