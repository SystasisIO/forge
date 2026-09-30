#pragma once

namespace forge::plugins::net::p2p::node {

struct parsed_policy {
   forge::net::p2p::path::policy path{};
   bool relay_client_enabled = true;
   bool relay_server_enabled = false;
   bool relay_public_allowed = false;
   std::chrono::milliseconds relay_reservation_ttl{60'000};
   std::size_t relay_max_candidates = 4;
};

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
[[nodiscard]] parsed_policy parse_policy(const config& config);
[[nodiscard]] std::vector<forge::net::p2p::endpoint> parse_endpoint_list(const std::vector<std::string>& values);
void apply_config(plugin::impl& state, const config& config);

} // namespace forge::plugins::net::p2p::node
