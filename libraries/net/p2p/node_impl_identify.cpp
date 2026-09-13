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

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.rendezvous;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/host_addresses.hxx"
#include "details/node_impl.hxx"
#include "details/protocol_capabilities.hxx"

namespace forge::net::p2p {
namespace {

constexpr auto identify_peer_record_domain = std::string_view{"libp2p-peer-record"};
constexpr auto identify_peer_record_payload_type = std::array<std::uint8_t, 2>{0x03, 0x01};

[[nodiscard]] signed_envelope seal_identify_peer_record(const rendezvous::peer_record& value, const public_key& key,
                                                        const forge::crypto::asymmetric::private_key& private_key) {
   const auto payload = rendezvous::codec::encode_peer_record(value);
   return signed_envelope::seal(key, private_key, identify_peer_record_domain, identify_peer_record_payload_type,
                                payload);
}

[[nodiscard]] rendezvous::peer_record open_identify_peer_record(const signed_envelope& envelope,
                                                                std::optional<peer_id> expected_signer) {
   if (std::ranges::equal(envelope.payload_type, identify_peer_record_payload_type)) {
      envelope.verify(identify_peer_record_domain, expected_signer);
      auto out = rendezvous::codec::decode_peer_record(envelope.payload);
      if (out.peer != envelope.signer()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "Identify signed peer record peer id mismatch");
      }
      return out;
   }
   if (envelope.payload_type == rendezvous::codec::peer_record_payload_type()) {
      return rendezvous::codec::open_peer_record(envelope, std::move(expected_signer));
   }
   FORGE_THROW_EXCEPTION(exceptions::codec_error, "Identify signed peer record has unsupported payload type");
}

boost::asio::awaitable<identify::document>
read_identify_document(auto& self, forge::net::p2p::stream& stream,
                       const std::shared_ptr<detail::resource_stream>& resource) {
   if (!resource) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P Identify stream is missing its resource scope");
   }
   // Identify owns the aggregate protobuf and one framed part while decoding.
   // Charge both before reading so a peer cannot grow unaccounted decode state.
   auto aggregate_memory = resource->reserve_memory(self.options.identify.max_total_message_size,
                                                    resource_manager::memory_priority::always);
   auto framed_memory =
       resource->reserve_memory(self.options.identify.max_message_size, resource_manager::memory_priority::always);
   if (!aggregate_memory || !framed_memory) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P Identify decode memory limit reached");
   }
   auto buffer = std::vector<std::uint8_t>{};
   auto protobuf = std::vector<std::uint8_t>{};
   auto parts = std::size_t{};
   while (true) {
      try {
         auto framed = co_await async_read_length_delimited(stream, buffer, self.options.identify.max_message_size);
         if (parts >= self.options.identify.max_message_parts) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "Identify has too many message parts");
         }
         auto payload = unwrap_length_delimited(framed, self.options.identify.max_message_size);
         if (protobuf.size() > self.options.identify.max_total_message_size ||
             payload.size() > self.options.identify.max_total_message_size - protobuf.size()) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "Identify message parts exceed aggregate size");
         }
         protobuf.insert(protobuf.end(), payload.begin(), payload.end());
         ++parts;
      } catch (const forge::exceptions::base& error) {
         if (!is_clean_stream_eof(error)) {
            throw;
         }
         if (parts == 0 || !buffer.empty()) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "Identify stream ended before a complete message");
         }
         break;
      }
   }
   co_return identify::decode(protobuf, self.options.identify);
}

[[nodiscard]] std::uint64_t next_peer_record_sequence(std::uint64_t previous) {
   const auto now =
       std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
           .count();
   const auto clock_value = now > 0 ? static_cast<std::uint64_t>(now) : std::uint64_t{1};
   if (previous == (std::numeric_limits<std::uint64_t>::max)()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P Identify peer record sequence exhausted");
   }
   return std::max(clock_value, previous + 1);
}

boost::asio::awaitable<identify::document> exchange_identify(auto self, auto session) {
   auto strand = boost::asio::make_strand(self->runtime.context().get_executor());
   auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
   auto timed_out = std::make_shared<std::atomic_bool>(false);
   auto timer = std::make_shared<boost::asio::steady_timer>(strand);
   timer->expires_after(self->options.identify.timeout);
   timer->async_wait([cancellation, timed_out](const boost::system::error_code& error) {
      if (error) {
         return;
      }
      timed_out->store(true, std::memory_order_release);
      cancellation->emit(boost::asio::cancellation_type::all);
   });

   try {
      auto document = co_await boost::asio::co_spawn(
          strand,
          [self, session]() -> boost::asio::awaitable<identify::document> {
             auto resource = std::shared_ptr<detail::resource_stream>{};
             auto stream = co_await self->open_session_stream(
                 session, builtins::identify, false,
                 detail::stream_admission_handler{
                     [&resource](const std::shared_ptr<detail::resource_stream>& admitted) { resource = admitted; },
                     {}});
             auto document = co_await read_identify_document(*self, stream, resource);
             co_await stream.async_close();
             co_return document;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_awaitable));
      timer->cancel();
      if (timed_out->load(std::memory_order_acquire)) {
         throw_operation_timeout("P2P Identify");
      }
      co_return document;
   } catch (...) {
      timer->cancel();
      if (timed_out->load(std::memory_order_acquire)) {
         throw_operation_timeout("P2P Identify");
      }
      throw;
   }
}

[[nodiscard]] std::optional<rendezvous::peer_record>
verified_peer_record(const peer_id& peer, const identify::document& document,
                     const std::optional<peer_store::record>& previous) {
   if (document.signed_peer_record.empty()) {
      return std::nullopt;
   }
   auto record = open_identify_peer_record(signed_envelope::decode(document.signed_peer_record), peer);
   if (previous && !previous->signed_peer_record.empty()) {
      const auto known = open_identify_peer_record(signed_envelope::decode(previous->signed_peer_record), peer);
      if (record.sequence < known.sequence) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "Identify signed peer record sequence regressed");
      }
   }
   return record;
}

void verify_identify_identity(const peer_id& peer, const identify::document& document) {
   if (!document.public_key.empty() && make_peer_id(decode_public_key(document.public_key)) != peer) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "Identify public key does not match authenticated peer id");
   }
}

[[nodiscard]] std::vector<forge::multiformats::multiaddr>
validated_identify_addresses(const std::vector<forge::multiformats::multiaddr>& addresses, const peer_id& peer,
                             const host_addresses::learning_context& context) {
   return host_addresses::sanitize_discovered_addresses(addresses, peer, context);
}

[[nodiscard]] identify::document
local_identify_document_locked(const auto& self,
                               std::optional<forge::net::p2p::endpoint> observed_endpoint = std::nullopt) {
   auto& state = self.identify_push_value;
   if (state.cached_generation != state.generation) {
      auto listen_addresses = std::vector<forge::multiformats::multiaddr>{};
      for (const auto& endpoint : self.local_endpoints_for_control_locked()) {
         listen_addresses.push_back(endpoint.to_multiaddr());
      }
      auto document = identify::document{
          .protocol_version = self.options.protocol_version,
          .agent_version = self.options.agent_version,
          .public_key = self.identity.public_key,
          .listen_endpoints = std::move(listen_addresses),
          .protocols = self.supported_protocols_locked(),
      };
      state.peer_record_sequence = next_peer_record_sequence(state.peer_record_sequence);
      const auto all_endpoints = document.listen_endpoints;
      auto signature_size = std::size_t{};
      auto signing_key = std::optional<public_key>{};
      if (self.identity.private_key && !self.identity.public_key.empty()) {
         signing_key = decode_public_key(self.identity.public_key);
         auto envelope = seal_identify_peer_record(
             rendezvous::peer_record{
                 .peer = self.local,
                 .endpoints = document.listen_endpoints,
                 .sequence = state.peer_record_sequence,
             },
             *signing_key, *self.identity.private_key);
         signature_size = envelope.signature.size();
         document.signed_peer_record = envelope.encode();
      }
      const auto encoded_size = [&]() {
         if (signing_key) {
            const auto payload = rendezvous::codec::encode_peer_record(rendezvous::peer_record{
                .peer = self.local,
                .endpoints = document.listen_endpoints,
                .sequence = state.peer_record_sequence,
            });
            document.signed_peer_record =
                signed_envelope{
                    .key = *signing_key,
                    .payload_type = std::vector<std::uint8_t>{identify_peer_record_payload_type.begin(),
                                                              identify_peer_record_payload_type.end()},
                    .payload = payload,
                    .signature = std::vector<std::uint8_t>(signature_size),
                }
                    .encode();
         }
         return identify::encode(document).size();
      };
      if (identify::encode(document).size() > self.options.identify.max_own_message_size && !all_endpoints.empty()) {
         auto lower = std::size_t{};
         auto upper = all_endpoints.size();
         while (lower < upper) {
            const auto candidate = lower + (upper - lower + 1) / 2;
            document.listen_endpoints.assign(all_endpoints.begin(),
                                             all_endpoints.begin() + static_cast<std::ptrdiff_t>(candidate));
            if (encoded_size() <= self.options.identify.max_own_message_size) {
               lower = candidate;
            } else {
               upper = candidate - 1;
            }
         }
         document.listen_endpoints.assign(all_endpoints.begin(),
                                          all_endpoints.begin() + static_cast<std::ptrdiff_t>(lower));
         if (signing_key) {
            document.signed_peer_record = seal_identify_peer_record(
                                              rendezvous::peer_record{
                                                  .peer = self.local,
                                                  .endpoints = document.listen_endpoints,
                                                  .sequence = state.peer_record_sequence,
                                              },
                                              *signing_key, *self.identity.private_key)
                                              .encode();
         }
      }
      state.cached_document = std::move(document);
      state.cached_generation = state.generation;
   }
   auto document = state.cached_document;
   if (observed_endpoint) {
      document.observed_endpoint = std::move(observed_endpoint);
      if (identify::encode(document).size() > self.options.identify.max_own_message_size) {
         document.observed_endpoint.reset();
      }
   }
   return document;
}

template <typename Operation>
boost::asio::awaitable<void> run_identify_operation_with_timeout(auto self, std::string_view name,
                                                                 Operation operation) {
   auto strand = boost::asio::make_strand(self->runtime.context().get_executor());
   auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
   auto timed_out = std::make_shared<std::atomic_bool>(false);
   auto timer = std::make_shared<boost::asio::steady_timer>(strand);
   timer->expires_after(self->options.identify.timeout);
   timer->async_wait([cancellation, timed_out](const boost::system::error_code& error) {
      if (error) {
         return;
      }
      timed_out->store(true, std::memory_order_release);
      cancellation->emit(boost::asio::cancellation_type::all);
   });

   try {
      co_await boost::asio::co_spawn(
          strand,
          [operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
             co_await std::move(operation)();
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_awaitable));
      timer->cancel();
      if (timed_out->load(std::memory_order_acquire)) {
         throw_operation_timeout(name);
      }
   } catch (...) {
      timer->cancel();
      if (timed_out->load(std::memory_order_acquire)) {
         throw_operation_timeout(name);
      }
      throw;
   }
}

[[nodiscard]] bool same_endpoints(const std::vector<forge::net::p2p::endpoint>& lhs,
                                  const std::vector<forge::net::p2p::endpoint>& rhs) {
   return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, {}, &forge::net::p2p::endpoint::to_string,
                                                         &forge::net::p2p::endpoint::to_string);
}

} // namespace

identify::document
node::impl::local_identify_document(std::optional<forge::net::p2p::endpoint> observed_endpoint) const {
   auto lock = std::scoped_lock{mutex};
   return local_identify_document_locked(*this, std::move(observed_endpoint));
}

void node::impl::validate_local_identify_document() const {
   const auto encoded = identify::encode(local_identify_document());
   if (encoded.size() > options.identify.max_own_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "base P2P Identify document exceeds the configured outbound limit");
   }
   try {
      static_cast<void>(identify::decode(encoded, options.identify));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "base P2P Identify document violates the configured Identify limits");
   }
}

node::impl::identify_snapshot node::impl::local_identify_snapshot() const {
   auto lock = std::scoped_lock{mutex};
   return identify_snapshot{
       .generation = identify_push_value.generation,
       .document = local_identify_document_locked(*this),
   };
}

void node::impl::register_protocol_handler(protocol_id protocol, node::protocol_handler handler) {
   if (protocol.value.empty() || protocol.value.front() != '/' || !handler) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P protocol handler requires protocol id and handler");
   }
   require_private_protocol_allowed(protocol);
   auto launch = false;
   {
      auto lock = std::scoped_lock{mutex};
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
      }
      if (handlers.size() >= options.limits.max_protocol_handlers) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P max protocol handlers reached");
      }
      if (handlers.contains(protocol)) {
         FORGE_THROW_EXCEPTION(exceptions::duplicate_protocol, "duplicate P2P protocol handler");
      }
      if (protocol.value.size() > options.identify.max_protocol_size) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P protocol id exceeds the Identify protocol limit");
      }

      auto candidate = local_identify_document_locked(*this);
      candidate.protocols.push_back(protocol);
      if (candidate.protocols.size() > options.identify.max_protocols) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                               "P2P protocol handler would exceed the Identify protocol count limit");
      }
      auto encoded = identify::encode(candidate);
      if (encoded.size() > options.identify.max_own_message_size) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                               "P2P protocol handler would exceed the local Identify message limit");
      }
      static_cast<void>(identify::decode(encoded, options.identify));

      handlers.emplace(std::move(protocol), std::move(handler));
      launch = advance_identify_generation_locked() && schedule_identify_push_locked();
   }
   if (launch) {
      launch_identify_pushes();
   }
}

bool node::impl::unregister_protocol_handler(const protocol_id& protocol) {
   auto launch = false;
   auto removed = false;
   {
      auto lock = std::scoped_lock{mutex};
      removed = handlers.erase(protocol) != 0;
      if (removed) {
         launch = advance_identify_generation_locked() && schedule_identify_push_locked();
      }
   }
   if (launch) {
      launch_identify_pushes();
   }
   return removed;
}

void node::impl::set_advertised_endpoints(std::vector<forge::net::p2p::endpoint> endpoints) {
   auto launch = false;
   auto endpoints_changed = false;
   {
      auto lock = std::scoped_lock{mutex};
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
      }
      const auto previous = local_endpoints_for_control_locked();
      options.advertised_endpoints = std::move(endpoints);
      refresh_reachability_locked();
      if (!same_endpoints(previous, local_endpoints_for_control_locked())) {
         endpoints_changed = true;
         launch = advance_identify_generation_locked() && schedule_identify_push_locked();
      }
   }
   if (endpoints_changed && provider_registry) {
      provider_registry->notify_endpoints_changed();
   }
   if (endpoints_changed) {
      notify_reachability_changed();
   }
   if (launch) {
      launch_identify_pushes();
   }
}

void node::impl::notify_listen_endpoints_changed() {
   auto launch = false;
   {
      auto lock = std::scoped_lock{mutex};
      refresh_reachability_locked();
      launch = advance_identify_generation_locked() && schedule_identify_push_locked();
   }
   if (provider_registry) {
      provider_registry->notify_endpoints_changed();
   }
   notify_reachability_changed();
   if (launch) {
      launch_identify_pushes();
   }
}

bool node::impl::advance_identify_generation_locked() noexcept {
   if (identify_push_value.generation == (std::numeric_limits<std::uint64_t>::max)()) {
      return false;
   }
   ++identify_push_value.generation;
   return true;
}

bool node::impl::schedule_identify_push_locked() noexcept {
   if (stopped || identify_push_value.coordinator_running) {
      return false;
   }
   const auto pending = std::ranges::any_of(sessions, [&](const auto& entry) {
      const auto& session = entry.second;
      return !session->closed && session->identify_push_supported &&
             session->identify_push_attempted_generation < identify_push_value.generation;
   });
   if (!pending) {
      return false;
   }
   identify_push_value.coordinator_running = true;
   return true;
}

void node::impl::launch_identify_pushes() {
   auto self = shared_from_this();
   if (launch_tracked([self]() mutable -> boost::asio::awaitable<void> {
          try {
             co_await self->run_identify_pushes();
          } catch (...) {
             auto lock = std::scoped_lock{self->mutex};
             self->identify_push_value.coordinator_running = false;
          }
       })) {
      return;
   }
   auto lock = std::scoped_lock{mutex};
   identify_push_value.coordinator_running = false;
}

boost::asio::awaitable<void> node::impl::run_identify_pushes() {
   while (true) {
      const auto snapshot = local_identify_snapshot();
      auto document = std::make_shared<const identify::document>(snapshot.document);
      if (identify::encode(*document).size() > options.identify.max_own_message_size) {
         auto lock = std::scoped_lock{mutex};
         identify_push_value.coordinator_running = false;
         co_return;
      }

      auto batch = std::vector<std::shared_ptr<session_state>>{};
      {
         auto lock = std::scoped_lock{mutex};
         batch.reserve(options.identify.max_push_operations);
         for (const auto& [_, session] : sessions) {
            if (batch.size() >= options.identify.max_push_operations) {
               break;
            }
            if (session->closed || !session->identify_push_supported ||
                session->identify_push_attempted_generation >= snapshot.generation) {
               continue;
            }
            session->identify_push_attempted_generation = snapshot.generation;
            batch.push_back(session);
         }
         if (batch.empty() && identify_push_value.generation == snapshot.generation) {
            identify_push_value.coordinator_running = false;
            co_return;
         }
      }
      if (batch.empty()) {
         continue;
      }

      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      using completion_channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>;
      auto completions = std::make_shared<completion_channel>(runtime.context().get_executor(), batch.size());
      for (const auto& session : batch) {
         auto self = shared_from_this();
         try {
            boost::asio::co_spawn(
                runtime.context(),
                [self, session, generation = snapshot.generation, document,
                 completions]() -> boost::asio::awaitable<void> {
                   try {
                      co_await self->send_identify_push(session, generation, document);
                   } catch (...) {
                      // A failed Push is attributable to this stream/session and must not stop the node.
                   }
                   static_cast<void>(completions->try_send(boost::system::error_code{}));
                },
                boost::asio::detached);
         } catch (...) {
            // A synchronous launch failure still consumes this batch slot so started Pushes are joined.
            static_cast<void>(completions->try_send(boost::system::error_code{}));
         }
      }
      for (auto completed = std::size_t{}; completed < batch.size(); ++completed) {
         co_await completions->async_receive(boost::asio::use_awaitable);
      }
   }
}

boost::asio::awaitable<void> node::impl::send_identify_push(const std::shared_ptr<session_state>& session,
                                                            std::uint64_t generation,
                                                            std::shared_ptr<const identify::document> document) {
   auto outgoing = *document;
   {
      auto lock = std::scoped_lock{mutex};
      const auto found = sessions.find(session->id);
      if (found == sessions.end() || found->second != session || session->closed) {
         co_return;
      }
      outgoing.observed_endpoint = session->remote_endpoint;
      if (identify::encode(outgoing).size() > options.identify.max_own_message_size) {
         outgoing.observed_endpoint.reset();
      }
   }
   auto encoded = std::make_shared<const std::vector<std::uint8_t>>(wrap_length_delimited(identify::encode(outgoing)));
   if (identify::encode(outgoing).size() > options.identify.max_own_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "local Identify Push document exceeds configured limit");
   }

   auto self = shared_from_this();
   co_await run_identify_operation_with_timeout(
       self, "P2P Identify Push", [self, session, encoded]() -> boost::asio::awaitable<void> {
          auto stream = co_await self->open_session_stream(session, builtins::identify_push);
          co_await stream.async_write(*encoded);
          co_await stream.async_close();
       });

   auto lock = std::scoped_lock{mutex};
   const auto found = sessions.find(session->id);
   if (found != sessions.end() && found->second == session && !session->closed) {
      session->identify_push_delivered_generation = std::max(session->identify_push_delivered_generation, generation);
   }
}

void node::impl::learn_from_identify(const std::shared_ptr<session_state>& session, const identify::document& document,
                                     bool received_push) {
   const auto peer = session->info.remote_peer;
   verify_identify_identity(peer, document);
   const auto context = host_addresses::learning_context{
       .source = host_addresses::source_kind::authenticated,
       .remote_endpoint = session->remote_endpoint,
   };
   auto launch = false;
   auto verified_dht_server = false;
   {
      auto lock = std::scoped_lock{mutex};
      const auto found = sessions.find(session->id);
      if (found == sessions.end() || found->second != session || session->closed) {
         return;
      }

      auto certified = std::optional<rendezvous::peer_record>{};
      auto certified_error = std::string{};
      const auto previous = store.find(peer);
      try {
         certified = verified_peer_record(peer, document, previous);
      } catch (const forge::exceptions::base& error) {
         certified_error = error.what();
      }
      auto update = peer_store::identify_update{};
      if (!received_push || document.present.protocol_version) {
         update.protocol_version = document.protocol_version;
      }
      if (!received_push || document.present.agent_version) {
         update.agent_version = document.agent_version;
      }
      if (!received_push || document.present.public_key) {
         update.public_key = document.public_key;
      }
      if (!received_push || !document.protocols.empty()) {
         update.protocols = document.protocols;
         update.capabilities = capabilities_for(document.protocols);
      }
      if (!received_push || document.present.observed_endpoint) {
         update.replace_observed_endpoint = true;
         update.observed_endpoint = document.observed_endpoint;
      }
      if (certified) {
         update.signed_peer_record = document.signed_peer_record;
         update.signed_endpoints = validated_identify_addresses(certified->endpoints, peer, context);
      } else if (!received_push || !document.listen_endpoints.empty()) {
         update.unsigned_endpoints = validated_identify_addresses(document.listen_endpoints, peer, context);
      }
      const auto record = store.apply_identify(peer, std::move(update));
      const auto remote_capabilities = capabilities_for(record.protocols);
      const auto supports_identify_push =
          std::ranges::find(record.protocols, builtins::identify_push) != record.protocols.end();
      auto routing_peer = dht::peer{.id = peer, .connection = dht::connection_type::can_connect};
      routing_peer.endpoints.reserve(record.endpoints.size());
      for (const auto& endpoint : record.endpoints) {
         routing_peer.endpoints.push_back(endpoint.address);
      }

      for (auto& [protocol, state] : dht_profiles) {
         if (std::ranges::find(record.protocols, protocol) != record.protocols.end()) {
            state->routing.upsert(routing_peer, dht::routing_admission::verified_server);
            verified_dht_server = true;
         } else {
            state->routing.remove(peer);
         }
      }

      if (!received_push) {
         session->info.capabilities = remote_capabilities;
         session->info.identify_state = identify::state::identified;
         session->remote_protocols = record.protocols;
         session->identify_error = std::move(certified_error);
      } else if (session->info.identify_state == identify::state::identified) {
         session->info.capabilities = remote_capabilities;
         session->remote_protocols = record.protocols;
         if (!certified_error.empty()) {
            session->identify_error = std::move(certified_error);
         }
      }
      session->identify_push_supported = supports_identify_push;
      if (!received_push && session->identify_push_supported) {
         launch = schedule_identify_push_locked();
      }
   }
   observe_address(session, document);
   notify_reachability_changed();
   if (verified_dht_server) {
      notify_dht_routing_refresh();
   }
   if (launch) {
      launch_identify_pushes();
   }
}

boost::asio::awaitable<void> node::impl::identify_session(const std::shared_ptr<session_state>& session) {
   auto unavailable = false;
   {
      auto lock = std::scoped_lock{mutex};
      if (session->closed || stopped) {
         session->info.capabilities = {};
         session->info.identify_state = identify::state::failed;
         session->identify_error = "P2P session closed before Identify";
         unavailable = true;
      } else if (session->info.identify_state == identify::state::identified ||
                 session->info.identify_state == identify::state::failed) {
         co_return;
      } else {
         session->info.identify_state = identify::state::identifying;
      }
   }
   if (unavailable) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P session closed before Identify");
   }

   auto self = shared_from_this();
   auto result = co_await identify_service.async_identify(
       session->id, [self, session]() -> boost::asio::awaitable<identify::document> {
          auto document = co_await exchange_identify(self, session);
          self->learn_from_identify(session, document);
          co_return document;
       });
   if (result.state == identify::state::identified) {
      auto lock = std::unique_lock{mutex};
      const auto found = sessions.find(session->id);
      if (!stopped && found != sessions.end() && found->second == session && !session->closed &&
          session->info.identify_state == identify::state::identified) {
         co_return;
      }
      session->info.capabilities = {};
      session->info.identify_state = identify::state::failed;
      session->identify_error = "P2P session closed during Identify";
      lock.unlock();
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P session closed during Identify");
   }

   auto session_closed = false;
   {
      auto lock = std::scoped_lock{mutex};
      const auto found = sessions.find(session->id);
      session_closed = stopped || found == sessions.end() || found->second != session || session->closed;
      if (session->info.identify_state == identify::state::identifying) {
         session->info.capabilities = {};
         session->info.identify_state = identify::state::failed;
         session->identify_error = std::move(result.error);
      }
   }
   if (session_closed) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P session closed during Identify");
   }
}

void node::impl::launch_identify(const std::shared_ptr<session_state>& session) {
   auto operation = lifecycle.track();
   if (!operation.active()) {
      return;
   }
   auto self = shared_from_this();
   const auto executor = operation.executor();
   boost::asio::co_spawn(
       executor,
       [self, session]() mutable -> boost::asio::awaitable<void> {
          try {
             co_await self->identify_session(session);
          } catch (...) {
             // The Identify state records attributable failure without terminating the authenticated session.
          }
       },
       [operation = std::move(operation)](std::exception_ptr error) mutable {
          static_cast<void>(error);
          operation.release();
       });
}

boost::asio::awaitable<std::optional<identify::document>>
node::impl::identify_peer_for_discovery(const peer_id& peer, discovery::source source,
                                        std::chrono::milliseconds timeout) {
   auto session = co_await ensure_direct_session(peer, timeout);
   co_await identify_session(session);
   {
      auto lock = std::scoped_lock{mutex};
      if (session->info.identify_state != identify::state::identified) {
         co_return std::nullopt;
      }
   }
   const auto discovered_at = std::chrono::system_clock::now();
   const auto record =
       store.apply_discovery(peer, peer_store::discovery_update{
                                       .source = source,
                                       .observed_at = discovered_at,
                                       .expires_at = discovered_at + options.limits.topology.refresh_interval,
                                   });
   if (!record) {
      co_return std::nullopt;
   }
   co_return identify::document{
       .protocol_version = record->protocol_version,
       .agent_version = record->agent_version,
       .public_key = record->public_key,
       .protocols = record->protocols,
       .signed_peer_record = record->signed_peer_record,
   };
}

boost::asio::awaitable<void> node::impl::handle_identify(std::shared_ptr<session_state> session,
                                                         forge::net::p2p::stream stream) {
   auto encoded = identify::encode(local_identify_document(session->remote_endpoint));
   if (encoded.size() > options.identify.max_own_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "local Identify document exceeds configured limit");
   }
   co_await stream.async_write(wrap_length_delimited(encoded));
   co_await stream.async_close();
}

boost::asio::awaitable<void> node::impl::handle_identify_push(std::shared_ptr<session_state> session,
                                                              forge::net::p2p::stream stream,
                                                              std::shared_ptr<detail::resource_stream> resource) {
   auto self = shared_from_this();
   co_await run_identify_operation_with_timeout(
       self, "P2P Identify Push",
       [self, session = std::move(session), stream = std::move(stream),
        resource = std::move(resource)]() mutable -> boost::asio::awaitable<void> {
          self->learn_from_identify(session, co_await read_identify_document(*self, stream, resource), true);
          co_await stream.async_close();
       });
}

} // namespace forge::net::p2p
