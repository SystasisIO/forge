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
import forge.multiformats.multiaddr;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
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
import forge.net.p2p.topology;
import forge.net.tcp.connection;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.exceptions;
import forge.net.yamux.session;

#include "details/libp2p_identity_material.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"
#include "details/resource_stream.hxx"
#include "details/relay_hop_exchange.hxx"
#include "details/relay_discovery.hxx"
#include "details/relay_pair.hxx"
#include "details/relay_transport.hxx"
#include "details/stream_upgrade.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

namespace {

[[nodiscard]] endpoint relay_circuit_endpoint(endpoint relay_endpoint, const peer_id& relay_peer,
                                              const peer_id& circuit_peer) {
   relay_endpoint.peer = relay_peer;
   relay_endpoint.relayed = endpoint::circuit{.target = circuit_peer};
   return relay_endpoint;
}

[[nodiscard]] const endpoint& relay_transport_endpoint(const auto& session) {
   if (session.direct_endpoint) {
      return *session.direct_endpoint;
   }
   if (session.remote_endpoint) {
      return *session.remote_endpoint;
   }
   FORGE_THROW_EXCEPTION(exceptions::internal, "P2P relay session has no canonical direct endpoint");
}

} // namespace

void node::impl::cleanup_expired_relay_reservations_locked() {
   const auto now = std::chrono::steady_clock::now();
   for (auto it = inbound_relay_reservations.begin(); it != inbound_relay_reservations.end();) {
      if (it->second.canceled || it->second.expires_at <= now) {
         if (metrics_value.active_relay_reservations > 0) {
            --metrics_value.active_relay_reservations;
         }
         ++metrics_value.relay_reservation_expirations;
         it = inbound_relay_reservations.erase(it);
      } else {
         ++it;
      }
   }
   for (auto it = outbound_relay_reservations.begin(); it != outbound_relay_reservations.end();) {
      if (it->second.canceled || it->second.expires_at <= now) {
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

bool node::impl::remember_outbound_relay_reservation(node::impl::relay_reservation_state reservation) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   outbound_relay_reservations[reservation.relay_peer] = std::move(reservation);
   return true;
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

bool node::impl::remember_inbound_relay_reservation(const peer_id& owner, relay::reservation::options request) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   if (inbound_relay_reservations.size() >= options.limits.relay.max_reservations &&
       !inbound_relay_reservations.contains(owner)) {
      ++metrics_value.relay_reservation_rejections;
      return false;
   }
   auto resource = resource_manager::relay_reservation{};
   auto reservation_id = std::uint64_t{};
   auto active_streams = std::size_t{};
   if (const auto existing = inbound_relay_reservations.find(owner); existing != inbound_relay_reservations.end()) {
      resource = std::move(existing->second.resource);
      reservation_id = existing->second.id;
      active_streams = existing->second.active_streams;
   } else {
      auto acquired = resources.reserve_relay(resource_manager::scope{.peer = owner, .protocol = builtins::relay_hop});
      if (!acquired) {
         if (acquired.outcome() == resource_manager::transition_result::policy_rejected) {
            ++metrics_value.relay_reservation_rejections;
            return false;
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P relay reservation resource admission failed");
      }
      resource = std::move(*acquired);
      reservation_id = next_reservation_id++;
   }
   const auto ttl = std::min(request.ttl, options.limits.relay.reservation_ttl);
   auto reservation = relay_reservation_state{
       .owner = owner,
       .relay_peer = local,
       .id = reservation_id,
       .expires_at = std::chrono::steady_clock::now() + ttl,
       .max_streams = std::min(request.max_streams, options.limits.relay.max_streams_per_reservation),
       .max_bytes = std::min(request.max_bytes, options.limits.relay.max_relay_bytes),
       .max_queued_bytes = std::min(request.max_queued_bytes, options.limits.relay.max_queued_bytes),
       .active_streams = active_streams,
       .resource = std::move(resource),
   };
   inbound_relay_reservations[owner] = std::move(reservation);
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
   ++metrics_value.relay_reservations;
   return true;
}

bool node::impl::cancel_inbound_relay_reservation(const peer_id& owner, std::uint64_t reservation_id) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   const auto it = inbound_relay_reservations.find(owner);
   if (it == inbound_relay_reservations.end() || (reservation_id != 0 && it->second.id != reservation_id)) {
      return false;
   }
   inbound_relay_reservations.erase(it);
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
   return true;
}

std::optional<node::impl::relay_admission> node::impl::begin_relay(const peer_id& owner, relay::status& status) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   if (metrics_value.active_relays >= options.limits.relay.max_active_relays) {
      ++metrics_value.relay_rejections;
      status = relay::status::resource_limit_exceeded;
      return std::nullopt;
   }
   auto circuit = resources.reserve_relay(owner);
   if (!circuit) {
      if (circuit.outcome() == resource_manager::transition_result::policy_rejected) {
         ++metrics_value.relay_rejections;
         status = relay::status::resource_limit_exceeded;
         return std::nullopt;
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P relay circuit resource admission failed");
   }
   auto reservation_id = std::optional<std::uint64_t>{};
   if (options.limits.relay.require_reservation) {
      const auto reservation = inbound_relay_reservations.find(owner);
      if (reservation == inbound_relay_reservations.end()) {
         ++metrics_value.relay_rejections;
         status = relay::status::no_reservation;
         return std::nullopt;
      }
      if (reservation->second.active_streams >= reservation->second.max_streams) {
         ++metrics_value.relay_rejections;
         status = relay::status::resource_limit_exceeded;
         return std::nullopt;
      }
      ++reservation->second.active_streams;
      reservation_id = reservation->second.id;
   }
   ++metrics_value.active_relays;
   ++metrics_value.relays_opened;
   status = relay::status::ok;
   return relay_admission{.circuit = std::move(*circuit), .reservation_id = reservation_id};
}

[[nodiscard]] std::uint64_t node::impl::relay_byte_limit(const peer_id& owner) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   const auto reservation = inbound_relay_reservations.find(owner);
   if (reservation != inbound_relay_reservations.end()) {
      return reservation->second.max_bytes;
   }
   return options.limits.relay.max_relay_bytes;
}

void node::impl::finish_relay(const peer_id& owner, std::optional<std::uint64_t> reservation_id) {
   auto lock = std::scoped_lock{mutex};
   auto reservation = inbound_relay_reservations.find(owner);
   if (reservation_id && reservation != inbound_relay_reservations.end() && reservation->second.id == *reservation_id &&
       reservation->second.active_streams > 0) {
      --reservation->second.active_streams;
   }
   if (metrics_value.active_relays > 0) {
      --metrics_value.active_relays;
   }
}

void node::impl::erase_inbound_relay_reservation_locked(const peer_id& owner) noexcept {
   inbound_relay_reservations.erase(owner);
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
}

void node::impl::record_relay_bytes(std::uint64_t bytes) noexcept {
   auto lock = std::scoped_lock{mutex};
   const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
   metrics_value.relay_bytes =
       bytes > maximum - metrics_value.relay_bytes ? maximum : metrics_value.relay_bytes + bytes;
}

void node::impl::record_path_open(path::kind kind) {
   auto lock = std::scoped_lock{mutex};
   if (kind == path::kind::direct) {
      ++metrics_value.path_direct_opens;
   } else {
      ++metrics_value.path_relay_opens;
   }
}

void node::impl::record_path_attempt(path::kind kind) {
   auto lock = std::scoped_lock{mutex};
   if (kind == path::kind::direct) {
      ++metrics_value.path_direct_attempts;
   } else {
      ++metrics_value.path_relay_attempts;
   }
}

void node::impl::record_hole_punch_result(hole_punch::status status) {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.hole_punch_attempts;
   if (status == hole_punch::status::succeeded) {
      ++metrics_value.hole_punch_successes;
   } else if (status == hole_punch::status::failed) {
      ++metrics_value.hole_punch_failures;
   }
}

void node::impl::record_direct_failure(const peer_id& peer) {
   store.mark_failure(peer);
   increment_direct_failure();
}

void node::impl::increment_direct_failure() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.direct_failures;
}

std::chrono::system_clock::time_point node::impl::endpoint_backoff_until(const peer_id& peer,
                                                                         const forge::net::p2p::endpoint& endpoint,
                                                                         path::kind kind) const {
   return endpoint_backoff_until(peer, endpoint.to_multiaddr(), kind);
}

std::chrono::system_clock::time_point node::impl::endpoint_backoff_until(
    const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind) const {
   auto failures = std::uint64_t{1};
   if (auto record = store.find(peer)) {
      const auto endpoint_string = address.to_string();
      for (const auto& current : record->endpoints) {
         if (current.kind == kind && current.address.to_string() == endpoint_string) {
            failures = current.failures + 1;
            break;
         }
      }
   }
   const auto base = options.limits.dial_backoff_base;
   const auto step = options.limits.dial_backoff_step;
   const auto cap = options.limits.dial_backoff_max;
   const auto cap_count = cap.count() > base.count() ? cap.count() - base.count() : 0;
   const auto step_count = step.count();
   const auto max_square =
       cap_count > 0 && step_count > 0 ? static_cast<std::uint64_t>(cap_count / step_count) : std::uint64_t{0};
   const auto square = failures > std::numeric_limits<std::uint64_t>::max() / failures
                           ? std::numeric_limits<std::uint64_t>::max()
                           : failures * failures;
   const auto extra = std::chrono::milliseconds{
       static_cast<std::chrono::milliseconds::rep>(std::min(square, max_square) * step_count)};
   return std::chrono::system_clock::now() + std::min(base + extra, cap);
}

void node::impl::record_relay_failure() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.relay_failures;
}

boost::asio::awaitable<relay::reservation::info>
node::impl::request_relay_reservation(const peer_id& relay_peer, relay::reservation::options reservation_options,
                                      std::chrono::milliseconds timeout) {
   validate_operation_timeout(timeout, "P2P relay reservation timeout");
   if (!options.relay_policy.client_enabled) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay client policy is disabled");
   }
   if (reservation_options.ttl.count() <= 0 || reservation_options.max_streams == 0 ||
       reservation_options.max_bytes == 0 || reservation_options.max_queued_bytes == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P relay reservation options");
   }
   const auto started = std::chrono::steady_clock::now();
   auto relay_session = co_await ensure_direct_session(relay_peer, timeout);
   try {
      auto exchange = co_await detail::async_exchange_relay_hop(
          runtime.context(), remaining_timeout(started, timeout, "P2P relay reservation"), "P2P relay reservation",
          [this, relay_session](
              detail::stream_admission_handler admitted) -> boost::asio::awaitable<forge::net::p2p::stream> {
             co_return co_await open_session_stream(relay_session, builtins::relay_hop, true, std::move(admitted));
          },
          relay::hop_message{.kind = relay::hop_message::message_kind::reserve},
          reachability::options{}.max_message_size);
      auto response = std::move(exchange.response);
      if (response.kind != relay::hop_message::message_kind::status || response.status != relay::status::ok ||
          !response.reservation_value) {
         FORGE_THROW_CODE(response.kind == relay::hop_message::message_kind::status ? exceptions::code::relay_rejected
                                                                                    : exceptions::code::protocol_error,
                          "P2P relay reservation rejected");
      }
      const auto now_seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
      const auto expires_at = std::chrono::seconds{static_cast<std::int64_t>(response.reservation_value->expires_at)};
      const auto ttl = expires_at > now_seconds ? expires_at - now_seconds : std::chrono::seconds{1};
      const auto limit = response.limit_value.value_or(relay::limit{
          .duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration),
          .data = options.limits.relay.max_relay_bytes,
      });
      auto info = relay::reservation::info{
          .relay_peer = relay_peer,
          .id = response.reservation_value->expires_at,
          .expires_at = expires_at,
          .ttl = std::chrono::duration_cast<std::chrono::milliseconds>(ttl),
          .max_streams = reservation_options.max_streams,
          .max_bytes = limit.data == 0 ? reservation_options.max_bytes : limit.data,
          .max_queued_bytes = reservation_options.max_queued_bytes,
          .relay_endpoints = response.reservation_value->relay_endpoints,
          .voucher = response.reservation_value->voucher,
      };
      // libp2p Circuit Relay v2 vouchers are signed envelopes. Keep the
      // envelope bytes intact here; validation belongs to the signed-envelope
      // layer, not to the older FORGE-local voucher shape.
      remember_outbound_relay_reservation(relay_reservation_state{
          .owner = local,
          .relay_peer = relay_peer,
          .id = info.id,
          .expires_at = std::chrono::steady_clock::now() + info.ttl,
          .max_streams = info.max_streams,
          .max_bytes = info.max_bytes,
          .max_queued_bytes = info.max_queued_bytes,
      });
      remember_relay_reservation_in_store(info);
      co_return info;
   } catch (const forge::exceptions::base& error) {
      rethrow_transport_as_p2p(error);
   }
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

boost::asio::awaitable<std::vector<relay::reservation::info>>
node::impl::refresh_relay_candidates(std::optional<peer_id> target, std::chrono::milliseconds timeout) {
   validate_operation_timeout(timeout, "P2P AutoRelay refresh timeout");
   if (!options.relay_policy.client_enabled) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay client policy is disabled");
   }
   if (!options.relay_policy.auto_discovery_enabled) {
      co_return std::vector<relay::reservation::info>{};
   }

   {
      auto lock = std::scoped_lock{mutex};
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
      }
      ++metrics_value.relay_discovery_refreshes;
   }

   const auto system_now = std::chrono::system_clock::now();
   relay_discovery::prune_expired_reservations(store, system_now, options.peer_state.max_peers);

   const auto target_reservations = options.relay_policy.target_reservations;
   auto fresh_count = fresh_outbound_relay_candidates(target_reservations, options.relay_policy.refresh_margin).size();
   if (fresh_count >= target_reservations) {
      co_return std::vector<relay::reservation::info>{};
   }

   const auto snapshot = store.candidates(capabilities::relay | capabilities::relay_reservation,
                                          options.relay_policy.max_candidates_per_refresh);
   auto candidates =
       relay_discovery::select_candidates(snapshot, relay_discovery::request{
                                                        .local = local,
                                                        .target = target.value_or(peer_id{}),
                                                        .now = system_now,
                                                        .limit = options.relay_policy.max_candidates_per_refresh,
                                                    });
   auto out = std::vector<relay::reservation::info>{};
   out.reserve(std::min(candidates.size(), target_reservations - fresh_count));

   const auto started = std::chrono::steady_clock::now();
   auto attempts_in_batch = std::size_t{0};
   for (const auto& candidate : candidates) {
      if (fresh_count + out.size() >= target_reservations) {
         break;
      }
      if (attempts_in_batch >= options.relay_policy.max_parallel_reservations) {
         co_await asio::post(runtime.context(), asio::use_awaitable);
         attempts_in_batch = 0;
      }
      if (has_fresh_outbound_relay_reservation(candidate.peer, options.relay_policy.refresh_margin)) {
         ++fresh_count;
         continue;
      }
      ++attempts_in_batch;
      {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.relay_discovery_attempts;
      }
      try {
         const auto remaining = remaining_timeout(started, timeout, "P2P AutoRelay refresh");
         auto info =
             co_await request_relay_reservation(candidate.peer,
                                                relay::reservation::options{
                                                    .ttl = options.limits.relay.reservation_ttl,
                                                    .max_streams = options.limits.relay.max_streams_per_reservation,
                                                    .max_bytes = options.limits.relay.max_relay_bytes,
                                                    .max_queued_bytes = options.limits.relay.max_queued_bytes,
                                                },
                                                remaining);
         store.mark_success(candidate.peer, path::kind::relay, std::chrono::milliseconds{0});
         {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.relay_discovery_successes;
         }
         out.push_back(std::move(info));
      } catch (const forge::exceptions::base&) {
         relay_discovery::backoff_candidate(store, candidate.peer,
                                            std::chrono::system_clock::now() + options.relay_policy.candidate_backoff);
         {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.relay_discovery_failures;
         }
      }
   }
   co_return out;
}

void node::impl::launch_relay_discovery_maintenance() {
   if (!options.relay_policy.client_enabled || !options.relay_policy.auto_discovery_enabled) {
      return;
   }
   {
      auto lock = std::scoped_lock{mutex};
      if (relay_discovery_value.maintenance_started) {
         return;
      }
      relay_discovery_value.maintenance_started = true;
   }
   auto self = shared_from_this();
   if (!launch_tracked([self]() -> asio::awaitable<void> {
          const auto wakeup = self->lifecycle_wakeup;
          auto observed = wakeup->epoch();
          while (true) {
             const auto deadline = std::chrono::steady_clock::now() + self->options.limits.topology.refresh_interval;
             // Keep this period fixed across unrelated admission notifications.
             while (!self->lifecycle.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                observed = co_await wakeup->async_wait_until(observed, deadline);
             }
             if (self->lifecycle.stop_requested()) {
                co_return;
             }
             {
                auto lock = std::scoped_lock{self->mutex};
                if (self->stopped) {
                   co_return;
                }
             }
             try {
                (void)co_await self->refresh_relay_candidates(std::nullopt,
                                                              self->options.limits.topology.query_timeout);
             } catch (const forge::exceptions::base&) {
                auto lock = std::scoped_lock{self->mutex};
                ++self->metrics_value.relay_discovery_failures;
             }
          }
       })) {
      auto lock = std::scoped_lock{mutex};
      relay_discovery_value.maintenance_started = false;
   }
}

boost::asio::awaitable<upgraded_session>
node::impl::open_relay_yamux(const peer_id& peer, const peer_id& relay_peer, std::chrono::milliseconds timeout,
                             std::function<void(const peer_id&)> authenticated_admission) {
   const auto started = std::chrono::steady_clock::now();
   record_path_attempt(path::kind::relay);
   auto relay_session = co_await ensure_direct_session(relay_peer, timeout);
   try {
      const auto& relay_endpoint = relay_transport_endpoint(*relay_session);
      const auto local_endpoint = relay_circuit_endpoint(relay_endpoint, relay_peer, local);
      const auto remote_endpoint = relay_circuit_endpoint(relay_endpoint, relay_peer, peer);
      connection_gate->address_dial(peer, remote_endpoint);
      auto exchange = co_await detail::async_exchange_relay_hop(
          runtime.context(), remaining_timeout(started, timeout, "P2P relay protocol open"), "P2P relay protocol open",
          [this, relay_session](
              detail::stream_admission_handler admitted) -> boost::asio::awaitable<forge::net::p2p::stream> {
             co_return co_await open_session_stream(relay_session, builtins::relay_hop, true, std::move(admitted));
          },
          relay::hop_message{
              .kind = relay::hop_message::message_kind::connect,
              .target = relay::peer{.id = peer},
          },
          reachability::options{}.max_message_size);
      auto response = std::move(exchange.response);
      if (response.kind != relay::hop_message::message_kind::status || response.status != relay::status::ok) {
         FORGE_THROW_CODE(response.kind == relay::hop_message::message_kind::status ? exceptions::code::relay_rejected
                                                                                    : exceptions::code::protocol_error,
                          response.kind == relay::hop_message::message_kind::status
                              ? "P2P relay open rejected with status " +
                                    std::to_string(static_cast<std::uint16_t>(response.status))
                              : "P2P relay open rejected with unexpected response");
      }
      record_path_open(path::kind::relay);
      auto stream = detail::stream_access::with_buffer(std::move(exchange.stream), std::move(exchange.buffered));
      co_return co_await upgrade_relay_outbound_session(
          std::move(stream), options, identity, peer,
          upgrade_callbacks{
              .secured =
                  [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                     gate->secured(connection_direction::outbound, authenticated_peer, local_endpoint, remote_endpoint);
                  },
              .established = std::move(authenticated_admission),
              .upgraded =
                  [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                     gate->upgraded(connection_direction::outbound, authenticated_peer, local_endpoint,
                                    remote_endpoint);
                  },
          });
   } catch (const forge::exceptions::base& error) {
      if (p2p_code(error) != exceptions::code::connection_rejected) {
         record_relay_failure();
      }
      rethrow_transport_as_p2p(error);
   }
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::ensure_relay_session(const peer_id& peer, const peer_id& relay_peer, std::chrono::milliseconds timeout) {
   if (auto existing = session_for_path(peer, path::kind::relay, relay_peer)) {
      co_return existing;
   }
   connection_gate->peer_dial(peer);
   auto reservation = resources.reserve_session(resource_manager::session_direction::outbound);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.backpressure_rejections;
         ++metrics_value.connection_rejections;
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P pending outbound relay session limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P outbound relay session resource admission failed");
   }
   auto upgraded = co_await open_relay_yamux(
       peer, relay_peer, timeout, [this, &reservation](const peer_id& authenticated_peer) {
          const auto transition = reservation->establish(resource_manager::session_scope{
              .peer = authenticated_peer,
              .direction = resource_manager::session_direction::outbound,
          });
          if (transition != resource_manager::transition_result::accepted) {
             if (transition != resource_manager::transition_result::policy_rejected) {
                FORGE_THROW_EXCEPTION(exceptions::internal, "P2P outbound relay session resource transition failed");
             }
             {
                auto lock = std::scoped_lock{mutex};
                ++metrics_value.backpressure_rejections;
                ++metrics_value.connection_rejections;
             }
             FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                   "P2P established outbound relay session limit reached");
          }
       });
   auto session = std::make_shared<session_state>();
   session->info = node::session_info{
       .remote_peer = std::move(upgraded.peer),
       .path = path::kind::relay,
       .relay_peer = relay_peer,
   };
   session->authentication = upgraded.authentication;
   session->connection = std::move(*upgraded.session).as_transport();
   session->resource = std::move(*reservation);
   co_await remember_session(session, connection_manager::direction::outbound);
   launch_session_accept_loop(session);
   launch_identify(session);
   co_return session;
}

boost::asio::awaitable<forge::net::p2p::stream> node::impl::open_protocol_via_relay(const peer_id& peer,
                                                                                    const protocol_id& protocol,
                                                                                    const peer_id& relay_peer,
                                                                                    std::chrono::milliseconds timeout) {
   auto session = co_await ensure_relay_session(peer, relay_peer, timeout);
   co_return co_await open_session_stream(session, protocol);
}

boost::asio::awaitable<void> node::impl::handle_relayed_yamux_stream(std::shared_ptr<node::impl::session_state> session,
                                                                     forge::net::transport::stream stream,
                                                                     resource_manager::stream_reservation reservation) {
   auto admitted =
       co_await accept_resource_stream(session->info.remote_peer, std::move(stream), std::move(reservation));
   detail::stream_access::set_authentication(admitted.stream, session->authentication);
   if (admitted.protocol == builtins::ping) {
      co_await handle_ping(std::move(admitted.stream));
   } else if (admitted.protocol == builtins::identify) {
      co_await handle_identify(session, std::move(admitted.stream));
   } else if (admitted.protocol == builtins::identify_push) {
      co_await handle_identify_push(session, std::move(admitted.stream), std::move(admitted.resource));
   } else if (admitted.protocol == builtins::dcutr) {
      co_await handle_dcutr(session, std::move(admitted.stream));
   } else if (dht_profiles.contains(admitted.protocol)) {
      co_await handle_dht(session, admitted.protocol, std::move(admitted.stream));
   } else if (admitted.protocol == builtins::rendezvous) {
      co_await handle_rendezvous(session, std::move(admitted.stream));
   } else if (admitted.protocol == builtins::meshsub_v11 || admitted.protocol == builtins::meshsub_v10) {
      co_await handle_pubsub(session, std::move(admitted.stream));
   } else {
      auto handler = handler_for(admitted.protocol);
      if (!handler) {
         increment_protocol_rejected();
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "unsupported negotiated relayed P2P protocol");
      }
      increment_protocol_accepted();
      co_await (*handler)(node::incoming_protocol_stream{
          .session = session_info_for(session),
          .protocol = admitted.protocol,
          .stream = std::move(admitted.stream),
      });
   }
   co_await detail::async_close_unescaped(admitted.resource);
}

boost::asio::awaitable<void> node::impl::handle_relay_stop(std::shared_ptr<node::impl::session_state> session,
                                                           forge::net::p2p::stream stream) {
   auto relay_buffer = std::vector<std::uint8_t>{};
   auto request = relay::codec::decode_stop(
       co_await async_read_length_delimited(stream, relay_buffer, reachability::options{}.max_message_size));
   if (request.kind != relay::stop_message::message_kind::connect || !request.source) {
      co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
          .kind = relay::stop_message::message_kind::status,
          .status = relay::status::malformed_message,
      }));
      co_return;
   }
   const auto& relay_endpoint = relay_transport_endpoint(*session);
   const auto local_endpoint = relay_circuit_endpoint(relay_endpoint, session->info.remote_peer, local);
   const auto remote_endpoint = relay_circuit_endpoint(relay_endpoint, session->info.remote_peer, request.source->id);
   connection_gate->accept(local_endpoint, remote_endpoint);
   auto reservation = resources.reserve_session(resource_manager::session_direction::inbound);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.backpressure_rejections;
            ++metrics_value.connection_rejections;
         }
         co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
             .kind = relay::stop_message::message_kind::status,
             .status = relay::status::resource_limit_exceeded,
         }));
         co_return;
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P inbound relay session resource admission failed");
   }
   co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
       .kind = relay::stop_message::message_kind::status,
       .limit_value = request.limit_value,
       .status = relay::status::ok,
   }));
   stream = detail::stream_access::with_buffer(std::move(stream), std::move(relay_buffer));
   auto upgraded = co_await upgrade_relay_inbound_session(
       std::move(stream), options, identity, request.source->id,
       upgrade_callbacks{
           .secured =
               [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                  gate->secured(connection_direction::inbound, authenticated_peer, local_endpoint, remote_endpoint);
           },
           .established =
               [&reservation](const peer_id& authenticated_peer) {
                  const auto transition = reservation->establish(resource_manager::session_scope{
                      .peer = authenticated_peer,
                      .direction = resource_manager::session_direction::inbound,
                  });
                  if (transition != resource_manager::transition_result::accepted) {
                     if (transition != resource_manager::transition_result::policy_rejected) {
                        FORGE_THROW_EXCEPTION(exceptions::internal,
                                              "P2P inbound relay session resource transition failed");
                     }
                     FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                           "P2P established inbound relay session limit reached");
                  }
               },
           .upgraded =
               [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                  gate->upgraded(connection_direction::inbound, authenticated_peer, local_endpoint, remote_endpoint);
               },
       });
   auto relayed_session = std::make_shared<session_state>();
   relayed_session->info = node::session_info{
       .remote_peer = std::move(upgraded.peer),
       .path = path::kind::relay,
       .relay_peer = session->info.remote_peer,
   };
   relayed_session->authentication = upgraded.authentication;
   relayed_session->connection = std::move(*upgraded.session).as_transport();
   relayed_session->resource = std::move(*reservation);
   co_await remember_session(relayed_session, connection_manager::direction::inbound);
   launch_session_accept_loop(relayed_session);
   launch_identify(relayed_session);
   if (options.capabilities.has(capabilities::hole_punching)) {
      auto self = shared_from_this();
      static_cast<void>(launch_tracked([self, relayed_session]() -> asio::awaitable<void> {
         static_cast<void>(co_await self->run_dcutr_initiator(relayed_session->info.remote_peer, relayed_session,
                                                              std::chrono::milliseconds{10'000}));
      }));
   }
}

boost::asio::awaitable<void> node::impl::handle_relay_hop(std::shared_ptr<node::impl::session_state> session,
                                                          forge::net::p2p::stream stream) {
   auto relay_buffer = std::vector<std::uint8_t>{};
   auto request = relay::codec::decode_hop(
       co_await async_read_length_delimited(stream, relay_buffer, reachability::options{}.max_message_size));
   if (request.kind == relay::hop_message::message_kind::reserve) {
      if (!options.relay_policy.service_enabled || !options.capabilities.has(capabilities::relay) ||
          !options.capabilities.has(capabilities::relay_reservation)) {
         co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
             .kind = relay::hop_message::message_kind::status,
             .status = relay::status::permission_denied,
         }));
         co_return;
      }
      if (session->info.path == path::kind::relay) {
         co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
             .kind = relay::hop_message::message_kind::status,
             .status = relay::status::permission_denied,
         }));
         co_return;
      }
      const auto reservation = remember_inbound_relay_reservation(
          session->info.remote_peer, relay::reservation::options{
                                         .ttl = options.limits.relay.reservation_ttl,
                                         .max_streams = options.limits.relay.max_streams_per_reservation,
                                         .max_bytes = options.limits.relay.max_relay_bytes,
                                         .max_queued_bytes = options.limits.relay.max_queued_bytes,
                                     });
      if (!reservation) {
         co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
             .kind = relay::hop_message::message_kind::status,
             .status = relay::status::reservation_refused,
         }));
         co_return;
      }
      auto endpoints = local_endpoints_for_control();
      const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch() + options.limits.relay.reservation_ttl);
      auto voucher = std::optional<signed_envelope>{};
      if (!options.public_key.empty()) {
         voucher = relay::codec::seal_reservation_voucher(
             relay::voucher{
                 .relay_peer = local,
                 .peer = session->info.remote_peer,
                 .expires_at = static_cast<std::uint64_t>(expires_at.count()),
             },
             decode_public_key(identity.public_key), require_libp2p_identity_private_key(identity));
      }
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .reservation_value =
              relay::reservation{
                  .expires_at = static_cast<std::uint64_t>(expires_at.count()),
                  .relay_endpoints = std::move(endpoints),
                  .voucher = std::move(voucher),
              },
          .limit_value =
              relay::limit{
                  .duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration),
                  .data = options.limits.relay.max_relay_bytes,
              },
          .status = relay::status::ok,
      }));
      co_await stream.async_close();
      co_return;
   }

   if (request.kind != relay::hop_message::message_kind::connect || !request.target) {
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .status = relay::status::malformed_message,
      }));
      co_return;
   }
   if (!options.relay_policy.service_enabled) {
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .status = relay::status::permission_denied,
      }));
      co_return;
   }
   const auto relay_owner = request.target->id;
   auto relay_status = relay::status::ok;
   auto relay_resource = begin_relay(relay_owner, relay_status);
   if (!relay_resource) {
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .status = relay_status,
      }));
      co_return;
   }
   const auto reservation_id = relay_resource->reservation_id;
   auto finish_relay_on_exit = [this, relay_owner, reservation_id](void*) noexcept {
      finish_relay(relay_owner, reservation_id);
   };
   auto relay_guard = std::unique_ptr<void, decltype(finish_relay_on_exit)>{this, std::move(finish_relay_on_exit)};

   auto target = std::optional<forge::net::p2p::stream>{};
   try {
      auto target_session = co_await ensure_direct_session(request.target->id);
      target.emplace(co_await open_session_stream(target_session, builtins::relay_stop, true));
      co_await target->async_write(relay::codec::encode_stop(relay::stop_message{
          .kind = relay::stop_message::message_kind::connect,
          .source = relay::peer{.id = session->info.remote_peer},
          .limit_value =
              relay::limit{
                  .duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration),
                  .data = options.limits.relay.max_relay_bytes,
              },
      }));
      auto stop_buffer = std::vector<std::uint8_t>{};
      const auto stop_status = relay::codec::decode_stop(
          co_await async_read_length_delimited(*target, stop_buffer, reachability::options{}.max_message_size));
      if (stop_status.kind != relay::stop_message::message_kind::status || stop_status.status != relay::status::ok) {
         target.reset();
      }
   } catch (...) {
      target.reset();
   }
   if (!target) {
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .status = relay::status::connection_failed,
      }));
      co_return;
   }

   co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
       .kind = relay::hop_message::message_kind::status,
       .limit_value =
           relay::limit{
               .duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration),
               .data = options.limits.relay.max_relay_bytes,
           },
       .status = relay::status::ok,
   }));
   stream = detail::stream_access::with_buffer(std::move(stream), std::move(relay_buffer));
   launch_relay_pumps(relay_owner, std::move(stream), std::move(*target), std::move(*relay_resource));
   static_cast<void>(relay_guard.release());
}

boost::asio::awaitable<void> node::impl::handle_dcutr(std::shared_ptr<node::impl::session_state> session,
                                                      forge::net::p2p::stream stream) {
   auto buffer = std::vector<std::uint8_t>{};
   auto first = co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size);
   auto request = hole_punch::codec::decode(first);
   if (request.kind != hole_punch::message::message_kind::connect) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected CONNECT");
   }
   auto observed = local_endpoints_for_control();
   co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
       .kind = hole_punch::message::message_kind::connect,
       .observed_endpoints = std::move(observed),
   }));
   auto sync_bytes = co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size);
   auto sync = hole_punch::codec::decode(sync_bytes);
   if (sync.kind != hole_punch::message::message_kind::sync) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected SYNC");
   }
   for (const auto& candidate : request.observed_endpoints) {
      try {
         (void)co_await connect_direct(candidate, node::connect_options{
                                                      .expected_peer = session->info.remote_peer,
                                                      .allow_relay = false,
                                                      .timeout = std::chrono::milliseconds{5'000},
                                                  });
         record_hole_punch_result(hole_punch::status::succeeded);
         co_return;
      } catch (const forge::exceptions::base& error) {
         if (detail::remote_peer_attributable_failure(p2p_code(error), false)) {
            record_direct_failure(session->info.remote_peer);
         }
      } catch (...) {
         record_direct_failure(session->info.remote_peer);
      }
   }
   if (co_await wait_for_direct_session(session->info.remote_peer, std::chrono::milliseconds{5'000})) {
      record_hole_punch_result(hole_punch::status::succeeded);
      co_return;
   }
   record_hole_punch_result(hole_punch::status::failed);
}

boost::asio::awaitable<bool> node::impl::wait_for_direct_session(const peer_id& peer,
                                                                 std::chrono::milliseconds timeout) {
   const auto started = std::chrono::steady_clock::now();
   while (std::chrono::steady_clock::now() - started < timeout) {
      const auto observed = lifecycle_wakeup->epoch();
      if (lifecycle.stop_requested()) {
         co_return false;
      }
      if (session_for_path(peer, path::kind::direct)) {
         co_return !lifecycle.stop_requested();
      }
      // Session admission and lifecycle stop both notify this sticky epoch.
      co_await lifecycle_wakeup->async_wait_until(observed, started + timeout);
   }
   co_return false;
}

boost::asio::awaitable<hole_punch::status>
node::impl::run_dcutr_initiator(const peer_id& peer, const std::shared_ptr<session_state>& session,
                                std::chrono::milliseconds timeout) {
   auto observed = local_endpoints_for_control();
   if (observed.empty()) {
      record_hole_punch_result(hole_punch::status::failed);
      co_return hole_punch::status::failed;
   }
   try {
      auto stream = co_await open_session_stream(session, builtins::dcutr);
      const auto sent = std::chrono::steady_clock::now();
      co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
          .kind = hole_punch::message::message_kind::connect,
          .observed_endpoints = observed,
      }));
      auto dcutr_buffer = std::vector<std::uint8_t>{};
      auto response = hole_punch::codec::decode(
          co_await async_read_length_delimited(stream, dcutr_buffer, hole_punch::options{}.max_message_size));
      const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sent);
      if (response.kind != hole_punch::message::message_kind::connect || response.observed_endpoints.empty()) {
         record_hole_punch_result(hole_punch::status::failed);
         co_return hole_punch::status::failed;
      }
      co_await stream.async_write(
          hole_punch::codec::encode(hole_punch::message{.kind = hole_punch::message::message_kind::sync}));
      if (rtt > std::chrono::milliseconds{0}) {
         auto timer = asio::steady_timer{runtime.context()};
         timer.expires_after(rtt / 2);
         co_await timer.async_wait(asio::use_awaitable);
      }
      for (const auto& candidate : response.observed_endpoints) {
         if (lifecycle.stop_requested()) {
            co_return hole_punch::status::failed;
         }
         try {
            (void)co_await connect_direct(candidate, node::connect_options{
                                                         .expected_peer = peer,
                                                         .allow_relay = false,
                                                         .timeout = timeout,
                                                     });
            record_hole_punch_result(hole_punch::status::succeeded);
            co_return hole_punch::status::succeeded;
         } catch (const forge::exceptions::base& error) {
            if (lifecycle.stop_requested() || p2p_code(error) == exceptions::code::canceled) {
               co_return hole_punch::status::failed;
            }
            if (detail::remote_peer_attributable_failure(p2p_code(error), false)) {
               record_direct_failure(peer);
            }
         } catch (...) {
            if (lifecycle.stop_requested()) {
               co_return hole_punch::status::failed;
            }
            record_direct_failure(peer);
         }
      }
      if (co_await wait_for_direct_session(peer, std::min(timeout, std::chrono::milliseconds{5'000}))) {
         record_hole_punch_result(hole_punch::status::succeeded);
         co_return hole_punch::status::succeeded;
      }
   } catch (...) {
   }
   if (!lifecycle.stop_requested()) {
      record_hole_punch_result(hole_punch::status::failed);
   }
   co_return hole_punch::status::failed;
}

void node::impl::launch_relay_pumps(peer_id owner, forge::net::p2p::stream left, forge::net::p2p::stream right,
                                    relay_admission admission) {
   auto self = shared_from_this();
   const auto byte_limit = relay_byte_limit(owner);
   const auto reservation_id = admission.reservation_id;
   auto pair = std::make_shared<detail::relay_pair>(
       std::move(owner), std::move(left), std::move(right), std::move(admission.circuit),
       runtime.context().get_executor(),
       std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration), byte_limit);
   auto finish = [self, pair, reservation_id] {
      if (pair->mark_finished()) {
         self->finish_relay(pair->owner, reservation_id);
      }
   };
   if (!launch_tracked([pair]() -> asio::awaitable<void> {
          if (co_await pair->async_wait_deadline()) {
             pair->cancel_streams();
          }
       })) {
      pair->cancel_streams();
      finish_relay(pair->owner, reservation_id);
      return;
   }
   if (!launch_tracked([self, pair, finish]() -> asio::awaitable<void> {
          try {
             while (true) {
                auto chunk = co_await pair->left.async_read_chunk();
                if (chunk.empty()) {
                   break;
                }
                if (!pair->left_to_right.consume(chunk.size())) {
                   self->record_relay_failure();
                   pair->cancel_streams();
                   break;
                }
                self->record_relay_bytes(chunk.size());
                co_await pair->right.async_write(std::move(chunk));
                if (pair->left_to_right.exhausted()) {
                   break;
                }
             }
          } catch (const forge::exceptions::base& error) {
             if (!is_orderly_stream_close(error)) {
                self->record_relay_failure();
             }
          } catch (...) {
             self->record_relay_failure();
          }
          try {
             co_await pair->right.async_close();
          } catch (...) {
             // Relay cleanup is best-effort after either side closes or fails.
          }
          finish();
       })) {
      finish();
   }
   if (!launch_tracked([self, pair, finish]() -> asio::awaitable<void> {
          try {
             while (true) {
                auto chunk = co_await pair->right.async_read_chunk();
                if (chunk.empty()) {
                   break;
                }
                if (!pair->right_to_left.consume(chunk.size())) {
                   self->record_relay_failure();
                   pair->cancel_streams();
                   break;
                }
                self->record_relay_bytes(chunk.size());
                co_await pair->left.async_write(std::move(chunk));
                if (pair->right_to_left.exhausted()) {
                   break;
                }
             }
          } catch (const forge::exceptions::base& error) {
             if (!is_orderly_stream_close(error)) {
                self->record_relay_failure();
             }
          } catch (...) {
             self->record_relay_failure();
          }
          try {
             co_await pair->left.async_close();
          } catch (...) {
             // Relay cleanup is best-effort after either side closes or fails.
          }
          finish();
       })) {
      finish();
   }
}

boost::asio::awaitable<hole_punch::status>
node::impl::attempt_hole_punch(peer_id peer, std::optional<peer_id> relay_peer, std::chrono::milliseconds timeout) {
   validate_operation_timeout(timeout, "P2P hole punch timeout");
   if (session_for_path(peer, path::kind::direct)) {
      co_return hole_punch::status::succeeded;
   }
   if (!relay_peer) {
      const auto record = store.find(peer);
      if (record) {
         for (const auto& endpoint : record->endpoints) {
            if (endpoint.relay_peer) {
               relay_peer = endpoint.relay_peer;
               break;
            }
         }
      }
   }
   if (!relay_peer) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P hole punching requires a relay peer");
   }
   auto observed = local_endpoints_for_control();
   if (observed.empty()) {
      record_hole_punch_result(hole_punch::status::failed);
      co_return hole_punch::status::failed;
   }
   try {
      static_cast<void>(co_await ensure_relay_session(peer, *relay_peer, timeout));
      if (co_await wait_for_direct_session(peer, timeout)) {
         record_hole_punch_result(hole_punch::status::succeeded);
         co_return hole_punch::status::succeeded;
      }
   } catch (...) {
      // DCUtR failures are expected on many NATs; the caller sees a typed status.
   }
   record_hole_punch_result(hole_punch::status::failed);
   co_return hole_punch::status::failed;
}

} // namespace forge::net::p2p
