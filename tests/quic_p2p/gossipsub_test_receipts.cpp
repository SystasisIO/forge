module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

module forge.net.p2p.node;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;

#include "gossipsub_test_receipts.hxx"

namespace forge::tests::p2p {

namespace p2p = forge::net::p2p;
namespace pubsub = p2p::pubsub;

void gossipsub_test_receipts::capture(const pubsub::trace_event& event) {
   if ((event.kind != pubsub::trace_kind::rpc_read && event.kind != pubsub::trace_kind::rpc_write) ||
       event.framed_rpc.empty()) { return; }
   try {
      auto rpc = pubsub::codec::decode(event.framed_rpc);
      auto grafts = rpc.control_value ? std::move(rpc.control_value->grafts) : std::vector<pubsub::control::graft>{};
      if (rpc.subscriptions.empty() && grafts.empty()) { return; }
      if (event.session_id == 0 || event.stream_id < 0 || event.generation == 0) {
         throw std::runtime_error{"GossipSub control receipt lacks a native owner"};
      }
      auto lock = std::scoped_lock{_mutex};
      if (_events.size() == 2048U) { throw std::runtime_error{"GossipSub control receipt bound exceeded"}; }
      _events.push_back(receipt{event.kind, event.peer, event.session_id, event.stream_id, event.generation,
                               std::move(rpc.subscriptions), std::move(grafts)});
   } catch (...) {
      auto lock = std::scoped_lock{_mutex};
      if (!_failure) { _failure = std::current_exception(); }
   }
}

bool gossipsub_test_receipts::subscribed(const p2p::peer_id& peer, const pubsub::topic& subject) const {
   auto lock = std::scoped_lock{_mutex};
   check_failure();
   return std::ranges::any_of(_events, [&](const auto& event) {
      return event.kind == pubsub::trace_kind::rpc_read && event.peer == peer && has_subscription(event, subject);
   });
}

std::uint64_t gossipsub_test_receipts::written_generation(const p2p::peer_id& peer, const pubsub::topic& subject) const {
   auto lock = std::scoped_lock{_mutex};
   check_failure();
   auto generation = std::uint64_t{};
   for (const auto& event : _events) {
      if (event.kind == pubsub::trace_kind::rpc_write && event.peer == peer && has_subscription(event, subject)) {
         generation = std::max(generation, event.generation);
      }
   }
   return generation;
}

bool gossipsub_test_receipts::reannounced_before_graft(const p2p::peer_id& peer, const pubsub::topic& subject,
                                                     std::int64_t former_stream) const {
   auto lock = std::scoped_lock{_mutex};
   check_failure();
   for (auto index = std::size_t{}; index < _events.size(); ++index) {
      const auto& announcement = _events[index];
      if (announcement.kind != pubsub::trace_kind::rpc_read || announcement.peer != peer ||
          announcement.stream == former_stream || !has_subscription(announcement, subject)) { continue; }
      for (auto next = index + 1U; next < _events.size(); ++next) {
         const auto& graft = _events[next];
         if (graft.kind == pubsub::trace_kind::rpc_read && graft.peer == peer &&
             graft.session == announcement.session && graft.stream == announcement.stream &&
             graft.generation == announcement.generation && std::ranges::any_of(graft.grafts, [&](const auto& entry) {
                return entry.subject == subject;
             })) { return true; }
      }
   }
   return false;
}

std::int64_t gossipsub_test_receipts::latest_subscription_stream(const p2p::peer_id& peer,
                                                              const pubsub::topic& subject) const {
   auto lock = std::scoped_lock{_mutex};
   check_failure();
   for (auto event = _events.rbegin(); event != _events.rend(); ++event) {
      if (event->kind == pubsub::trace_kind::rpc_read && event->peer == peer && has_subscription(*event, subject)) {
         return event->stream;
      }
   }
   throw std::runtime_error{"GossipSub subscription has no native read receipt"};
}

bool gossipsub_test_receipts::has_subscription(const receipt& event, const pubsub::topic& subject) {
   return std::ranges::any_of(event.subscriptions, [&](const auto& entry) {
      return entry.subscribe && entry.subject == subject;
   });
}

void gossipsub_test_receipts::check_failure() const {
   if (_failure) { std::rethrow_exception(_failure); }
}

} // namespace forge::tests::p2p
