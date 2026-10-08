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
#include <exception>
#include <functional>
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
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/libp2p_identity_material.hxx"
#include "details/node_impl.hxx"

namespace forge::net::p2p {

boost::asio::awaitable<pubsub::subscription> node::async_subscribe(pubsub::topic subject, pubsub::handler handler) {
   if (subject.value.empty() || !handler) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub subscription requires topic and handler");
   }
   auto self = impl_;
   auto subscription = pubsub::subscription{.subscribe = true, .subject = std::move(subject)};
   if (subscription.subject.value.size() > self->options.limits.pubsub.limits.max_topic_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub topic exceeds configured byte limit");
   }
   {
      auto lock = std::scoped_lock{self->mutex};
      const auto local_subscription_limit =
          std::min(self->options.limits.pubsub.limits.max_topics, self->options.limits.pubsub.limits.max_subscriptions);
      if (self->pubsub_value.handlers.size() >= local_subscription_limit &&
          !self->pubsub_value.handlers.contains(subscription.subject.value)) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub topic limit reached");
      }
      const auto [mesh, inserted] = self->pubsub_value.mesh.try_emplace(subscription.subject.value);
      try {
         self->pubsub_value.handlers.insert_or_assign(subscription.subject.value, std::move(handler));
      } catch (...) {
         if (inserted) {
            self->pubsub_value.mesh.erase(mesh);
         }
         throw;
      }
      self->pubsub_value.fanout.erase(subscription.subject.value);
   }
   auto peers = self->pubsub_candidate_peers(subscription.subject.value);
   for (const auto& peer : peers) {
      auto send_generation = std::optional<std::uint64_t>{};
      try {
         co_await self->send_pubsub_rpc(peer,
                                        pubsub::rpc{.subscriptions = std::vector<pubsub::subscription>{subscription}},
                                        send_generation);
      } catch (const forge::exceptions::base& error) {
         self->record_pubsub_send_failure(peer, error, send_generation);
      }
   }
   co_return subscription;
}

boost::asio::awaitable<void> node::async_unsubscribe(pubsub::topic subject) {
   if (subject.value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub unsubscribe requires topic");
   }
   auto self = impl_;
   auto subscription = pubsub::subscription{.subscribe = false, .subject = std::move(subject)};
   if (!self->options.capabilities.has(capabilities::pubsub)) { co_return; }
   auto outbound = std::map<peer_id, pubsub::rpc>{};
   try {
      const auto px = self->prepare_pubsub_prune_peers();
      const auto peers = self->pubsub_candidate_peers(subscription.subject.value);
      for (const auto& peer : peers) {
         outbound[peer].subscriptions.push_back(subscription);
      }
      const auto lock = std::scoped_lock{self->mutex};
      if (self->stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot unsubscribe GossipSub after shutdown");
      }
      const auto now = std::chrono::steady_clock::now();
      auto commands = std::vector<detail::pubsub_control_queue::command>{};
      const auto mesh = self->pubsub_value.mesh.find(subscription.subject.value);
      if (mesh != self->pubsub_value.mesh.end()) {
         commands.reserve(mesh->second.size());
         for (const auto& peer : mesh->second) {
            commands.push_back(self->make_pubsub_control_locked(peer, subscription.subject,
                detail::pubsub_control_queue::kind::prune,
                self->options.limits.pubsub.limits.unsubscribe_backoff, px));
         }
      }
      auto change = self->prepare_pubsub_controls_locked(std::move(commands), now);
      if (!change) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub unsubscribe control queue is full");
      }
      // Every recipient, container node, backoff and immutable payload is prepared above.
      self->commit_pubsub_controls_locked(std::move(*change), now);
      self->pubsub_value.handlers.erase(subscription.subject.value);
      self->pubsub_value.mesh.erase(subscription.subject.value);
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub unsubscribe preparation allocation failed");
   } catch (const forge::exceptions::base& error) {
      if (!exceptions::is(error, exceptions::code::invalid_options)) { throw; }
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub unsubscribe control exceeds wire limit");
   }
   self->flush_pubsub_controls();
   for (auto& [peer, rpc] : outbound) {
      auto send_generation = std::optional<std::uint64_t>{};
      try {
         co_await self->send_pubsub_rpc(peer, std::move(rpc), send_generation);
      } catch (const forge::exceptions::base& error) {
         self->record_pubsub_send_failure(peer, error, send_generation);
      }
   }
   co_return;
}

boost::asio::awaitable<pubsub::message> node::async_publish(pubsub::topic subject, std::vector<std::uint8_t> data) {
   co_return co_await async_publish(std::move(subject), std::move(data), pubsub::publish_options{
       .sign = impl_->options.limits.pubsub.signatures != pubsub::signature_policy::strict_no_sign});
}

boost::asio::awaitable<pubsub::message> node::async_publish(pubsub::topic subject, std::vector<std::uint8_t> data,
                                                            pubsub::publish_options publish_options) {
   if (subject.value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub publish requires topic");
   }
   auto self = impl_;
   {
      auto lock = std::scoped_lock{self->mutex};
      if (self->stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot publish GossipSub message after node shutdown");
      }
   }
   if (subject.value.size() > self->options.limits.pubsub.limits.max_topic_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub topic exceeds configured byte limit");
   }
   if (data.size() > self->options.limits.pubsub.limits.max_data_size) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub publish exceeds max data size");
   }
   const auto no_auth = self->options.limits.pubsub.signatures == pubsub::signature_policy::strict_no_sign;
   if (no_auth && publish_options.sign) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub strict-no-sign node cannot publish signed messages");
   }
   auto value = pubsub::message{
       .data = std::move(data),
       .seqno = no_auth ? std::vector<std::uint8_t>{} : self->next_pubsub_seqno(),
       .subject = std::move(subject),
   };
   if (publish_options.sign) {
      pubsub::codec::sign_message(value, require_libp2p_identity_private_key(self->identity), self->options.limits.pubsub);
      if (!value.from || *value.from != self->local) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "GossipSub signing key does not match local Peer ID");
      }
   } else if (self->options.limits.pubsub.signatures == pubsub::signature_policy::strict_sign) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub strict-sign node cannot publish unsigned messages");
   }

   const auto id = pubsub::codec::message_id(value, self->options.limits.pubsub);
   {
      auto lock = std::scoped_lock{self->mutex};
      const auto key = bytes_key(id);
      self->remember_local_pubsub_message_locked(key, value);
   }
   self->increment_pubsub_published();

   auto attempted = std::size_t{};
   auto sent = std::size_t{};
   auto terminal_kind = std::optional<exceptions::code>{};
   auto terminal_message = std::string{};
   for (const auto& peer : self->pubsub_publish_peers(value.subject.value)) {
      ++attempted;
      auto send_generation = std::optional<std::uint64_t>{};
      try {
         co_await self->send_pubsub_rpc(peer, pubsub::rpc{.messages = std::vector<pubsub::message>{value}},
                                      send_generation);
         ++sent;
      } catch (const forge::exceptions::base& error) {
         const auto kind = p2p_code(error);
         if (!terminal_kind && (kind == exceptions::code::closed || kind == exceptions::code::canceled ||
                                kind == exceptions::code::timeout || kind == exceptions::code::backpressure_rejected)) {
            terminal_kind = kind;
            terminal_message = error.what();
         }
         self->record_pubsub_send_failure(peer, error, send_generation);
      }
   }
   if (attempted > 0 && sent == 0) {
      if (terminal_kind) {
         FORGE_THROW_CODE(*terminal_kind, terminal_message);
      }
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "GossipSub publish could not reach any candidate peer");
   }
   co_return value;
}

pubsub::snapshot node::pubsub_snapshot() const {
   return impl_->pubsub_snapshot();
}

pubsub::score_snapshot node::pubsub_scores() const {
   return impl_->pubsub_scores();
}

} // namespace forge::net::p2p
