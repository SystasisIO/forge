#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace forge::net::p2p::detail {

// Test-only access through the existing node friend; no production bypass.
boost::asio::awaitable<forge::asio::gate::ticket> hold_session_admission_for_test(node&);
std::vector<std::uint8_t> local_signed_peer_record_for_test(node&);
boost::asio::awaitable<void> unregister_topology_rendezvous_for_test(node&, std::size_t, std::string);

}
