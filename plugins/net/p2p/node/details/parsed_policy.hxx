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

} // namespace forge::plugins::net::p2p::node
