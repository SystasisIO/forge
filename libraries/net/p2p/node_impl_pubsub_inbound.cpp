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

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error);
[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept;

boost::asio::awaitable<std::vector<std::uint8_t>> async_read_length_delimited(forge::net::p2p::stream& stream,
                                                                              std::vector<std::uint8_t>& buffer,
                                                                              std::size_t max_payload_size);

[[nodiscard]] std::string bytes_key(std::span<const std::uint8_t> bytes) {
   return {bytes.begin(), bytes.end()};
}

bool node::impl::pubsub_control_over_limit(const pubsub::control& value) const noexcept {
   return value.have.size() > options.limits.pubsub.limits.max_ihave_per_peer ||
          value.want.size() > options.limits.pubsub.limits.max_iwant_per_peer ||
          value.grafts.size() > options.limits.pubsub.limits.max_graft_per_peer;
}

bool node::impl::record_pubsub_subscription_locked(const peer_id& peer, const std::string& topic) {
   if (const auto topics = pubsub_value.peer_topics.find(peer);
       topics != pubsub_value.peer_topics.end() && topics->second.contains(topic)) {
      return true;
   }
   const auto subscribed = static_cast<std::size_t>(
       std::ranges::count_if(pubsub_value.peer_topics,
                             [&topic](const auto& peer_topics) { return peer_topics.second.contains(topic); }));
   if (subscribed >= options.limits.pubsub.limits.max_peers_per_topic) {
      return false;
   }
   pubsub_value.peer_topics[peer].insert(topic);
   return true;
}

void node::impl::penalize_pubsub_backoff_violation(const peer_id& peer) {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.pubsub_invalid_messages;
   ++pubsub_value.scores[peer].invalid_messages;
   pubsub_value.scores[peer].value -= 1.0;
}

boost::asio::awaitable<void> node::impl::handle_pubsub(std::shared_ptr<node::impl::session_state> session,
                                                       forge::net::p2p::stream stream) {
   if (!options.capabilities.has(capabilities::pubsub)) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "GossipSub is disabled");
   }

   auto generation = std::uint64_t{};
   {
      auto lock = std::scoped_lock{mutex};
      generation = pubsub_value.next_inbound_generation++;
      pubsub_value.inbound[session->info.remote_peer].emplace(generation, session->id);
   }
   try {
      co_await handle_pubsub_stream(session, std::move(stream));
   } catch (...) {
      finish_pubsub_inbound(session->info.remote_peer, generation);
      throw;
   }
   finish_pubsub_inbound(session->info.remote_peer, generation);
}

boost::asio::awaitable<void> node::impl::handle_pubsub_stream(std::shared_ptr<node::impl::session_state> session,
                                                              forge::net::p2p::stream stream) {

   auto buffer = std::vector<std::uint8_t>{};
   while (true) {
      auto payload = std::vector<std::uint8_t>{};
      auto close_after_error = false;
      try {
         payload = co_await async_read_length_delimited(stream, buffer, options.limits.pubsub.limits.max_rpc_size);
      } catch (const forge::exceptions::base& error) {
         auto closed_by_node = false;
         {
            auto lock = std::scoped_lock{mutex};
            closed_by_node = stopped || session->closed;
         }
         if (closed_by_node || is_orderly_stream_close(error)) {
            co_return;
         }
         increment_pubsub_invalid(session->info.remote_peer);
         increment_protocol_rejected();
         close_after_error = true;
      }
      if (close_after_error) {
         stream.cancel();
         co_return;
      }

      auto value = pubsub::rpc{};
      close_after_error = false;
      try {
         value = pubsub::codec::decode(payload, options.limits.pubsub);
      } catch (const forge::exceptions::base&) {
         increment_pubsub_invalid(session->info.remote_peer);
         increment_protocol_rejected();
         close_after_error = true;
      }
      if (close_after_error) {
         stream.cancel();
         co_return;
      }

      if (!value.subscriptions.empty()) {
         auto subscription_limit_reached = false;
         {
            auto lock = std::scoped_lock{mutex};
            for (const auto& subscription : value.subscriptions) {
               if (subscription.subscribe) {
                  if (!record_pubsub_subscription_locked(session->info.remote_peer, subscription.subject.value)) {
                     subscription_limit_reached = true;
                  }
               } else {
                  if (auto topics = pubsub_value.peer_topics.find(session->info.remote_peer);
                      topics != pubsub_value.peer_topics.end()) {
                     topics->second.erase(subscription.subject.value);
                     if (topics->second.empty()) {
                        pubsub_value.peer_topics.erase(topics);
                     }
                  }
                  if (auto mesh = pubsub_value.mesh.find(subscription.subject.value); mesh != pubsub_value.mesh.end()) {
                     mesh->second.erase(session->info.remote_peer);
                  }
               }
            }
            if (subscription_limit_reached) {
               ++metrics_value.backpressure_rejections;
               ++metrics_value.protocol_rejections;
            }
         }
      }

      if (value.control_value) {
         increment_pubsub_control();
         if (pubsub_control_over_limit(*value.control_value)) {
            increment_pubsub_invalid(session->info.remote_peer);
            increment_protocol_rejected();
            stream.cancel();
            co_return;
         }
         auto missing = std::vector<std::vector<std::uint8_t>>{};
         auto cached = std::vector<pubsub::message>{};
         auto rejected_grafts = std::vector<pubsub::control::prune>{};
         auto capacity_rejected = false;
         auto backoff_violations = std::size_t{};
         {
            auto lock = std::scoped_lock{mutex};
            const auto now = std::chrono::steady_clock::now();
            const auto backoff_limit = detail::pubsub_backoff::limit_for(
                options.limits.pubsub.limits.max_topics, options.limits.max_sessions);
            pubsub_value.backoffs.expire(now);
            for (const auto& graft : value.control_value->grafts) {
               if (!pubsub_value.handlers.contains(graft.subject.value)) {
                  continue;
               }
               const auto local_backoff =
                   pubsub_value.backoffs.local_status(graft.subject.value, session->info.remote_peer, now);
               const auto remote_backoff =
                   pubsub_value.backoffs.remote_status(graft.subject.value, session->info.remote_peer, now);
               if (local_backoff != detail::pubsub_backoff::status::none ||
                   remote_backoff != detail::pubsub_backoff::status::none) {
                  if (local_backoff == detail::pubsub_backoff::status::exact ||
                      remote_backoff == detail::pubsub_backoff::status::exact) {
                     ++backoff_violations;
                  }
                  pubsub_value.backoffs.record_local(graft.subject.value, session->info.remote_peer,
                                                      options.limits.pubsub.limits.prune_backoff, now, backoff_limit);
                  rejected_grafts.push_back(pubsub::control::prune{
                      .subject = graft.subject,
                      .backoff = options.limits.pubsub.limits.prune_backoff,
                  });
               } else if (record_pubsub_subscription_locked(session->info.remote_peer, graft.subject.value)) {
                  pubsub_value.mesh[graft.subject.value].insert(session->info.remote_peer);
               } else {
                  capacity_rejected = true;
                  pubsub_value.backoffs.record_local(graft.subject.value, session->info.remote_peer,
                                                      options.limits.pubsub.limits.prune_backoff, now, backoff_limit);
                  rejected_grafts.push_back(pubsub::control::prune{
                      .subject = graft.subject,
                      .backoff = options.limits.pubsub.limits.prune_backoff,
                  });
               }
            }
            for (const auto& prune : value.control_value->prunes) {
               const auto remote_backoff = detail::pubsub_backoff::remote_duration(
                   prune.backoff, options.limits.pubsub.limits.prune_backoff);
               pubsub_value.backoffs.record_remote(prune.subject.value, session->info.remote_peer, remote_backoff, now,
                                                    backoff_limit);
               if (auto mesh = pubsub_value.mesh.find(prune.subject.value); mesh != pubsub_value.mesh.end()) {
                  mesh->second.erase(session->info.remote_peer);
               }
            }
            for (const auto& ihave : value.control_value->have) {
               if (!pubsub_value.handlers.contains(ihave.subject.value)) {
                  continue;
               }
               const auto now = std::chrono::steady_clock::now();
               for (const auto& id : ihave.message_ids) {
                  const auto key = bytes_key(id);
                  const auto cached_message = pubsub_value.cache.find(key);
                  if (cached_message != pubsub_value.cache.end() && cached_message->second.subject != ihave.subject) {
                     continue;
                  }
                  if (should_request_pubsub_message_locked(key, session->info.remote_peer, now)) {
                     missing.push_back(id);
                  }
               }
            }
            for (const auto& iwant : value.control_value->want) {
               for (const auto& id : iwant.message_ids) {
                  const auto key = bytes_key(id);
                  if (const auto found = pubsub_value.cache.find(key);
                      found != pubsub_value.cache.end() && can_serve_pubsub_message_locked(key)) {
                     cached.push_back(found->second);
                  }
               }
            }
         }
         if (capacity_rejected) {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.backpressure_rejections;
            ++metrics_value.protocol_rejections;
         }
         for (auto violation = std::size_t{}; violation < backoff_violations; ++violation) {
            penalize_pubsub_backoff_violation(session->info.remote_peer);
         }
         if (backoff_violations != 0) {
            increment_protocol_rejected();
         }
         if (!rejected_grafts.empty()) {
            try {
               co_await send_pubsub_rpc(
                   session->info.remote_peer,
                   pubsub::rpc{.control_value = pubsub::control{.prunes = std::move(rejected_grafts)}});
            } catch (const forge::exceptions::base& error) {
               record_pubsub_send_failure(session->info.remote_peer, error);
            }
         }
         if (!missing.empty()) {
            try {
               co_await send_pubsub_rpc(
                   session->info.remote_peer,
                   pubsub::rpc{.control_value =
                                   pubsub::control{.want = std::vector<pubsub::control::iwant>{
                                                       pubsub::control::iwant{.message_ids = std::move(missing)}}}});
            } catch (const forge::exceptions::base&) {
               increment_protocol_rejected();
            }
         }
         if (!cached.empty()) {
            try {
               co_await send_pubsub_rpc(session->info.remote_peer, pubsub::rpc{.messages = std::move(cached)});
            } catch (const forge::exceptions::base&) {
               increment_protocol_rejected();
            }
         }
      }

      for (const auto& published : value.messages) {
         increment_pubsub_received();

         auto signature_ok = true;
         const auto signed_message = !published.signature.empty();
         switch (options.limits.pubsub.signatures) {
         case pubsub::signature_policy::strict_sign:
            signature_ok = pubsub::codec::verify_message(published);
            break;
         case pubsub::signature_policy::strict_no_sign:
            signature_ok = !signed_message;
            break;
         case pubsub::signature_policy::lax_sign:
         case pubsub::signature_policy::lax_no_sign:
            signature_ok = !signed_message || pubsub::codec::verify_message(published);
            break;
         }
         if (!signature_ok) {
            increment_pubsub_invalid(session->info.remote_peer);
            continue;
         }

         const auto id = pubsub::codec::message_id(published);
         const auto key = bytes_key(id);
         auto handler = std::optional<pubsub::handler>{};
         {
            auto lock = std::scoped_lock{mutex};
            if (const auto found = pubsub_value.handlers.find(published.subject.value);
                found != pubsub_value.handlers.end()) {
               handler = found->second;
            }
         }
         const auto claim = claim_pubsub_message(session->info.remote_peer, key, published, handler.has_value());
         if (claim.status == pubsub_state::claim_status::invalid) {
            increment_pubsub_invalid(session->info.remote_peer);
            increment_protocol_rejected();
            continue;
         }
         if (claim.status == pubsub_state::claim_status::duplicate ||
             claim.status == pubsub_state::claim_status::backpressured) {
            continue;
         }

         auto result = pubsub::validation_result::accept;
         if (handler) {
            try {
               result = co_await (*handler)(pubsub::event{
                   .source = session->info.remote_peer,
                   .value = published,
               });
               finish_pubsub_validation(session->info.remote_peer);
            } catch (...) {
               finish_pubsub_validation(session->info.remote_peer);
               defer_pubsub_message(key, claim.generation);
               throw;
            }
         }
         if (result == pubsub::validation_result::retry) {
            defer_pubsub_message(key, claim.generation);
            continue;
         }
         if (handler && !complete_pubsub_message(key, claim.generation, result)) {
            continue;
         }
         if (result == pubsub::validation_result::reject) {
            increment_pubsub_invalid(session->info.remote_peer);
            continue;
         }
         if (result == pubsub::validation_result::accept && handler) {
            increment_pubsub_delivered();
         }

         auto should_forward = false;
         {
            auto lock = std::scoped_lock{mutex};
            should_forward = pubsub_value.handlers.contains(published.subject.value) ||
                             pubsub_value.mesh.contains(published.subject.value);
         }
         if (!should_forward) {
            continue;
         }
         for (const auto& peer : pubsub_candidate_peers(published.subject.value, session->info.remote_peer)) {
            try {
               co_await send_pubsub_rpc(peer, pubsub::rpc{.messages = std::vector<pubsub::message>{published}});
            } catch (const forge::exceptions::base&) {
               increment_protocol_rejected();
            }
         }
      }
   }
}

} // namespace forge::net::p2p
