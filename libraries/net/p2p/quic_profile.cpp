module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.asio.notification;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.resource_manager;
import forge.multiformats.exceptions;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.multiformats.multibase;
import forge.multiformats.multiaddr;
import forge.net.quic.connection;
import forge.net.quic.connector;
import forge.net.quic.endpoint;
import forge.net.quic.exceptions;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.security;
import forge.net.quic.transport;
import forge.net.transport.limits;
import forge.net.transport.session;
import forge.net.p2p.stream;
import forge.net.tcp.connection;
import forge.net.yamux.session;

#include "details/direct_transport.hxx"
#include "details/coordinated_dial.hxx"
#include "details/cancellation_latch.hxx"
#include "details/connection_gate.hxx"
#include "details/owner_cancellation.hxx"
#include "details/pending_quic_connection.hxx"
#include "details/quic_client_token_cache.hxx"
#include "details/quic_client_options.hxx"
#include "details/operation_deadline.hxx"
#include "details/stream_upgrade.hxx"
#include "details/quic_profile.hxx"

namespace forge::net::p2p::direct {

namespace {

[[nodiscard]] forge::net::quic::transport_limits quic_limits(const forge::net::transport::limits& value) noexcept {
   return forge::net::quic::transport_limits{
       .max_connections = value.max_connections,
       .max_streams_per_connection = value.max_streams_per_connection,
       .max_queued_bytes = value.max_queued_bytes,
       .max_inbound_queued_bytes = value.max_inbound_queued_bytes,
       .max_inbound_queued_packets = value.max_inbound_queued_packets,
       .max_frame_size = value.max_frame_size,
   };
}

[[nodiscard]] forge::net::quic::endpoint quic_endpoint_for(const forge::net::p2p::endpoint& value) {
   if (!value.is_direct_quic()) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P endpoint is not a direct QUIC endpoint");
   }
   return forge::net::quic::from_transport_endpoint(value.transport);
}

[[nodiscard]] forge::net::p2p::endpoint p2p_endpoint_for(const forge::net::quic::endpoint& value) {
   return forge::net::p2p::endpoint{.transport = forge::net::quic::to_transport_endpoint(value)};
}

[[nodiscard]] std::string listener_key(forge::net::p2p::endpoint value) {
   value.peer.reset();
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.transport.host, error);
   if (!error) {
      value.transport.host = address.to_string();
   }
   return value.to_string();
}

[[nodiscard]] exceptions::code map_quic_error(forge::net::quic::exceptions::code kind) noexcept {
   using quic_kind = forge::net::quic::exceptions::code;
   switch (kind) {
   case quic_kind::invalid_endpoint:
   case quic_kind::invalid_options:
      return exceptions::code::invalid_options;
   case quic_kind::connect_timeout:
   case quic_kind::handshake_timeout:
   case quic_kind::idle_timeout:
      return exceptions::code::timeout;
   case quic_kind::peer_verification_failed:
   case quic_kind::alpn_mismatch:
   case quic_kind::tls_failed:
      return exceptions::code::peer_verification_failed;
   case quic_kind::frame_too_large:
   case quic_kind::malformed_frame:
      return exceptions::code::codec_error;
   case quic_kind::backpressure_rejected:
      return exceptions::code::backpressure_rejected;
   case quic_kind::connection_rejected:
      return exceptions::code::connection_rejected;
   case quic_kind::connection_closed:
   case quic_kind::stream_closed:
   case quic_kind::stream_reset:
      return exceptions::code::closed;
   case quic_kind::canceled:
      return exceptions::code::canceled;
   case quic_kind::dependency_unavailable:
   case quic_kind::internal:
   case quic_kind::unsupported:
      return exceptions::code::internal;
   }
   return exceptions::code::internal;
}

[[noreturn]] void rethrow_quic_as_p2p(const forge::exceptions::base& error) {
   const auto code = forge::net::quic::exceptions::code_of(error);
   if (code) {
      FORGE_THROW_CODE(map_quic_error(*code), error.what());
   }
   throw;
}

[[nodiscard]] peer_id insecure_legacy_peer_id(std::span<const std::uint8_t> der) {
   return peer_id::from_bytes(forge::multiformats::multihash::sha2_256(der).encode());
}

[[nodiscard]] peer_id strict_peer_id_from_certificate_der(std::span<const std::uint8_t> der) {
   try {
      return make_peer_id_from_certificate_der(der);
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "P2P peer certificate is missing a valid signed libp2p identity extension");
   }
}

[[nodiscard]] peer_id verified_peer_id_for(const forge::net::quic::connection& connection,
                                           const std::optional<peer_id>& expected, bool insecure_test_mode) {
   if (insecure_test_mode) {
      if (expected) {
         return *expected;
      }
      if (const auto certificate = connection.peer_certificate()) {
         try {
            return make_peer_id_from_certificate_der(certificate->der);
         } catch (const forge::exceptions::base&) {
            // Insecure test mode still accepts legacy certificates without the libp2p extension.
         }
         return insecure_legacy_peer_id(certificate->der);
      }
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "P2P insecure QUIC test session has no peer certificate");
   }

   const auto certificate = connection.peer_certificate();
   if (!certificate) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P session has no verified peer certificate");
   }
   const auto remote = strict_peer_id_from_certificate_der(certificate->der);
   if (expected && remote != *expected) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P peer id does not match expected peer");
   }
   return remote;
}

[[nodiscard]] std::optional<peer_id> expected_peer_for(const forge::net::p2p::endpoint& endpoint,
                                                       const node::connect_options& options) {
   if (options.expected_peer) {
      return options.expected_peer;
   }
   return endpoint.peer;
}

} // namespace

quic_profile::quic_profile(forge::asio::runtime& runtime_value, const node::options& options_value,
                           resource_manager resources_value,
                           std::shared_ptr<forge::net::p2p::detail::connection_gate> gate)
    : runtime_(runtime_value), options_(options_value), resources_(std::move(resources_value)), gate_(std::move(gate)),
      client_tokens_(std::make_shared<detail::quic_client_token_cache>(options_value.peer_state.max_peers)) {}

[[nodiscard]] bool quic_profile::supports(const forge::net::p2p::endpoint& endpoint) const noexcept {
   return endpoint.is_direct_quic();
}

[[nodiscard]] bool quic_profile::listening() const noexcept {
   auto lock = std::scoped_lock{listeners_mutex_};
   return std::ranges::any_of(listeners_, [](const auto& item) { return item.second.active; });
}

[[nodiscard]] std::vector<forge::net::p2p::endpoint> quic_profile::local_endpoints() const {
   auto out = std::vector<forge::net::p2p::endpoint>{};
   auto lock = std::scoped_lock{listeners_mutex_};
   out.reserve(listeners_.size());
   for (const auto& [_, listener] : listeners_) {
      if (listener.active) {
         out.push_back(listener.local);
      }
   }
   return out;
}

forge::net::p2p::endpoint quic_profile::listen(forge::net::p2p::endpoint endpoint) {
   if (!endpoint.is_direct_quic()) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P endpoint is not a direct QUIC endpoint");
   }
   const auto requested_key = listener_key(endpoint);
   {
      auto lock = std::scoped_lock{listeners_mutex_};
      if (listeners_stopped_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P QUIC direct listener is stopped");
      }
      if (endpoint.transport.port != 0) {
         auto found = listeners_.find(requested_key);
         if (found != listeners_.end() && found->second.active) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P QUIC direct listener endpoint is already active");
         }
      }
   }
   try {
      auto lifecycle = resources_.reserve_lifecycle();
      if (!lifecycle) {
         if (lifecycle.outcome() == resource_manager::transition_result::policy_rejected) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P QUIC listener lifecycle limit reached");
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P QUIC listener lifecycle resource admission failed");
      }
      auto descriptor = lifecycle->reserve_file_descriptors(1);
      if (!descriptor) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P QUIC listener file descriptor limit reached");
      }
      auto native_lifetime = std::make_shared<resource_manager::file_descriptor_reservation>(std::move(*descriptor));
      auto listener =
          std::make_shared<forge::net::quic::listener>(runtime_, quic_endpoint_for(endpoint), server_options());
      auto local = p2p_endpoint_for(listener->local_endpoint());
      const auto key = listener_key(local);
      auto stopped = false;
      auto duplicate = false;
      {
         auto lock = std::scoped_lock{listeners_mutex_};
         stopped = listeners_stopped_;
         const auto found = listeners_.find(key);
         duplicate = found != listeners_.end() && found->second.active;
         if (!stopped && !duplicate) {
            listeners_.emplace(
                key,
                listener_entry{.value = listener, .native_lifetime = native_lifetime, .local = local, .active = true});
         }
      }
      if (stopped || duplicate) {
         try {
            listener->stop();
         } catch (...) {
         }
      }
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P QUIC direct listener is stopped");
      }
      if (duplicate) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P QUIC direct listener endpoint is already active");
      }
      return local;
   } catch (const forge::exceptions::base& error) {
      rethrow_quic_as_p2p(error);
   }
}

void quic_profile::stop() {
   client_tokens_->close();
   auto listeners = stop_listeners();
   for (const auto& listener : listeners) {
      try {
         listener->stop();
      } catch (...) {
      }
   }
   auto active = std::vector<std::shared_ptr<cancellation_latch>>{};
   {
      auto lock = std::scoped_lock{active_mutex_};
      stopped_ = true;
      for (auto iterator = active_.begin(); iterator != active_.end();) {
         if (auto operation = iterator->lock()) {
            active.push_back(std::move(operation));
            ++iterator;
         } else {
            iterator = active_.erase(iterator);
         }
      }
   }
   for (const auto& operation : active) {
      operation->request_stop();
   }
}

boost::asio::awaitable<void> quic_profile::async_stop() {
   stop();
   auto listeners = listener_snapshot();
   for (const auto& listener : listeners) {
      co_await listener->async_stop();
   }
   auto lock = std::scoped_lock{listeners_mutex_};
   listeners_.clear();
}

boost::asio::awaitable<connection>
quic_profile::async_connect(forge::net::p2p::endpoint endpoint, const node::connect_options& options,
                            std::shared_ptr<cancellation_latch> cancellation, std::shared_ptr<void> native_lifetime,
                            authenticated_admission_handler authenticated, tcp_transport_progress_handler,
                            std::shared_ptr<forge::net::quic::connector> connector, bool strict_identity,
                            native_socket_admission_handler socket_admission) {
   // QUIC owns a different handshake path and never emits TCP progress.
   auto selected_listener = std::optional<forge::net::p2p::endpoint>{};
   auto dial_options = options;
   const auto started = std::chrono::steady_clock::now();
   auto cancel_current = std::make_shared<cancellation_latch>();
   auto parent_subscription =
       cancellation_latch::subscribe(cancellation, [cancel_current] noexcept { cancel_current->request_stop(); });
   track(cancel_current);
   if (!connector) {
      const auto local_source = select_dial_source(runtime_.context(), resources_, local_endpoints(), endpoint);
      if (local_source) {
         auto source = listener_entry{};
         auto key = std::string{};
         {
            auto lock = std::scoped_lock{listeners_mutex_};
            for (const auto& [candidate_key, entry] : listeners_) {
               if (entry.active && owns_coordinated_source(entry.local, *local_source)) {
                  source = entry;
                  key = candidate_key;
                  break;
               }
            }
         }
         if (!source.value || !listener_is_current(key, source.value)) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "ordinary QUIC source listener stopped during selection");
         }
         selected_listener = source.local;
         native_lifetime = std::make_shared<native_connection_lifetime>(native_connection_lifetime{
             .source = source.value, .source_admission = source.native_lifetime, .parent = std::move(native_lifetime)});
         connector = std::make_shared<forge::net::quic::connector>(runtime_, *source.value,
                                                                   quic_endpoint_for(*local_source));
      } else {
         if (socket_admission) {
            socket_admission(1);
         }
         connector = std::make_shared<forge::net::quic::connector>(runtime_);
      }
      if (selected_listener && socket_admission) {
         socket_admission(0);
      }
   }
   if (cancel_current->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P QUIC direct connect canceled before start");
   }
   dial_options.timeout -= std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::now() - started);
   if (dial_options.timeout <= std::chrono::milliseconds::zero()) {
      throw_operation_timeout("P2P QUIC direct source selection");
   }
   auto operation_stop = std::make_shared<forge::net::p2p::detail::worker_stop_bridge>();
   auto stop_requested = std::make_shared<std::atomic_bool>(false);
   auto pending = std::make_shared<detail::pending_quic_connection>();
   cancel_current->arm([operation_stop, stop_requested, pending] noexcept {
      stop_requested->store(true, std::memory_order_release);
      operation_stop->request_stop();
      pending->request_cancel();
   });
   auto failure = std::exception_ptr{};
   try {
      const auto expected_peer = expected_peer_for(endpoint, options);
      co_await forge::net::p2p::detail::async_run_with_owner_cancellation(
          operation_stop,
          [this, connector, endpoint, options = dial_options, expected_peer, stop_requested, native_lifetime, pending,
           strict_identity](boost::asio::cancellation_slot) mutable -> boost::asio::awaitable<void> {
             if (stop_requested->load(std::memory_order_acquire)) {
                FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P QUIC direct connect canceled before start");
             }
             auto client_options = detail::make_quic_client_options(
                 endpoint, expected_peer, options.timeout, quic_limits(options_.transport_limits),
                 options_.certificate_pem, options_.private_key_pem,
                 !strict_identity && options_.allow_insecure_test_mode, client_tokens_);
             client_options.connection_lifetime = std::move(native_lifetime);
             pending->install(
                 co_await connector->async_connect(quic_endpoint_for(endpoint), std::move(client_options)));
          });
      auto quic = pending->get();
      if (!quic || stop_requested->load(std::memory_order_acquire)) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P QUIC direct connect canceled");
      }
      const auto remote =
          verified_peer_id_for(*quic, expected_peer, !strict_identity && options_.allow_insecure_test_mode);
      auto local_endpoint = p2p_endpoint_for(quic->local_endpoint());
      auto remote_endpoint = p2p_endpoint_for(quic->remote_endpoint());
      gate_->secured(connection_direction::outbound, remote, local_endpoint, remote_endpoint);
      if (authenticated) {
         authenticated(remote);
      }
      gate_->upgraded(connection_direction::outbound, remote, local_endpoint, remote_endpoint);
      if (!cancel_current->finish()) {
         pending->request_cancel();
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P QUIC direct connect canceled");
      }
      auto promoted = pending->take();
      co_return connection{
          .peer = remote,
          .session = forge::net::quic::as_transport_session(std::move(promoted)),
          .local_endpoint = std::move(local_endpoint),
          .remote_endpoint = std::move(remote_endpoint),
          .native_lifetime = std::move(native_lifetime),
          .authentication = !strict_identity && options_.allow_insecure_test_mode ? peer_authentication::unverified
                                                                                  : peer_authentication::quic_tls,
          .role = upgrade_role::initiator,
      };
   } catch (...) {
      failure = std::current_exception();
   }
   pending->request_cancel();
   static_cast<void>(cancel_current->finish());
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto rejected = pending->take();
   try {
      co_await rejected.async_close();
   } catch (...) {
   }
   if (stop_requested->load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P QUIC direct connect canceled");
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::exceptions::base& error) {
      rethrow_quic_as_p2p(error);
   }
}

boost::asio::awaitable<connection> quic_profile::async_connect_coordinated(
    endpoint remote, peer_id expected_peer, upgrade_role role, std::chrono::milliseconds budget,
    std::shared_ptr<cancellation_latch> cancellation, authenticated_admission_handler authenticated,
    std::optional<endpoint> requested, std::shared_ptr<forge::net::p2p::detail::coordinated_dial> owner) {
   try {
      if (role != upgrade_role::initiator && role != upgrade_role::responder) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid coordinated QUIC upgrade role");
      }
      gate_->peer_dial(expected_peer);
      gate_->address_dial(expected_peer, remote);
      const auto local = select_coordinated_source(local_endpoints(), remote, requested);
      auto source = listener_entry{};
      auto key = std::string{};
      {
         auto lock = std::scoped_lock{listeners_mutex_};
         for (const auto& [candidate_key, entry] : listeners_) {
            if (entry.active && owns_coordinated_source(entry.local, local)) {
               source = entry;
               key = candidate_key;
               break;
            }
         }
      }
      if (!source.value || !listener_is_current(key, source.value)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated QUIC source listener is not active");
      }
      if (owner && (owner->source.get() != source.value.get() || owner->source_key != key)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated QUIC listener generation changed");
      }
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "coordinated QUIC canceled before admission");
      }
      auto dial = resource_manager::dial_reservation{};
      if (!owner) { dial = reserve_coordinated_dial(resources_, expected_peer); }
      if (role == upgrade_role::responder) {
         auto result = co_await async_wait_coordinated_inbound(source, key, local, std::move(remote),
                                                           std::move(expected_peer), budget, std::move(cancellation),
                                                           std::move(authenticated), owner ? owner->pending_quic : nullptr);
         result.coordinated_owner = std::move(owner);
         co_return result;
      }
      auto admission = std::make_shared<resource_manager::session_reservation>(
          reserve_coordinated_session(resources_, resource_manager::session_direction::outbound));
      // The borrowed listener already owns the UDP descriptor: no per-dial FD.
      auto lifetime = std::make_shared<native_connection_lifetime>(native_connection_lifetime{
          .source = source.value,
          .source_admission = source.native_lifetime,
          .admission = admission,
      });
      auto connector = std::make_shared<forge::net::quic::connector>(runtime_, *source.value, quic_endpoint_for(local));
      auto result = co_await async_connect(
          std::move(remote), node::connect_options{.expected_peer = expected_peer, .timeout = budget},
          std::move(cancellation), lifetime,
          [admission, expected_peer, authenticated = std::move(authenticated)](const peer_id& actual) {
             establish_coordinated_session(*admission, expected_peer, actual,
                                           resource_manager::session_direction::outbound, authenticated);
          },
          {}, std::move(connector), true);
      result.admission = std::move(*admission);
      result.native_lifetime = std::move(lifetime);
      result.coordinated_owner = std::move(owner);
      co_return result;
   } catch (const forge::exceptions::base& error) {
      rethrow_quic_as_p2p(error);
   }
}

void quic_profile::prepare_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) {
   const auto local = select_coordinated_source(local_endpoints(), owner->remote, owner->options.local_source);
   auto source = listener_entry{};
   auto source_key = std::string{};
   {
      const auto lock = std::scoped_lock{listeners_mutex_};
      for (const auto& [key, entry] : listeners_) {
         if (entry.active && owns_coordinated_source(entry.local, local)) { source = entry; source_key = key; break; }
      }
   }
   if (!source.value || !listener_is_current(source_key, source.value)) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated QUIC source is not an active listener");
   }
   auto permit = reserve_coordinated_dial(resources_, owner->options.expected_peer);
   auto pending = owner->role == upgrade_role::responder ? std::make_shared<detail::pending_quic_connection>() : nullptr;
   const auto ticket = std::make_tuple(source_key, listener_key(local), listener_key(owner->remote),
                                     owner->options.expected_peer.value);
   auto stored_key = source_key;
   {
      const auto lock = std::scoped_lock{listeners_mutex_};
      const auto current = listeners_.find(source_key);
      if (current == listeners_.end() || !current->second.active || current->second.value != source.value) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated QUIC listener generation changed before lease");
      }
      std::erase_if(coordinated_inbound_, [](const auto& item) { return item.second.expired(); });
      if (pending && coordinated_inbound_.contains(ticket)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated QUIC tuple is already leased");
      }
      if (pending && coordinated_inbound_.size() >= max_coordinated_candidates) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "coordinated QUIC leases are at capacity");
      }
      owner->source = source.value;
      owner->source_admission = source.native_lifetime;
      owner->source_key = std::move(stored_key);
      owner->permit = std::move(permit);
      owner->pending_quic = pending;
      if (pending) { coordinated_inbound_.emplace(ticket, std::move(pending)); }
   }
}

void quic_profile::release_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) noexcept {
   const auto lock = std::scoped_lock{listeners_mutex_};
   if (owner->pending_quic) {
      std::erase_if(coordinated_inbound_, [&owner](const auto& item) { return item.second.lock() == owner->pending_quic; });
   }
}

boost::asio::awaitable<connection> quic_profile::async_accept(forge::net::p2p::endpoint endpoint) {
   try {
      const auto key = listener_key(std::move(endpoint));
      auto listener = std::shared_ptr<forge::net::quic::listener>{};
      {
         auto lock = std::scoped_lock{listeners_mutex_};
         const auto found = listeners_.find(key);
         if (found != listeners_.end() && found->second.active) {
            listener = found->second.value;
         }
      }
      if (!listener) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P QUIC direct listener is not active");
      }
      for (;;) {
         auto quic = co_await listener->async_accept();
         auto pending = std::shared_ptr<detail::pending_quic_connection>{};
         try {
            const auto peer = verified_peer_id_for(quic, std::nullopt, false);
            auto lock = std::scoped_lock{listeners_mutex_};
            const auto found = coordinated_inbound_.find(
                std::make_tuple(key, listener_key(p2p_endpoint_for(quic.local_endpoint())),
                                listener_key(p2p_endpoint_for(quic.remote_endpoint())), peer.value));
            if (found != coordinated_inbound_.end()) {
               pending = found->second.lock();
            }
         } catch (const forge::exceptions::base&) {
            // No strict identity means no coordinated match, even in test mode.
         }
         if (pending && pending->try_install(quic)) {
            continue;
         }
         co_return co_await async_promote_inbound(std::move(quic), key, listener);
      }
   } catch (const forge::exceptions::base& error) {
      rethrow_quic_as_p2p(error);
   }
}

boost::asio::awaitable<connection>
quic_profile::async_promote_inbound(forge::net::quic::connection quic, const std::string& key,
                                    const std::shared_ptr<forge::net::quic::listener>& listener,
                                    std::optional<peer_id> expected, authenticated_admission_handler authenticated,
                                    std::shared_ptr<void> lifetime) {
   auto admission = std::shared_ptr<resource_manager::session_reservation>{};
   auto failure = std::exception_ptr{};
   try {
      if (!listener_is_current(key, listener)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P QUIC direct listener stopped during accept");
      }
      const auto local_endpoint = p2p_endpoint_for(quic.local_endpoint());
      const auto remote_endpoint = p2p_endpoint_for(quic.remote_endpoint());
      // Apply host policy after native acceptance. Rejecting an Initial
      // in the transport filter only causes QUIC packet retransmission.
      gate_->accept(local_endpoint, remote_endpoint);
      auto admission_token = forge::net::quic::detail::connection_access::take_inbound_admission(quic);
      admission = std::static_pointer_cast<resource_manager::session_reservation>(std::move(admission_token));
      if (!admission || !admission->active()) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P QUIC connection is missing inbound admission");
      }
      const auto remote = verified_peer_id_for(quic, expected, !expected && options_.allow_insecure_test_mode);
      const auto transition = admission->establish(resource_manager::session_scope{
          .peer = remote,
          .direction = resource_manager::session_direction::inbound,
      });
      if (transition != resource_manager::transition_result::accepted) {
         if (transition != resource_manager::transition_result::policy_rejected) {
            FORGE_THROW_EXCEPTION(exceptions::internal, "P2P inbound QUIC session resource transition failed");
         }
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P established inbound session limit reached");
      }
      gate_->secured(connection_direction::inbound, remote, local_endpoint, remote_endpoint);
      if (authenticated) {
         authenticated(remote);
      }
      gate_->upgraded(connection_direction::inbound, remote, local_endpoint, remote_endpoint);
      co_return connection{
          .peer = remote,
          .session = forge::net::quic::as_transport_session(std::move(quic)),
          .local_endpoint = std::move(local_endpoint),
          .remote_endpoint = std::move(remote_endpoint),
          .admission = std::move(*admission),
          .native_lifetime = std::move(lifetime),
          .authentication = !expected && options_.allow_insecure_test_mode ? peer_authentication::unverified
                                                                           : peer_authentication::quic_tls,
          .role = upgrade_role::responder,
      };
   } catch (...) {
      failure = std::current_exception();
   }
   quic.request_cancel();
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   try {
      co_await quic.async_close();
   } catch (...) {
   }
   // Inbound admission survives until the lower connection has joined cleanup.
   admission.reset();
   std::rethrow_exception(failure);
}

boost::asio::awaitable<connection>
quic_profile::async_wait_coordinated_inbound(listener_entry source, const std::string& key, endpoint local,
                                             endpoint remote, peer_id expected_peer, std::chrono::milliseconds budget,
                                             std::shared_ptr<cancellation_latch> cancellation,
                                             authenticated_admission_handler authenticated,
                                             std::shared_ptr<detail::pending_quic_connection> prepared) {
   using namespace boost::asio::experimental::awaitable_operators;
   const auto already_prepared = static_cast<bool>(prepared);
   auto pending = prepared ? std::move(prepared) : std::make_shared<detail::pending_quic_connection>();
   if (!already_prepared) {
      auto lock = std::scoped_lock{listeners_mutex_};
      std::erase_if(coordinated_inbound_, [](const auto& entry) { return entry.second.expired(); });
      const auto ticket = std::make_tuple(key, listener_key(local), listener_key(remote), expected_peer.value);
      if (auto found = coordinated_inbound_.find(ticket);
          found != coordinated_inbound_.end() && !found->second.expired()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated QUIC inbound wait is already active");
      }
      if (coordinated_inbound_.size() >= max_coordinated_candidates) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "coordinated QUIC candidate limit reached");
      }
      coordinated_inbound_[ticket] = pending;
   }
   auto cancel_current = std::make_shared<cancellation_latch>();
   auto subscription =
       cancellation_latch::subscribe(cancellation, [cancel_current] noexcept { cancel_current->request_stop(); });
   track(cancel_current);
   auto operation_stop = std::make_shared<forge::net::p2p::detail::worker_stop_bridge>();
   cancel_current->arm([operation_stop, pending] noexcept {
      operation_stop->request_stop();
      pending->request_cancel();
   });
   auto deadline = operation_deadline{runtime_.context(), budget};
   deadline.arm([cancel_current] noexcept { cancel_current->request_stop(); });
   auto lifetime = std::make_shared<native_connection_lifetime>(native_connection_lifetime{
       .source = source.value,
       .source_admission = source.native_lifetime,
   });
   auto failure = std::exception_ptr{};
   auto result = connection{};
   try {
      // The node's ordinary accept loop routes only a strictly authenticated
      // matching peer and candidate tuple here, once. Other candidates probe
      // independently; unrelated inbound sessions keep their normal path.
      co_await forge::net::p2p::detail::async_run_with_owner_cancellation(
          operation_stop,
          [source, local, remote, budget, lifetime,
           pending](boost::asio::cancellation_slot) -> boost::asio::awaitable<void> {
             auto probe_failure = std::exception_ptr{};
             auto probe_and_wait = [&]() -> boost::asio::awaitable<void> {
                try {
                   static_cast<void>(co_await source.value->async_punch(
                       quic_endpoint_for(local), quic_endpoint_for(remote),
                       forge::net::quic::punch_options{
                           .timeout = std::min(budget, std::chrono::milliseconds{30'000}),
                           .max_packets = 64,
                           .lifetime = lifetime,
                       }));
                   co_await pending->async_wait();
                } catch (...) {
                   if (!pending->get()) {
                      probe_failure = std::current_exception();
                      pending->request_cancel();
                   }
                }
             };
             static_cast<void>(co_await (pending->async_wait() || probe_and_wait()));
             if (probe_failure) {
                std::rethrow_exception(probe_failure);
             }
          });
      auto native = pending->take();
      result = co_await async_promote_inbound(std::move(native), key, source.value, expected_peer,
                                              std::move(authenticated), lifetime);
      const auto completed = deadline.finish();
      if (!completed) {
         throw_operation_timeout("P2P coordinated QUIC inbound wait");
      }
      if (!cancel_current->finish()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P coordinated QUIC inbound wait canceled");
      }
      co_return std::move(result);
   } catch (...) {
      failure = std::current_exception();
   }
   const auto timed_out = deadline.timed_out();
   const auto canceled = cancel_current->stop_requested();
   pending->request_cancel();
   static_cast<void>(cancel_current->finish());
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto rejected = pending->take();
   try {
      co_await rejected.async_close();
   } catch (...) {
   }
   co_await async_discard_unpublished(result);
   if (timed_out) {
      throw_operation_timeout("P2P coordinated QUIC inbound wait");
   }
   if (canceled) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P coordinated QUIC inbound wait canceled");
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::exceptions::base& error) {
      rethrow_quic_as_p2p(error);
   }
}

[[nodiscard]] bool
quic_profile::listener_is_current(const std::string& key,
                                  const std::shared_ptr<forge::net::quic::listener>& listener) const {
   auto lock = std::scoped_lock{listeners_mutex_};
   const auto found = listeners_.find(key);
   return !listeners_stopped_ && found != listeners_.end() && found->second.active && found->second.value == listener;
}

[[nodiscard]] std::vector<std::shared_ptr<forge::net::quic::listener>> quic_profile::stop_listeners() {
   auto listeners = std::vector<std::shared_ptr<forge::net::quic::listener>>{};
   auto lock = std::scoped_lock{listeners_mutex_};
   listeners_stopped_ = true;
   listeners.reserve(listeners_.size());
   for (auto& [_, listener] : listeners_) {
      listener.active = false;
      listeners.push_back(listener.value);
   }
   return listeners;
}

[[nodiscard]] std::vector<std::shared_ptr<forge::net::quic::listener>> quic_profile::listener_snapshot() const {
   auto listeners = std::vector<std::shared_ptr<forge::net::quic::listener>>{};
   auto lock = std::scoped_lock{listeners_mutex_};
   listeners.reserve(listeners_.size());
   for (const auto& [_, listener] : listeners_) {
      listeners.push_back(listener.value);
   }
   return listeners;
}

void quic_profile::track(const std::shared_ptr<cancellation_latch>& operation) {
   auto cancel_now = false;
   {
      auto lock = std::scoped_lock{active_mutex_};
      cancel_now = stopped_;
      if (!cancel_now) {
         active_.erase(
             std::remove_if(active_.begin(), active_.end(), [](const auto& operation) { return operation.expired(); }),
             active_.end());
         active_.push_back(operation);
      }
   }
   if (cancel_now) {
      operation->request_stop();
   }
}

[[nodiscard]] forge::net::quic::server_options quic_profile::server_options() const {
   return forge::net::quic::server_options{
       .alpn = "libp2p",
       .limits = quic_limits(options_.transport_limits),
       .security = detail::make_quic_peer_verifier({}, options_.allow_insecure_test_mode),
       .certificate_pem = options_.certificate_pem,
       .private_key_pem = options_.private_key_pem,
       .inbound_admission = [resources = resources_]() mutable -> std::shared_ptr<void> {
          auto admission = resources.reserve_session(resource_manager::session_direction::inbound);
          if (!admission) {
             if (admission.outcome() == resource_manager::transition_result::policy_rejected) {
                return {};
             }
             FORGE_THROW_EXCEPTION(exceptions::internal, "P2P QUIC inbound session resource admission failed");
          }
          return std::make_shared<resource_manager::session_reservation>(std::move(*admission));
       },
   };
}

} // namespace forge::net::p2p::direct
