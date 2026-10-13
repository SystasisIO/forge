#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Include after the native node/pubsub modules and pubsub_router_fixture.hxx.
namespace forge::tests::p2p::gossipsub_control_tests {

forge::net::p2p::pubsub::options control_options(const forge::net::p2p::pubsub::topic&);
std::size_t rpc_count(const pubsub_router_fixture&, const forge::net::p2p::node&,
    const forge::net::p2p::peer_id&, forge::net::p2p::pubsub::trace_kind,
    const std::function<bool(const forge::net::p2p::pubsub::rpc&)>&);
std::size_t control_count(const pubsub_router_fixture&, const forge::net::p2p::node&,
    const forge::net::p2p::peer_id&, forge::net::p2p::pubsub::trace_kind, bool prune);
std::size_t announcement_count(const pubsub_router_fixture&, const forge::net::p2p::node&,
    const forge::net::p2p::peer_id&);
std::optional<pubsub_router_receipt> last_prune(const pubsub_router_fixture&, const forge::net::p2p::node&,
    const forge::net::p2p::peer_id&, forge::net::p2p::pubsub::trace_kind);
void connect_announced(pubsub_router_fixture&, forge::net::p2p::node&, forge::net::p2p::node&);
void expect_prune(pubsub_router_fixture&, forge::net::p2p::node&, forge::net::p2p::node&, std::size_t before,
    std::chrono::seconds backoff = std::chrono::seconds{1});
std::string malformed_diagnostics(const forge::net::p2p::node&);
void check_behaviour(const forge::net::p2p::node&, const forge::net::p2p::peer_id&, double penalty);
void register_echo(forge::net::p2p::node&);
void echo(pubsub_router_fixture&, forge::net::p2p::node&, forge::net::p2p::node&);
void exercise_topic_capacity(bool violate_backoff);
void exercise_backoff(bool leave, bool violate_backoff, std::string_view listen_address);

void graft_at_topic_subscription_cap_sends_prune_without_state_or_subscribe_loop();
void received_prune_blocks_immediate_graft_until_backoff_expiry();
void unsubscribe_sends_leave_and_enforces_backoff_until_expiry();
void heartbeat_obeys_sent_and_received_prune_backoff_until_expiry();
void control_spam_is_penalized_without_stopping_node();
void abusive_peer_crossing_malformed_threshold_closes_only_offender_session();
void active_registered_handler_error_is_charged_once_without_closing_session();

} // namespace forge::tests::p2p::gossipsub_control_tests
