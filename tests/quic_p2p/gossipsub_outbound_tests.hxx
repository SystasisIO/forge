#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <vector>
#include "gossipsub_test_receipts.hxx"
#include "pubsub_router_fixture.hxx"

namespace forge::tests::p2p::gossipsub_outbound_tests {

struct delivery_state {
   std::mutex mutex;
   std::condition_variable ready;
   std::set<std::vector<std::uint8_t>> values;
};

void singleflights_cold_subscriptions_then_serializes_concurrent_publishes();
void dead_outbound_generation_detaches_mesh_and_reannounces_before_graft();

// Native outbound regressions share owned receipts and a passive validation gate.
class native_trace {
 public:
   void bind(forge::net::p2p::node&);
   void capture(const forge::net::p2p::pubsub::trace_event&);
   void arm(const forge::net::p2p::peer_id&, const forge::net::p2p::pubsub::topic&,
            std::vector<std::uint8_t>);
   void release() noexcept;
   void stage(std::string_view);
   [[nodiscard]] bool held(const std::vector<std::uint8_t>&) const;
   [[nodiscard]] std::vector<pubsub_router_receipt> receipts() const;
   [[nodiscard]] bool observed(forge::net::p2p::pubsub::trace_kind,
                              const std::vector<std::uint8_t>&) const;
   [[nodiscard]] std::size_t messages(forge::net::p2p::pubsub::trace_kind,
                                     const forge::net::p2p::peer_id&,
                                     const forge::net::p2p::pubsub::topic&) const;
   gossipsub_test_receipts controls;

 private:
   forge::net::p2p::node* _owner = nullptr;
   mutable std::mutex _mutex;
   std::condition_variable _changed;
   std::vector<pubsub_router_receipt> _receipts;
   std::exception_ptr _failure;
   std::optional<forge::net::p2p::peer_id> _gate_peer;
   forge::net::p2p::pubsub::topic _gate_topic;
   std::vector<std::uint8_t> _gate_data;
   std::vector<std::uint8_t> _gate_id;
   std::string_view _stage = "unarmed";
   bool _held = false;
   bool _released = false;
   bool _reentered = false;
};

void rejects_publish_after_cached_stream_shutdown();
void forwards_between_subscribed_peers();
void outbound_byte_limit_rejects_publish_without_stopping_node();
void outbound_byte_limit_counts_blocked_active_publication_per_peer();
void outbound_byte_limit_is_global_across_peers();

} // namespace forge::tests::p2p::gossipsub_outbound_tests

// Block-scope declarations in the five legacy bodies avoid touching their shared includes.
namespace forge::net::p2p {
void gossipsub_outbound_cached_shutdown_regression();
void gossipsub_outbound_forwarding_regression();
void gossipsub_outbound_quota_rejection_regression();
void gossipsub_outbound_per_peer_quota_regression();
void gossipsub_outbound_global_quota_regression();
} // namespace forge::net::p2p
