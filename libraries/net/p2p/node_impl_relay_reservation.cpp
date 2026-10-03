module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/cancellation_latch.hxx"
#include "details/host_addresses.hxx"
#include "details/owner_cancellation.hxx"
#include "details/reachability_manager.hxx"
#include "details/node_impl.hxx"
#include "details/relay_hop_exchange.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

void node::impl::cleanup_expired_relay_reservations_locked() {
   const auto now = std::chrono::steady_clock::now();
   for (auto it = inbound_relay_reservations.begin(); it != inbound_relay_reservations.end();) {
      if (it->second.canceled || it->second.expires_at <= now) {
         if (metrics_value.active_relay_reservations > 0) {
            --metrics_value.active_relay_reservations;
         }
         ++metrics_value.relay_reservation_expirations;
         static_cast<void>(connections.unprotect(it->first, "forge:relay-service"));
         it = inbound_relay_reservations.erase(it);
      } else {
         ++it;
      }
   }
   for (auto it = outbound_relay_reservations.begin(); it != outbound_relay_reservations.end();) {
      if (it->second.canceled || it->second.expires_at <= now) {
         static_cast<void>(connections.unprotect(it->first, "forge:relay-client"));
         it = outbound_relay_reservations.erase(it);
      } else {
         ++it;
      }
   }
}

[[nodiscard]] bool node::impl::has_outbound_relay_reservation(const peer_id& relay_peer) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   return outbound_relay_reservations.contains(relay_peer);
}

[[nodiscard]] bool node::impl::has_fresh_outbound_relay_reservation(const peer_id& relay_peer,
                                                                    std::chrono::milliseconds refresh_margin) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   const auto it = outbound_relay_reservations.find(relay_peer);
   if (it == outbound_relay_reservations.end()) {
      return false;
   }
   return it->second.expires_at > std::chrono::steady_clock::now() + refresh_margin;
}

[[nodiscard]] std::vector<peer_id>
node::impl::fresh_outbound_relay_candidates(std::size_t limit, std::chrono::milliseconds refresh_margin) {
   auto out = std::vector<peer_id>{};
   if (limit == 0) {
      return out;
   }
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   auto scored = std::vector<std::pair<double, peer_id>>{};
   scored.reserve(outbound_relay_reservations.size());
   for (const auto& [relay_peer, reservation] : outbound_relay_reservations) {
      if (reservation.expires_at <= std::chrono::steady_clock::now() + refresh_margin) {
         continue;
      }
      const auto record = store.find(relay_peer);
      scored.push_back({record ? record->score : 0.0, relay_peer});
   }
   std::stable_sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
      if (left.first != right.first) {
         return left.first > right.first;
      }
      return left.second.to_string() < right.second.to_string();
   });
   for (const auto& [_, relay_peer] : scored) {
      if (out.size() >= limit) {
         break;
      }
      out.push_back(relay_peer);
   }
   return out;
}

void node::impl::remember_relay_reservation_in_store(const relay::reservation::info& info) {
   auto relay_endpoints = std::vector<forge::net::p2p::endpoint>{};
   relay_endpoints.reserve(info.relay_endpoints.size());
   for (const auto& endpoint : info.relay_endpoints) {
      relay_endpoints.push_back(endpoint);
   }
   store.upsert_relay_reservation(peer_store::relay_record{
       .relay = info.relay_peer,
       .reservation_id = info.id,
       .expires_at = std::chrono::system_clock::time_point{info.expires_at},
       .endpoints = std::move(relay_endpoints),
       .voucher = info.voucher ? info.voucher->encode() : std::vector<std::uint8_t>{},
   });
}

boost::asio::awaitable<relay::reservation::info>
node::impl::request_relay_reservation(const peer_id& relay_peer, relay::reservation::options reservation_options,
                                     std::chrono::milliseconds timeout,
                                     std::shared_ptr<cancellation_latch> parent, bool automatic,
                                     std::uint64_t generation, std::uint64_t session_id) {
   validate_operation_timeout(timeout, "P2P relay reservation timeout");
   if (!options.relay_policy.client_enabled || private_network_enabled()) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay client policy is disabled");
   }
   if (!valid_peer_id(relay_peer) || relay_peer == local || reservation_options.ttl.count() <= 0 ||
       reservation_options.max_streams == 0 || reservation_options.max_bytes == 0 ||
       reservation_options.max_queued_bytes == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P relay reservation options");
   }
   auto self = shared_from_this();
   auto tracked = lifecycle.track();
   if (!tracked.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P relay lifecycle is closed"); }
   auto operation = std::make_shared<relay_reservation_operation>();
   operation->cancellation = std::make_shared<cancellation_latch>();
   operation->automatic = automatic;
   auto release = [self, relay_peer = peer_id{relay_peer}, operation](void*) noexcept {
      {
         const auto lock = std::scoped_lock{self->mutex};
         const auto found = self->relay_reservation_operations.find(relay_peer);
         if (found != self->relay_reservation_operations.end() && found->second == operation) {
            self->relay_reservation_operations.erase(found);
         }
      }
      self->notify_autorelay_changed();
   };
   static_assert(std::is_nothrow_move_constructible_v<decltype(release)>);
   const auto guard = std::unique_ptr<void, decltype(release)>{this, std::move(release)};
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped || session_admission_closed) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped"); }
      const auto public_host = reachability_manager_value &&
          reachability_manager_value->current().effective == reachability::state::publicly_reachable;
      if (public_host && (automatic || !options.relay_policy.public_relay_allowed)) {
         FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P public host relay reservation policy denied");
      }
      if (automatic && generation != autorelay_generation) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P AutoRelay generation changed");
      }
      const auto found = relay_reservation_operations.find(relay_peer);
      if (found != relay_reservation_operations.end()) {
         if (automatic) { FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay reservation already in flight"); }
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P relay reservation already in flight");
      } else if (relay_reservation_operations.size() >= options.limits.relay.max_reservations) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P relay operation bound reached");
      }
      relay_reservation_operations.emplace(relay_peer, operation);
   }
   const auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto parent_subscription = cancellation_latch::subscribe(parent,
       [cancellation = operation->cancellation]() noexcept { cancellation->request_stop(); });
   auto subscription = cancellation_latch::subscribe(operation->cancellation,
       [stop]() noexcept { stop->request_stop(); });
   auto deadline = operation_deadline{runtime.context(), timeout};
   deadline.arm([cancellation = operation->cancellation]() noexcept { cancellation->request_stop(); });
   auto result = std::optional<relay::reservation::info>{};
   auto failure = std::exception_ptr{};
   try {
      co_await detail::async_run_with_owner_cancellation(stop,
          [self, relay_peer, reservation_options, timeout, operation, automatic, generation, session_id,
           stop, &result](boost::asio::cancellation_slot) -> boost::asio::awaitable<void> {
             const auto started = std::chrono::steady_clock::now();
             auto relay_session = std::shared_ptr<session_state>{};
             if (session_id != 0) {
                const auto lock = std::scoped_lock{self->mutex};
                const auto found = self->sessions.find(session_id);
                if (found != self->sessions.end()) { relay_session = found->second; }
                if (!relay_session || relay_session->closed || relay_session->info.remote_peer != relay_peer) {
                   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P AutoRelay source session was lost");
                }
             } else {
                relay_session = co_await self->ensure_direct_session(relay_peer, timeout,
                    self->options.path_policy.max_direct_endpoints,
                    node::connect_options{}.direct_attempt_timeout, operation->cancellation);
             }
             // Cached claims are hints only. Authenticate and complete Identify
             // on this very direct session before admitting automatic RESERVE.
             if (automatic) { co_await self->identify_session(relay_session); }
             {
                const auto lock = std::scoped_lock{self->mutex};
                if (relay_session->closed || relay_session->info.path != path::kind::direct ||
                    (automatic && (relay_session->authentication == peer_authentication::unverified ||
                     relay_session->info.identify_state != identify::state::identified ||
                     std::ranges::find(relay_session->remote_protocols, builtins::relay_hop) ==
                         relay_session->remote_protocols.end()))) {
                   FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P relay lacks authenticated direct HOP support");
                }
             }
             auto exchange = co_await detail::async_exchange_relay_hop(
                 self->runtime.context(), remaining_timeout(started, timeout, "P2P relay reservation"),
                 "P2P relay reservation",
                 [self, relay_session](detail::stream_admission_handler admitted)
                     -> boost::asio::awaitable<forge::net::p2p::stream> {
                    co_return co_await self->open_session_stream(relay_session, builtins::relay_hop, true,
                                                                std::move(admitted));
                 }, relay::hop_message{.kind = relay::hop_message::message_kind::reserve},
                 4 * 1024, operation->cancellation);
             const auto& response = exchange.response;
             if (response.kind != relay::hop_message::message_kind::status || response.status != relay::status::ok ||
                 !response.reservation_value) {
                FORGE_THROW_CODE(response.kind == relay::hop_message::message_kind::status
                                     ? exceptions::code::relay_rejected : exceptions::code::protocol_error,
                                 "P2P relay reservation rejected");
             }
             const auto& remote = *response.reservation_value;
             const auto system_now = std::chrono::system_clock::now();
             const auto steady_now = std::chrono::steady_clock::now();
             const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(system_now.time_since_epoch());
             const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::time_point::max().time_since_epoch()).count();
             if (remote.expires_at == 0 || remote.expires_at > static_cast<std::uint64_t>(max_seconds)) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P relay expiry is unrepresentable");
             }
             const auto expires_at = std::chrono::seconds{static_cast<std::int64_t>(remote.expires_at)};
             const auto expires = std::chrono::system_clock::time_point{expires_at};
             if (expires <= system_now || expires - system_now >
                 std::chrono::steady_clock::time_point::max() - steady_now) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P relay expiry is expired or unrepresentable");
             }
             const auto ttl = std::chrono::duration_cast<std::chrono::milliseconds>(expires - system_now);
             if (ttl.count() <= 0 || remote.relay_endpoints.empty() ||
                 remote.relay_endpoints.size() > self->options.identify.max_listen_endpoints) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P relay reservation has no usable bounded lifetime/addresses");
             }
             auto endpoints = std::map<std::string, endpoint>{};
             for (auto address : remote.relay_endpoints) {
                if ((!address.is_direct_tcp() && !address.is_direct_quic()) || address.relayed ||
                    (address.peer && *address.peer != relay_peer) || address.transport.port == 0 ||
                    host_addresses::has_interface_zone(address) ||
                    host_addresses::classify_endpoint_scope(address) == host_addresses::endpoint_scope::unroutable) {
                   FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P relay endpoint has wrong peer or invalid direct transport");
                }
                address.peer = relay_peer;
                const auto canonical = parse_endpoint(address.to_multiaddr().to_string());
                endpoints.emplace(canonical.to_string(), canonical);
             }
             if (remote.voucher) {
                const auto voucher = relay::codec::open_reservation_voucher(*remote.voucher, relay_peer,
                    static_cast<std::uint64_t>(now_seconds.count()));
                if (voucher.peer != self->local || voucher.expires_at != remote.expires_at) {
                   FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P relay voucher peer/expiry mismatch");
                }
             }
             auto info = relay::reservation::info{
                 .relay_peer = relay_peer,
                 .id = remote.expires_at,
                 .expires_at = expires_at,
                 .ttl = ttl,
                 .max_streams = std::min(reservation_options.max_streams, self->options.limits.relay.max_streams_per_reservation),
                 .max_bytes = std::min(reservation_options.max_bytes, self->options.limits.relay.max_relay_bytes),
                 .max_queued_bytes = std::min(reservation_options.max_queued_bytes, self->options.limits.relay.max_queued_bytes),
                 .voucher = remote.voucher,
                 .remote_limit = response.limit_value,
             };
             if (info.remote_limit && info.remote_limit->data != 0) {
                info.max_bytes = std::min(info.max_bytes, info.remote_limit->data);
             }
             for (const auto& [_, address] : endpoints) { info.relay_endpoints.push_back(address); }
             // Close/reset only this operation's stream, never the shared session.
             exchange.stream.request_cancel();
             {
                const auto lock = std::scoped_lock{self->mutex};
                const auto active = self->relay_reservation_operations.find(relay_peer);
                const auto session = self->sessions.find(relay_session->id);
                const auto public_host = self->reachability_manager_value &&
                    self->reachability_manager_value->current().effective == reachability::state::publicly_reachable;
                if (self->stopped || self->session_admission_closed || operation->canceled || stop->stop_requested() ||
                    operation->cancellation->stop_requested() || active == self->relay_reservation_operations.end() ||
                    active->second != operation || session == self->sessions.end() || session->second != relay_session ||
                    relay_session->closed || (automatic && (generation != self->autorelay_generation || public_host)) ||
                    (!automatic && public_host && !self->options.relay_policy.public_relay_allowed)) {
                   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay completion lost its owner");
                }
                self->cleanup_expired_relay_reservations_locked();
                const auto existing = self->outbound_relay_reservations.find(relay_peer);
                if (existing == self->outbound_relay_reservations.end() &&
                    (self->outbound_relay_reservations.size() >= self->options.limits.relay.max_reservations ||
                     (automatic && self->outbound_relay_reservations.size() >= self->options.relay_policy.target_reservations))) {
                   FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P relay reservation bound reached");
                }
                // A manual owner is not silently adopted by an automatic renewal.
                if (automatic && existing != self->outbound_relay_reservations.end() && !existing->second.automatic) {
                   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay reservation has a manual owner");
                }
                auto state = relay_reservation_state{
                    .owner = self->local, .relay_peer = relay_peer, .id = info.id,
                    .expires_at = steady_now + ttl, .max_streams = info.max_streams,
                    .max_bytes = info.max_bytes, .max_queued_bytes = info.max_queued_bytes,
                    .info = info, .session_id = relay_session->id, .automatic = automatic, .operation = operation};
                const auto already_protected = self->outbound_relay_reservations.contains(relay_peer);
                self->connections.protect(relay_peer, "forge:relay-client");
                try {
                   self->remember_relay_reservation_in_store(info);
                   self->outbound_relay_reservations.insert_or_assign(relay_peer, std::move(state));
                } catch (...) {
                   if (!already_protected) { static_cast<void>(self->connections.unprotect(relay_peer, "forge:relay-client")); }
                   throw;
                }
                result = std::move(info);
             }
          }, {.lifecycle_stop = tracked.stop_source()});
   } catch (...) { failure = std::current_exception(); }
   const auto completed = deadline.finish();
   if (!completed || deadline.timed_out() || failure || operation->cancellation->stop_requested()) {
      {
         const auto lock = std::scoped_lock{mutex};
         const auto active = outbound_relay_reservations.find(relay_peer);
         if (active != outbound_relay_reservations.end() && active->second.operation.lock() == operation) {
            outbound_relay_reservations.erase(active);
            static_cast<void>(connections.unprotect(relay_peer, "forge:relay-client"));
         }
      }
      refresh_relay_publication();
   }
   if (!completed || deadline.timed_out()) { throw_operation_timeout("P2P relay reservation"); }
   if (failure) {
      try { std::rethrow_exception(failure); }
      catch (const forge::exceptions::base& error) { rethrow_transport_as_p2p(error); }
   }
   if (!result || operation->cancellation->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay reservation canceled");
   }
   refresh_relay_publication();
   start_autorelay();
   notify_autorelay_changed();
   co_return std::move(*result);
}

boost::asio::awaitable<void> node::impl::ensure_relay_reservation(const peer_id& relay_peer,
                                                                  std::chrono::milliseconds timeout) {
   if (has_outbound_relay_reservation(relay_peer)) {
      co_return;
   }
   (void)co_await request_relay_reservation(relay_peer,
                                            relay::reservation::options{
                                                .ttl = options.limits.relay.reservation_ttl,
                                                .max_streams = options.limits.relay.max_streams_per_reservation,
                                                .max_bytes = options.limits.relay.max_relay_bytes,
                                                .max_queued_bytes = options.limits.relay.max_queued_bytes,
                                            },
                                            timeout);
}


} // namespace forge::net::p2p
