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
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>

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

#include "details/host_addresses.hxx"
#include "details/node_impl.hxx"
#include "details/owner_cancellation.hxx"
#include "details/relay_pair.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

bool node::impl::admit_relay_service_locked(const session_state& session) {
   const auto now = std::chrono::steady_clock::now();
   const auto& limits = options.limits.relay;
   std::erase_if(relay_service_requests, [&](const auto& request) {
      return now - request.observed_at >= limits.service_request_window;
   });
   if (!session.remote_endpoint || relay_service_requests.size() >= limits.max_service_requests) { return false; }
   const auto& ip = session.remote_endpoint->transport.host;
   const auto peer_count = std::ranges::count_if(relay_service_requests, [&](const auto& request) {
      return request.peer == session.info.remote_peer;
   });
   const auto ip_count = std::ranges::count_if(relay_service_requests, [&](const auto& request) { return request.ip == ip; });
   if (static_cast<std::size_t>(peer_count) >= limits.max_service_requests_per_peer ||
       static_cast<std::size_t>(ip_count) >= limits.max_service_requests_per_ip) { return false; }
   relay_service_requests.push_back({session.info.remote_peer, ip, now});
   return true;
}

bool node::impl::remember_inbound_relay_reservation(const std::shared_ptr<session_state>& session,
                                                   relay::reservation::options request) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   const auto& owner = session->info.remote_peer;
   const auto live = sessions.find(session->id);
   if (stopped || session_admission_closed || session->closed || session->info.path != path::kind::direct ||
       !session->remote_endpoint || live == sessions.end() || live->second != session) { return false; }
   const auto& ip = session->remote_endpoint->transport.host;
   const auto ip_reservations = std::ranges::count_if(inbound_relay_reservations, [&](const auto& item) {
      return item.first != owner && item.second.remote_ip == ip;
   });
   if (static_cast<std::size_t>(ip_reservations) >= options.limits.relay.max_reservations_per_ip) {
      ++metrics_value.relay_reservation_rejections;
      return false;
   }
   if (inbound_relay_reservations.size() >= options.limits.relay.max_reservations &&
       !inbound_relay_reservations.contains(owner)) {
      ++metrics_value.relay_reservation_rejections;
      return false;
   }
   const auto existing = inbound_relay_reservations.find(owner);
   const auto renewing = existing != inbound_relay_reservations.end();
   const auto ttl = std::min(request.ttl, options.limits.relay.reservation_ttl);
   auto reservation = relay_reservation_state{
       .owner = owner,
       .relay_peer = local,
       .id = renewing ? existing->second.id : next_reservation_id,
       .expires_at = std::chrono::steady_clock::now() + ttl,
       .max_streams = std::min(request.max_streams, options.limits.relay.max_streams_per_reservation),
       .max_bytes = std::min(request.max_bytes, options.limits.relay.max_relay_bytes),
       .max_queued_bytes = std::min(request.max_queued_bytes, options.limits.relay.max_queued_bytes),
       .active_streams = renewing ? existing->second.active_streams : 0,
       .session_id = session->id,
       .remote_ip = ip,
   };
   if (!renewing) {
      auto acquired = resources.reserve_relay(resource_manager::scope{.peer = owner, .protocol = builtins::relay_hop});
      if (!acquired) {
         if (acquired.outcome() == resource_manager::transition_result::policy_rejected) {
            ++metrics_value.relay_reservation_rejections;
            return false;
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P relay reservation resource admission failed");
      }
      reservation.resource = std::move(*acquired);
   }
   const auto discard_new = [this, &owner, renewing](void*) noexcept {
      if (!renewing) { inbound_relay_reservations.erase(owner); }
   };
   auto insertion_guard = std::unique_ptr<void, decltype(discard_new)>{this, discard_new};
   const auto entry = renewing ? existing : inbound_relay_reservations.try_emplace(owner).first;
   // Stage copies, map allocation and protection before transferring old ownership.
   connections.protect(owner, "forge:relay-service");
   static_assert(std::is_nothrow_move_assignable_v<relay_reservation_state>);
   if (renewing) { reservation.resource = std::move(entry->second.resource); }
   entry->second = std::move(reservation);
   if (!renewing) { ++next_reservation_id; }
   static_cast<void>(insertion_guard.release());
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
   ++metrics_value.relay_reservations;
   notify_autorelay_changed();
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
   static_cast<void>(connections.unprotect(owner, "forge:relay-service"));
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
   return true;
}

std::optional<node::impl::relay_admission> node::impl::begin_relay(const peer_id& owner, const peer_id& source,
                                                               relay::status& status) {
   auto lock = std::scoped_lock{mutex};
   cleanup_expired_relay_reservations_locked();
   const auto active_source = relay_peer_active.find(source);
   const auto active_target = relay_peer_active.find(owner);
   const auto source_count = active_source == relay_peer_active.end() ? 0 : active_source->second;
   const auto target_count = active_target == relay_peer_active.end() ? 0 : active_target->second;
   const auto participation = source == owner ? std::size_t{2} : std::size_t{1};
   if (metrics_value.active_relays >= options.limits.relay.max_active_relays ||
       participation > options.limits.relay.max_circuits_per_peer ||
       source_count > options.limits.relay.max_circuits_per_peer - participation ||
       target_count > options.limits.relay.max_circuits_per_peer - participation) {
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
   const auto reservation = inbound_relay_reservations.find(owner);
   if (options.limits.relay.require_reservation && reservation == inbound_relay_reservations.end()) {
      ++metrics_value.relay_rejections;
      status = relay::status::no_reservation;
      return std::nullopt;
   }
   const auto target_limit = reservation == inbound_relay_reservations.end()
       ? options.limits.relay.max_streams_per_reservation : reservation->second.max_streams;
   if (participation > target_limit || target_count > target_limit - participation) {
      ++metrics_value.relay_rejections;
      status = relay::status::resource_limit_exceeded;
      return std::nullopt;
   }
   if (reservation != inbound_relay_reservations.end()) { reservation_id = reservation->second.id; }
   auto admission = relay_admission{.circuit = std::move(*circuit), .reservation_id = reservation_id, .source = source};
   static_assert(std::is_nothrow_move_constructible_v<relay_admission>);
   const auto discard_empty = [this, &source, &owner](void*) noexcept {
      for (const auto* peer : {&source, &owner}) {
         const auto entry = relay_peer_active.find(*peer);
         if (entry != relay_peer_active.end() && entry->second == 0) { relay_peer_active.erase(entry); }
      }
   };
   auto counters_guard = std::unique_ptr<void, decltype(discard_empty)>{this, discard_empty};
   const auto source_entry = relay_peer_active.try_emplace(source, 0).first;
   const auto target_entry = relay_peer_active.try_emplace(owner, 0).first;
   if (reservation != inbound_relay_reservations.end()) {
      ++reservation->second.active_streams;
   }
   ++source_entry->second;
   ++target_entry->second;
   static_cast<void>(counters_guard.release());
   ++metrics_value.active_relays;
   ++metrics_value.relays_opened;
   status = relay::status::ok;
   return admission;
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

void node::impl::finish_relay(const peer_id& owner, std::optional<std::uint64_t> reservation_id, const peer_id& source) {
   auto lock = std::scoped_lock{mutex};
   auto reservation = inbound_relay_reservations.find(owner);
   if (reservation_id && reservation != inbound_relay_reservations.end() && reservation->second.id == *reservation_id &&
       reservation->second.active_streams > 0) {
      --reservation->second.active_streams;
   }
   if (metrics_value.active_relays > 0) {
      --metrics_value.active_relays;
   }
   for (const auto* peer : {&source, &owner}) {
      const auto active = relay_peer_active.find(*peer);
      if (active != relay_peer_active.end() && active->second > 0 && --active->second == 0) { relay_peer_active.erase(active); }
   }
}

void node::impl::erase_inbound_relay_reservation_locked(const peer_id& owner) noexcept {
   inbound_relay_reservations.erase(owner);
   static_cast<void>(connections.unprotect(owner, "forge:relay-service"));
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
}

boost::asio::awaitable<void> node::impl::handle_relay_hop(std::shared_ptr<node::impl::session_state> session,
                                                          forge::net::p2p::stream stream) {
   auto relay_buffer = std::vector<std::uint8_t>{};
   auto request = relay::codec::decode_hop(
       co_await async_read_length_delimited(stream, relay_buffer, reachability::options{}.max_message_size));
   auto admitted = false;
   auto direct = false;
   {
      const auto lock = std::scoped_lock{mutex};
      const auto active = sessions.find(session->id);
      direct = session->info.path == path::kind::direct && !session->closed &&
               active != sessions.end() && active->second == session && !stopped && !session_admission_closed;
      admitted = direct && options.relay_policy.service_enabled && admit_relay_service_locked(*session);
   }
   if (!direct || !options.relay_policy.service_enabled || !admitted) {
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .status = !direct || !options.relay_policy.service_enabled ? relay::status::permission_denied
                                                                   : relay::status::resource_limit_exceeded,
      }));
      co_return;
   }
   if (request.kind == relay::hop_message::message_kind::reserve) {
      if (!options.relay_policy.service_enabled) {
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
          session, relay::reservation::options{
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
      std::erase_if(endpoints, [](const auto& value) {
         return (!value.is_direct_tcp() && !value.is_direct_quic()) || value.relayed ||
                value.transport.port == 0 || host_addresses::has_interface_zone(value) ||
                host_addresses::classify_endpoint_scope(value) == host_addresses::endpoint_scope::unroutable;
      });
      if (endpoints.size() > options.identify.max_listen_endpoints) { endpoints.resize(options.identify.max_listen_endpoints); }
      for (auto& value : endpoints) { value.peer = local; }
      const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch() + options.limits.relay.reservation_ttl);
      auto voucher = std::optional<signed_envelope>{};
      if (identity.private_key && !identity.public_key.empty()) {
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
   auto relay_resource = std::optional<relay_admission>{};
   const auto finish_relay_on_exit = [this, &relay_owner, &relay_resource, &session](void*) noexcept {
      if (relay_resource) { finish_relay(relay_owner, relay_resource->reservation_id, session->info.remote_peer); }
   };
   auto relay_guard = std::unique_ptr<void, decltype(finish_relay_on_exit)>{this, finish_relay_on_exit};
   relay_resource = begin_relay(relay_owner, session->info.remote_peer, relay_status);
   if (!relay_resource) {
      co_await stream.async_write(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .status = relay_status,
      }));
      co_return;
   }
   const auto reservation_id = relay_resource->reservation_id;

   auto target = std::optional<forge::net::p2p::stream>{};
   auto target_session = std::shared_ptr<session_state>{};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto tracked = lifecycle.track();
   auto deadline = operation_deadline{runtime.context(), options.limits.relay.handshake_timeout};
   deadline.arm([stop]() noexcept { stop->request_stop(); });
   auto handshake_failure = std::exception_ptr{};
   try {
      {
         const auto lock = std::scoped_lock{mutex};
         const auto owner = inbound_relay_reservations.find(relay_owner);
         if (owner != inbound_relay_reservations.end()) {
            const auto active = sessions.find(owner->second.session_id);
            if (active != sessions.end() && !active->second->closed &&
                active->second->info.path == path::kind::direct && active->second->info.remote_peer == relay_owner) {
               target_session = active->second;
            }
         } else if (!options.limits.relay.require_reservation) {
            target_session = session_for_path_locked(relay_owner, path::kind::direct, std::nullopt);
         }
      }
      if (!target_session) {
         FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay destination has no connected direct reservation owner");
      }
      if (!tracked.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P relay service is stopping"); }
      co_await detail::async_run_with_owner_cancellation(stop,
          [this, target_session, source = session->info.remote_peer, stop, &target](
              boost::asio::cancellation_slot slot) -> asio::awaitable<void> {
             auto owned = std::make_shared<forge::net::p2p::stream>();
             auto cancellation = detail::owner_stream_cancellation{slot, owned};
             auto admission = detail::make_owner_stream_admission(std::move(slot), stop);
             auto failure = std::exception_ptr{};
             try {
                *owned = co_await open_session_stream(target_session, builtins::relay_stop, true, std::move(admission));
                if (stop->stop_requested()) {
                   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay STOP handshake canceled");
                }
                co_await owned->async_write(relay::codec::encode_stop(relay::stop_message{
                    .kind = relay::stop_message::message_kind::connect,
                    .source = relay::peer{.id = source},
                    .limit_value = relay::limit{
                        .duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration),
                        .data = options.limits.relay.max_relay_bytes,
                    },
                }));
                auto stop_buffer = std::vector<std::uint8_t>{};
                const auto stop_status = relay::codec::decode_stop(
                    co_await async_read_length_delimited(*owned, stop_buffer, reachability::options{}.max_message_size));
                if (stop_status.kind != relay::stop_message::message_kind::status || stop_status.status != relay::status::ok) {
                   FORGE_THROW_EXCEPTION(exceptions::relay_rejected, "P2P relay STOP rejected");
                }
             } catch (...) { failure = std::current_exception(); }
             if (failure) {
                co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
                cancellation.request_cancel();
                try { co_await owned->async_close(); } catch (...) {}
                std::rethrow_exception(failure);
             }
             target.emplace(std::move(*owned));
          }, {.lifecycle_stop = tracked.stop_source()});
   } catch (...) { handshake_failure = std::current_exception(); }
   const auto completed = deadline.finish();
   auto live_owners = false;
   {
      const auto lock = std::scoped_lock{mutex};
      const auto source = sessions.find(session->id);
      const auto destination = target_session ? sessions.find(target_session->id) : sessions.end();
      const auto owner = inbound_relay_reservations.find(relay_owner);
      live_owners = !stopped && !session_admission_closed && !session->closed &&
          source != sessions.end() && source->second == session && target_session && !target_session->closed &&
          destination != sessions.end() && destination->second == target_session &&
          (!reservation_id || (owner != inbound_relay_reservations.end() && !owner->second.canceled &&
           owner->second.id == *reservation_id && owner->second.session_id == target_session->id &&
           owner->second.expires_at > std::chrono::steady_clock::now()));
   }
   if (handshake_failure || !completed || deadline.timed_out() || deadline.stopped() || stop->stop_requested() || !live_owners) {
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      if (target) {
         target->request_cancel();
         try { co_await target->async_close(); } catch (...) {}
      }
      target.reset();
   }
   if (!target) {
      relay_guard.reset();
      relay_resource.reset();
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

void node::impl::launch_relay_pumps(peer_id owner, forge::net::p2p::stream left, forge::net::p2p::stream right,
                                    relay_admission admission) {
   auto self = shared_from_this();
   const auto byte_limit = relay_byte_limit(owner);
   const auto reservation_id = admission.reservation_id;
   const auto source = admission.source;
   auto pair = std::make_shared<detail::relay_pair>(
       std::move(owner), std::move(left), std::move(right), std::move(admission.circuit),
       runtime.context().get_executor(),
       std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration), byte_limit);
   auto finish = [self, pair, reservation_id, source] {
      if (pair->mark_finished()) {
         self->finish_relay(pair->owner, reservation_id, source);
      }
   };
   auto deadline_work = std::function<asio::awaitable<void>()>{[pair]() -> asio::awaitable<void> {
          if (co_await pair->async_wait_deadline()) {
             pair->cancel_streams();
          }
       }};
   auto left_work = std::function<asio::awaitable<void>()>{[self, pair, finish]() -> asio::awaitable<void> {
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
       }};
   auto right_work = std::function<asio::awaitable<void>()>{[self, pair, finish]() -> asio::awaitable<void> {
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
       }};
   // Complete all throwing task staging before any worker can own completion.
   if (!launch_tracked(std::move(deadline_work))) {
      pair->cancel_streams();
      finish();
      return;
   }
   if (!launch_tracked(std::move(left_work))) { pair->cancel_streams(); finish(); }
   if (!launch_tracked(std::move(right_work))) { pair->cancel_streams(); finish(); }
}


} // namespace forge::net::p2p
