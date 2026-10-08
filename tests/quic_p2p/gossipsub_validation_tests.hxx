#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>
#include "libp2p_identity_fixture.hxx"

// Include after the node/stream/identity/endpoint/pubsub modules and pubsub_router_fixture.hxx.
namespace forge::tests::p2p::gossipsub_validation_tests {

forge::net::p2p::node::options options_for(const identity_fixture&,
    forge::net::p2p::capability_set = {.bits = forge::net::p2p::capabilities::direct_quic |
                                             forge::net::p2p::capabilities::pubsub});
forge::net::p2p::node::options pubsub_options_for(const identity_fixture&);
forge::net::p2p::endpoint listen(forge::net::p2p::node&, forge::asio::runtime&);
forge::net::p2p::endpoint listen_tcp(forge::net::p2p::node&, forge::asio::runtime&);
void wait_on_runtime(forge::asio::runtime&, std::chrono::milliseconds, std::string_view);
void wait_for_gossipsub(forge::asio::runtime&, std::string_view, const std::function<bool()>&);
bool authenticated_gossipsub_peer(const forge::net::p2p::node&, const forge::net::p2p::peer_id&);
bool gossipsub_mesh_peer(const forge::net::p2p::node&, const forge::net::p2p::peer_id&,
                         const forge::net::p2p::pubsub::topic&);
forge::net::p2p::pubsub::options native_options(const forge::net::p2p::pubsub::topic&);
std::vector<forge::net::p2p::pubsub::rpc> read_rpcs(const pubsub_router_fixture&,
    const forge::net::p2p::node&, const forge::net::p2p::peer_id&,
    forge::net::p2p::pubsub::trace_kind = forge::net::p2p::pubsub::trace_kind::rpc_read);
std::size_t want_count(const pubsub_router_fixture&, const forge::net::p2p::node&,
    const forge::net::p2p::peer_id&, const std::vector<std::uint8_t>&);
bool committed(const pubsub_router_fixture&, const forge::net::p2p::node&,
    const std::vector<std::uint8_t>&, forge::net::p2p::pubsub::validation_result);
forge::net::p2p::pubsub::peer_score_snapshot score_of(const forge::net::p2p::node&,
    const forge::net::p2p::peer_id&);
void connect_ready(pubsub_router_fixture&, forge::net::p2p::node&, forge::net::p2p::node&);
void read_barrier(pubsub_router_fixture&, forge::net::p2p::stream&, forge::net::p2p::node&,
    const forge::net::p2p::peer_id&);

void nodes_deliver_signed_publish_over_negotiated_stream();
void retry_is_redelivered_after_bounded_cooldown();
void ignore_remains_terminal_during_history_window();
void in_progress_validation_is_not_replaced_after_cache_pressure();
void retry_source_is_not_replaced_by_foreign_ihave();
void retry_stops_after_max_attempts_without_duplicate_history();
void unavailable_source_stops_retry_requests_after_limit();
void reject_remains_terminal_during_history_window();
void validation_queue_limit_retries_excess_without_penalizing_peer();

} // namespace forge::tests::p2p::gossipsub_validation_tests
