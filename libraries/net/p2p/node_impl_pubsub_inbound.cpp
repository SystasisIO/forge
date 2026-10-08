module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <random>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"

#include "details/pubsub_peer_score.hxx"
#include "details/pubsub_router.hxx"

namespace forge::net::p2p {

std::string bytes_key(std::span<const std::uint8_t> bytes) {
   return {bytes.begin(), bytes.end()};
}

bool node::impl::pubsub_session_live_locked(const std::shared_ptr<session_state>& session) const noexcept {
   if (stopped || !session || session->closed || session->authentication == peer_authentication::unverified) {
      return false;
   }
   const auto owner = sessions.find(session->id);
   return owner != sessions.end() && owner->second == session;
}

boost::asio::awaitable<void> node::impl::handle_pubsub(std::shared_ptr<session_state> session,
                                                       forge::net::p2p::stream stream, protocol_id protocol) {
   if (!options.capabilities.has(capabilities::pubsub)) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "GossipSub is disabled");
   }
   auto generation = std::uint64_t{};
   auto stale_owner = false;
   {
      const auto lock = std::scoped_lock{mutex};
      stale_owner = !pubsub_session_live_locked(session);
      if (!stale_owner) {
         if (!pubsub_peer_live_locked(session->info.remote_peer) && !connect_pubsub_peer_locked(session->info.remote_peer)) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub score/peer capacity reached");
         }
         generation = pubsub_value.next_inbound_generation++;
         const auto [row, inserted] = pubsub_value.inbound.try_emplace(session->info.remote_peer);
         try {
            row->second.emplace(generation, session->id);
         } catch (...) {
            if (inserted) {
               pubsub_value.inbound.erase(row);
            }
            throw;
         }
      }
   }
   if (stale_owner) {
      stream.request_cancel(); // Only this late stream, outside the node mutex; never resurrect its peer.
      co_return;
   }
   try {
      co_await handle_pubsub_stream(session, std::move(stream), std::move(protocol), generation);
   } catch (...) {
      finish_pubsub_inbound(session->info.remote_peer, generation);
      throw;
   }
   finish_pubsub_inbound(session->info.remote_peer, generation);
}

boost::asio::awaitable<void> node::impl::handle_pubsub_stream(std::shared_ptr<session_state> session,
    forge::net::p2p::stream stream, protocol_id protocol, std::uint64_t generation) {
   const auto& peer = session->info.remote_peer;
   auto wire_options = options.limits.pubsub;
   wire_options.preferred = protocol == builtins::meshsub_v10 ? pubsub::version::v1_0 : pubsub::version::v1_1;
   auto buffer = std::vector<std::uint8_t>{};
   const auto reject = [&](const std::optional<pubsub::topic>& subject, bool protocol_rejected) {
      return increment_pubsub_invalid(session, subject, protocol_rejected);
   };
   while (true) {
      auto payload = std::vector<std::uint8_t>{};
      auto close_after_error = false;
      try {
         payload = co_await async_read_length_delimited(stream, buffer, wire_options.limits.max_rpc_size);
      } catch (const forge::exceptions::base& error) {
         auto closed_by_node = false;
         {
            const auto lock = std::scoped_lock{mutex};
            closed_by_node = !pubsub_session_live_locked(session);
         }
         if (closed_by_node || is_orderly_stream_close(error)) {
            co_return;
         }
         if (!reject(std::nullopt, true)) { co_return; }
         close_after_error = true;
      }
      if (close_after_error) {
         stream.cancel();
         co_return;
      }
      trace_pubsub([&] {
         return pubsub::trace_event{.kind = pubsub::trace_kind::rpc_read, .peer = peer,
            .generation = generation, .protocol = protocol, .session_id = session->id, .stream_id = stream.id(),
            .framed_rpc = payload};
      });
      auto received = pubsub::codec::received_rpc{};
      try {
         received = pubsub::codec::decode_received(payload, wire_options);
      } catch (const forge::exceptions::base&) {
         if (!reject(std::nullopt, true)) { co_return; }
         close_after_error = true;
      }
      if (close_after_error) {
         stream.cancel();
         co_return;
      }
      const auto& value = received.value;
      {
         const auto lock = std::scoped_lock{mutex};
         if (!pubsub_session_live_locked(session)) { co_return; }
         for (const auto& subscription : value.subscriptions) {
            if (subscription.subscribe) {
               if (!record_pubsub_subscription_locked(peer, subscription.subject.value)) {
                  ++metrics_value.backpressure_rejections;
               }
            } else {
               if (const auto topics = pubsub_value.peer_topics.find(peer); topics != pubsub_value.peer_topics.end()) {
                  pubsub_value.remote_topic_entries -= topics->second.erase(subscription.subject.value);
                  if (topics->second.empty()) {
                     pubsub_value.peer_topics.erase(topics);
                  }
               }
               prune_pubsub_peer_locked(subscription.subject.value, peer);
            }
         }
      }
      sample_pubsub_application_scores(peer);
      {
         const auto lock = std::scoped_lock{mutex};
         if (!pubsub_session_live_locked(session)) { co_return; }
         const auto graylist = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.graylist_threshold : 0.0;
         // Even graylisted peers can withdraw bounded subscription state.
         if (!pubsub_peer_live_locked(peer) || pubsub_score_locked(peer) < graylist) {
            continue;
         }
      }
      if (value.control_value) {
         co_await handle_pubsub_control(session, *value.control_value, protocol);
      }
      for (const auto& subject : received.invalid_messages) {
         {
            const auto lock = std::scoped_lock{mutex};
            if (!pubsub_session_live_locked(session)) { co_return; }
            ++metrics_value.pubsub_messages_received;
         }
         if (!reject(subject, false)) { co_return; }
      }
      for (const auto& published : value.messages) {
         {
            const auto lock = std::scoped_lock{mutex};
            if (!pubsub_session_live_locked(session)) { co_return; }
            ++metrics_value.pubsub_messages_received;
         }
         auto signature_ok = true;
         const auto signed_message = !published.signature.empty();
         switch (wire_options.signatures) {
         case pubsub::signature_policy::strict_sign:
            signature_ok = pubsub::codec::verify_message(published, wire_options);
            break;
         case pubsub::signature_policy::strict_no_sign:
            signature_ok = !signed_message;
            break;
         case pubsub::signature_policy::lax_sign:
         case pubsub::signature_policy::lax_no_sign:
            signature_ok = !signed_message || pubsub::codec::verify_message(published, wire_options);
            break;
         }
         if (!signature_ok) {
            if (!reject(published.subject, false)) { co_return; }
            continue;
         }
         if (published.from && *published.from == local && peer != local) {
            // Donor SelfOrigin is P4, even after our local cache entry expires; never a malformed strike.
            if (!reject(published.subject, false)) { co_return; }
            continue;
         }
         const auto id = pubsub::codec::message_id(published, wire_options);
         const auto key = bytes_key(id);
         auto handler = std::optional<pubsub::handler>{};
         {
            const auto lock = std::scoped_lock{mutex};
            if (!pubsub_session_live_locked(session)) { co_return; }
            pubsub_value.router->fulfill(key); // Donor: a verified arrival fulfills promises before app validation.
            if (const auto found = pubsub_value.handlers.find(published.subject.value); found != pubsub_value.handlers.end()) {
               handler = found->second;
            }
         }
         const auto claim = claim_pubsub_message(peer, key, published, handler.has_value(), session);
         if (claim.status == pubsub_state::claim_status::invalid) {
            if (!reject(published.subject, true)) { co_return; }
            continue;
         }
         if (claim.status != pubsub_state::claim_status::claimed) {
            continue;
         }
         auto finish = [this, &peer, &handler](void*) noexcept {
            if (handler) {
               finish_pubsub_validation(peer);
            }
         };
         auto admission = std::unique_ptr<void, decltype(finish)>{this, finish};
         auto result = pubsub::validation_result::accept;
         if (handler) {
            try {
               result = co_await (*handler)(pubsub::event{.source = peer, .value = published});
            } catch (...) {
               admission.reset();
               defer_pubsub_message(key, claim.generation, session);
               throw;
            }
         }
         admission.reset();
         if (result == pubsub::validation_result::retry) {
            defer_pubsub_message(key, claim.generation, session);
            continue;
         }
         if (!complete_pubsub_message(key, claim.generation, result, session)) {
            continue;
         }
         trace_pubsub([&] {
            return pubsub::trace_event{.kind = pubsub::trace_kind::validation_committed, .peer = peer,
               .author = published.from, .subject = published.subject, .message_id = id, .seqno = published.seqno,
               .data = published.data, .generation = claim.generation, .result = result, .protocol = protocol,
               .session_id = session->id, .stream_id = stream.id()};
         });
         if (result == pubsub::validation_result::reject) {
            const auto lock = std::scoped_lock{mutex};
            if (!pubsub_session_live_locked(session)) { co_return; }
            ++metrics_value.pubsub_invalid_messages;
            if (const auto row = pubsub_value.scores.find(peer); row != pubsub_value.scores.end()) {
               ++row->second.invalid_messages;
            }
            continue; // P4 was committed exactly once by the score engine, not malformed-RPC policy.
         }
         if (result != pubsub::validation_result::accept) {
            continue; // Ignore neither delivers nor forwards.
         }
         if (handler) {
            {
               const auto lock = std::scoped_lock{mutex};
               if (!pubsub_session_live_locked(session)) { co_return; }
               ++metrics_value.pubsub_messages_delivered;
               if (const auto row = pubsub_value.scores.find(peer); row != pubsub_value.scores.end()) {
                  ++row->second.delivered_messages;
               }
            }
            trace_pubsub([&] {
               return pubsub::trace_event{.kind = pubsub::trace_kind::delivery, .peer = peer,
                  .author = published.from, .subject = published.subject, .message_id = id, .seqno = published.seqno,
                  .data = published.data, .generation = claim.generation, .result = result, .protocol = protocol,
                  .session_id = session->id, .stream_id = stream.id()};
            });
         }
         for (const auto& recipient : pubsub_forward_peers(published.subject.value, peer, published.from)) {
            {
               const auto lock = std::scoped_lock{mutex};
               if (!pubsub_session_live_locked(session)) { co_return; }
            }
            auto send_generation = std::optional<std::uint64_t>{};
            try {
               co_await send_pubsub_rpc(recipient, pubsub::rpc{.messages = {published}}, send_generation);
            } catch (const forge::exceptions::base& error) {
               record_pubsub_send_failure(recipient, error, send_generation);
            }
         }
      }
   }
}

} // namespace forge::net::p2p
