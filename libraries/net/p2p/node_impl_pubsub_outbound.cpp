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
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
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
import forge.net.p2p.envelope;
import forge.net.p2p.rendezvous;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/certified_peer_record.hxx"
#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error);
[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept;

boost::asio::awaitable<std::vector<std::uint8_t>> async_read_length_delimited(forge::net::p2p::stream& stream,
                                                                              std::vector<std::uint8_t>& buffer,
                                                                              std::size_t max_payload_size);

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::ensure_pubsub_direct_session(const peer_id& peer, std::shared_ptr<session_state> origin) {
   auto participant = detail::connection_singleflight_registry::lease{};
   auto start = std::optional<detail::connection_singleflight_registry::operation>{};
   auto tracked = detail::session_teardown::ticket{};
   auto existing = std::shared_ptr<session_state>{};
   auto active = std::shared_ptr<detail::connection_singleflight_registry::operation>{};
   auto rollback_preparation = [this, &start, &active, &participant](void*) noexcept {
      if (start) {
         const auto lock = std::scoped_lock{mutex};
         pubsub_value.connection_gates.rollback_unpublished(active ? *active : *start, participant);
      }
   };
   auto preparation = std::unique_ptr<void, decltype(rollback_preparation)>{this, std::move(rollback_preparation)};
   {
      auto lock = std::scoped_lock{mutex};
      if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) {
         co_return std::shared_ptr<session_state>{};
      }
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot connect GossipSub peer after node shutdown");
      }
      existing = session_for_path_locked(peer, path::kind::direct, std::nullopt);
      if (!existing) {
         auto joined = pubsub_value.connection_gates.join(peer, runtime.context().get_executor());
         participant = std::move(joined.participant);
         start = std::move(joined.start);
         if (start) {
            tracked = teardown.track();
         }
      }
   }
   if (existing) {
      co_return existing;
   }
   auto release_participant = [this, &participant, &start, &preparation](void*) noexcept {
      // The preparation owner must settle coalesced waiters before releasing its initiating lease.
      if (start && preparation) {
         return;
      }
      auto lock = std::scoped_lock{mutex};
      pubsub_value.connection_gates.leave(participant);
   };
   auto participant_guard = std::unique_ptr<void, decltype(release_participant)>{this, std::move(release_participant)};

   if (start) {
      auto self = shared_from_this();
      active = std::make_shared<detail::connection_singleflight_registry::operation>(std::move(*start));
      auto tracked_operation = std::make_shared<detail::session_teardown::ticket>(std::move(tracked));
      if (!launch_tracked([self, peer, active, tracked_operation]() mutable -> boost::asio::awaitable<void> {
             static_cast<void>(tracked_operation);
             auto terminal_code = exceptions::code::internal;
             auto finish = [&self, &active, &terminal_code](void*) noexcept {
                const auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.fail(*active, terminal_code, {});
             };
             auto terminal = std::unique_ptr<void, decltype(finish)>{self.get(), std::move(finish)};
             try {
                static_cast<void>(
                    co_await self->ensure_direct_session(peer, self->options.limits.topology.query_timeout));
                auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.succeed(*active);
             } catch (const forge::exceptions::base& error) {
                terminal_code = p2p_code(error);
                auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.fail(*active, terminal_code, error.what());
             } catch (...) {
                auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.fail(*active, exceptions::code::internal,
                                                         "GossipSub peer connection failed internally");
             }
             co_return;
          })) {
         preparation.reset();
         FORGE_THROW_EXCEPTION(exceptions::internal, "GossipSub peer connection could not be started");
      }
   }
   static_cast<void>(preparation.release());

   auto result = detail::connection_singleflight_registry::outcome{};
   try {
      result = co_await participant.wait();
   } catch (const boost::system::system_error& error) {
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "GossipSub peer connection canceled while waiting");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "GossipSub peer connection wait failed",
                            forge::exceptions::ctx("reason", error.code().message()));
   }
   {
      const auto lock = std::scoped_lock{mutex};
      if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) {
         co_return std::shared_ptr<session_state>{};
      }
      if (!result.succeeded) {
         FORGE_THROW_CODE(result.error.value_or(exceptions::code::internal), std::move(result.message));
      }
      if (auto connected = session_for_path_locked(peer, path::kind::direct, std::nullopt)) {
         co_return connected;
      }
   }
   FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub direct session closed after connection singleflight");
}

boost::asio::awaitable<bool> node::impl::send_pubsub_rpc(const peer_id& peer, pubsub::rpc rpc,
    std::optional<std::uint64_t>& send_generation,
    std::shared_ptr<const detail::pubsub_control_queue::batch> control, bool check_intents,
    std::shared_ptr<session_state> origin) {
   send_generation.reset();
   if (control || origin) {
      const auto lock = std::scoped_lock{mutex};
      if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) { co_return false; }
      if (control && !validate_pubsub_control_locked(*control, check_intents)) { co_return false; }
   }
   if (!control && options.limits.pubsub.peer_exchange && rpc.control_value && !rpc.control_value->prunes.empty()) {
      auto eligible = std::set<peer_id>{};
      {
         const auto lock = std::scoped_lock{mutex};
         if (pubsub_score_locked(peer) >= 0) {
            for (const auto& [candidate, state] : pubsub_value.peers) {
               if (candidate != peer && state.connected && pubsub_score_locked(candidate) >= 0) {
                  eligible.insert(candidate);
               }
            }
         }
      }
      auto records = std::vector<pubsub::peer_info>{};
      for (const auto& candidate : eligible) {
         if (records.size() >= options.limits.pubsub.limits.max_peers_per_topic) {
            break;
         }
         const auto record = store.find(candidate);
         if (!record || record->signed_peer_record.empty()) {
            continue;
         }
         try {
            static_cast<void>(detail::certified_peer_record::open(signed_envelope::decode(record->signed_peer_record), candidate));
            records.push_back(pubsub::peer_info{.peer = candidate, .signed_peer_record = record->signed_peer_record});
         } catch (const forge::exceptions::base&) {
            // Store hydration alone is not authentication of forwarded PX envelopes.
         }
      }
      for (auto& prune : rpc.control_value->prunes) {
         prune.peers = records;
      }
   }
   auto protocol = pubsub::codec::protocol(options.limits.pubsub.preferred);
   if (options.limits.pubsub.preferred == pubsub::version::v1_1 && options.limits.pubsub.allow_v1_0_fallback) {
      const auto record = store.find(peer);
      const auto supports_v11 = record && std::ranges::any_of(record->protocols, [](const protocol_id& value) {
                                   return value == builtins::meshsub_v11;
                                });
      const auto supports_v10 = record && std::ranges::any_of(record->protocols, [](const protocol_id& value) {
                                   return value == builtins::meshsub_v10;
                                });
      if (supports_v10 && !supports_v11) {
         protocol = builtins::meshsub_v10;
      }
   }
   auto wire_options = options.limits.pubsub;
   wire_options.preferred = protocol == builtins::meshsub_v10 ? pubsub::version::v1_0 : pubsub::version::v1_1;
   auto encoded = pubsub::codec::encode(rpc, wire_options);
   auto reserved_bytes = encoded.size();
   auto rpc_reserved = encoded.size();
   reserve_pubsub_outbound_bytes(peer, reserved_bytes);
   auto release_bytes = [this, &peer, &reserved_bytes](void*) noexcept {
      release_pubsub_outbound_bytes(peer, reserved_bytes);
   };
   static_assert(std::is_nothrow_move_constructible_v<decltype(release_bytes)>);
   auto reservation = std::unique_ptr<void, decltype(release_bytes)>{this, std::move(release_bytes)};
   auto written = false;
   try {
      auto session = co_await ensure_pubsub_direct_session(peer, origin);
      if (!session) { co_return false; }
      while (true) {
         auto session_id = std::uint64_t{};
         auto generation = std::uint64_t{};
         auto write_gate = std::shared_ptr<forge::asio::gate>{};
         {
            auto lock = std::scoped_lock{mutex};
            if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) { co_return false; }
            if (stopped) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub direct session closed before publication");
            }
            auto current = pubsub_value.outbound.find(peer);
            if (current != pubsub_value.outbound.end()) {
               const auto owner_session = sessions.find(current->second.session_id);
               const auto owner_live = owner_session != sessions.end() && !owner_session->second->closed;
               if (owner_live && current->second.write_gate && !current->second.write_gate->closed()) {
                  session = owner_session->second;
               } else {
                  const auto owner_id = current->second.session_id;
                  const auto owner_gate = current->second.write_gate;
                  invalidate_pubsub_outbound_locked(peer, owner_id, owner_gate);
                  current = pubsub_value.outbound.end();
               }
            }
            const auto selected_session = sessions.find(session->id);
            if (selected_session == sessions.end() || selected_session->second != session || session->closed ||
                session->info.remote_peer != peer || session->authentication == peer_authentication::unverified) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub direct session closed before publication");
            }
            // Bind attribution to the selected authenticated session before any gate/open/write await.
            const auto lifetime = pubsub_value.peers.find(peer);
            send_generation = lifetime == pubsub_value.peers.end() ? std::nullopt
                : std::optional<std::uint64_t>{lifetime->second.generation};
            if (current == pubsub_value.outbound.end()) {
               pubsub_value.outbound.emplace(peer, pubsub_state::outbound_generation{
                   .session_id = session->id,
                   .generation = pubsub_value.next_outbound_generation++,
                   .write_gate = std::make_shared<forge::asio::gate>(),
               });
            }
            session_id = pubsub_value.outbound.at(peer).session_id;
            write_gate = pubsub_value.outbound.at(peer).write_gate;
            generation = pubsub_value.outbound.at(peer).generation;
         }

         auto write_ticket = forge::asio::gate::ticket{};
         try {
            write_ticket = co_await write_gate->acquire();
         } catch (const forge::asio::exceptions::canceled&) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "GossipSub publication canceled while waiting for peer stream");
         } catch (const forge::asio::exceptions::rejected&) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub peer stream closed while waiting for publication");
         }

         auto outbound = std::shared_ptr<forge::net::p2p::stream>{};
         auto replace_generation = false;
         {
            auto lock = std::scoped_lock{mutex};
            if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) { co_return false; }
            const auto current = pubsub_value.outbound.find(peer);
            if (stopped || current == pubsub_value.outbound.end() || current->second.session_id != session_id ||
                current->second.write_gate != write_gate) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub peer stream was closed before publication");
            }
            if (current->second.stream && !current->second.stream->valid()) {
               const auto dead_stream = current->second.stream;
               invalidate_pubsub_outbound_locked(peer, session_id, write_gate, dead_stream);
               replace_generation = true;
            } else {
               outbound = current->second.stream;
               if (outbound) {
                  protocol = current->second.protocol;
               }
            }
         }
         if (replace_generation) {
            write_ticket.release();
            continue;
         }

         auto snapshot = std::vector<std::uint8_t>{};
         auto snapshot_written = false;
         auto rpc_written = false;
         auto failure = std::exception_ptr{};
         auto snapshot_pending = false;
         {
            const auto lock = std::scoped_lock{mutex};
            if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) { co_return false; }
            const auto current = pubsub_value.outbound.find(peer);
            if (stopped || current == pubsub_value.outbound.end() || current->second.session_id != session_id ||
                current->second.write_gate != write_gate || current->second.stream != outbound) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub peer stream was replaced before publication");
            }
            snapshot_pending = current->second.snapshot_pending;
         }
         // Prepare all encodings and byte admission before opening/publishing a native stream. Local pressure
         // or allocation failure must not invalidate a healthy generation or prune its mesh membership.
         wire_options.preferred = protocol == builtins::meshsub_v10 ? pubsub::version::v1_0 : pubsub::version::v1_1;
         encoded = pubsub::codec::encode(rpc, wire_options);
         if (encoded.size() > rpc_reserved) {
            const auto additional = encoded.size() - rpc_reserved;
            reserve_pubsub_outbound_bytes(peer, additional);
            reserved_bytes += additional;
            rpc_reserved = encoded.size();
         }
         if (snapshot_pending) {
            auto subscriptions = local_pubsub_subscriptions();
            if (!subscriptions.empty()) {
               snapshot = pubsub::codec::encode(pubsub::rpc{.subscriptions = std::move(subscriptions)}, wire_options);
               reserve_pubsub_outbound_bytes(peer, snapshot.size());
               reserved_bytes += snapshot.size();
            }
         }
         auto fallback_encoded = std::vector<std::uint8_t>{};
         if (!outbound && protocol == builtins::meshsub_v11 && options.limits.pubsub.allow_v1_0_fallback) {
            auto fallback_options = wire_options;
            fallback_options.preferred = pubsub::version::v1_0;
            fallback_encoded = pubsub::codec::encode(rpc, fallback_options);
            if (fallback_encoded.size() > rpc_reserved) {
               const auto additional = fallback_encoded.size() - rpc_reserved;
               reserve_pubsub_outbound_bytes(peer, additional);
               reserved_bytes += additional;
               rpc_reserved = fallback_encoded.size();
            }
         }
         auto prepared_stream = outbound ? std::shared_ptr<forge::net::p2p::stream>{}
                                         : std::make_shared<forge::net::p2p::stream>();
         auto owned_snapshot = forge::net::transport::chunk{snapshot};
         // Retain encoded bytes only for the immutable enabled tracer's lazy write event.
         auto owned_rpc = forge::net::transport::chunk{options.limits.pubsub.tracer ? encoded : std::move(encoded)};
         auto owned_fallback = forge::net::transport::chunk{
             options.limits.pubsub.tracer ? fallback_encoded : std::move(fallback_encoded)};
         auto open_phase = detail::stream_open_phase::admission;
         auto preparing_write = false;
         auto writing = false;
         const auto fail = [&](bool invalidate) {
            failure = std::current_exception();
            if (invalidate) {
               const auto lock = std::scoped_lock{mutex};
               invalidate_pubsub_outbound_locked(peer, session_id, write_gate, outbound);
            }
         };
         try {
            {
               const auto lock = std::scoped_lock{mutex};
               if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) { co_return false; }
            }
            if (!outbound) {
               const auto open_timeout =
                   attempt_timeout(options.limits.topology.query_timeout, node::open_options{}.direct_attempt_timeout,
                                   "GossipSub protocol open direct attempt");
               const auto started = std::chrono::steady_clock::now();
               auto stream = forge::net::p2p::stream{};
               auto fallback = false;
               try {
                  stream = co_await open_protocol_on_direct_session(peer, protocol, session, open_timeout, {}, &open_phase);
               } catch (const forge::exceptions::base& error) {
                  if (protocol != builtins::meshsub_v11 || !options.limits.pubsub.allow_v1_0_fallback ||
                      p2p_code(error) != exceptions::code::unsupported_protocol) {
                     throw;
                  }
                  fallback = true;
               }
               if (fallback) {
                  {
                     const auto lock = std::scoped_lock{mutex};
                     if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) {
                        co_return false;
                     }
                  }
                  protocol = builtins::meshsub_v10;
                  encoded = std::move(fallback_encoded);
                  owned_rpc = std::move(owned_fallback);
                  stream = co_await open_protocol_on_direct_session(peer, protocol, session,
                     remaining_timeout(started, open_timeout, "GossipSub fallback negotiation"), {}, &open_phase);
               }
               outbound = std::move(prepared_stream);
               *outbound = std::move(stream);
               auto stale = false;
               auto stale_origin = false;
               {
                  auto lock = std::scoped_lock{mutex};
                  const auto current = pubsub_value.outbound.find(peer);
                  stale_origin = origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin));
                  stale = stopped || current == pubsub_value.outbound.end() ||
                          current->second.session_id != session_id || current->second.write_gate != write_gate;
                  if (!stale && !stale_origin) {
                     current->second.stream = outbound;
                     current->second.protocol = protocol;
                  }
               }
               if (stale_origin) {
                  outbound->cancel();
                  co_return false;
               }
               if (stale) {
                  outbound->cancel();
                  FORGE_THROW_EXCEPTION(exceptions::closed,
                                        "GossipSub peer stream closed while opening publication stream");
               }
            }

            if (snapshot_pending) {
               if (!snapshot.empty()) {
                  preparing_write = true;
                  auto write = outbound->async_write(std::move(owned_snapshot));
                  preparing_write = false;
                  {
                     const auto lock = std::scoped_lock{mutex};
                     if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) {
                        co_return false;
                     }
                  }
                  writing = true;
                  co_await std::move(write);
                  writing = false;
                  snapshot_written = true;
               }
               auto lock = std::scoped_lock{mutex};
               const auto current = pubsub_value.outbound.find(peer);
               if (stopped || current == pubsub_value.outbound.end() || current->second.session_id != session_id ||
                   current->second.write_gate != write_gate || current->second.stream != outbound) {
                  FORGE_THROW_EXCEPTION(exceptions::closed,
                                        "GossipSub peer stream was replaced after subscription snapshot");
               }
               current->second.snapshot_pending = false;
            }
            preparing_write = true;
            auto write = outbound->async_write(std::move(owned_rpc));
            preparing_write = false;
            auto current_control = true;
            {
               const auto lock = std::scoped_lock{mutex};
               const auto current = pubsub_value.outbound.find(peer);
               if (stopped || current == pubsub_value.outbound.end() || current->second.session_id != session_id ||
                   current->second.generation != generation || current->second.write_gate != write_gate ||
                   current->second.stream != outbound) {
                  FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub peer stream replaced before native write");
               }
               // After stream-open AND subscription-snapshot awaits, with the peer write ticket held.
               // No allocations or suspension between this admission and the lower control write.
               current_control = !origin || (origin->info.remote_peer == peer && pubsub_session_live_locked(origin));
               if (control) { current_control = validate_pubsub_control_locked(*control, check_intents) && current_control; }
            }
            if (current_control) {
               writing = true;
               co_await std::move(write);
               writing = false;
               rpc_written = true;
               written = true;
               if (control && check_intents) {
                  const auto lock = std::scoped_lock{mutex};
                  pubsub_value.controls->acknowledge(control);
               }
            }
         } catch (const forge::exceptions::base& error) {
            // Only provenance from local admission is neutral. Downstream write failures,
            // negotiation and scoped binding can already have native effects.
            const auto local_rejection =
               (!outbound && open_phase == detail::stream_open_phase::local_rejected &&
                  exceptions::is(error, exceptions::code::backpressure_rejected)) ||
               (!outbound && open_phase == detail::stream_open_phase::local_failed &&
                  exceptions::is(error, exceptions::code::internal)) ||
               (writing && detail::resource_stream::is_memory_rejection(error));
            fail(!local_rejection);
         } catch (const std::bad_alloc& error) {
            auto prepared_failure = writing ? detail::resource_stream::preparation_failure(error) : std::exception_ptr{};
            if (preparing_write) {
               failure = std::current_exception(); // Awaitable creation failed before the lower body ran.
            } else if (prepared_failure) {
               failure = std::move(prepared_failure);
            } else {
               fail(outbound || open_phase != detail::stream_open_phase::admission);
            }
         } catch (...) {
            fail(true);
         }
         write_ticket.release();
         // Passive callbacks can reenter snapshots/publication, so neither mutex nor write gate is held.
         if (snapshot_written) {
            trace_pubsub([&] {
               return pubsub::trace_event{.kind = pubsub::trace_kind::rpc_write, .peer = peer,
                  .generation = generation, .protocol = protocol, .session_id = session_id,
                  .stream_id = outbound->id(), .framed_rpc = snapshot};
            });
         }
         if (rpc_written) {
            trace_pubsub([&] {
               return pubsub::trace_event{.kind = pubsub::trace_kind::rpc_write, .peer = peer,
                  .generation = generation, .protocol = protocol, .session_id = session_id,
                  .stream_id = outbound->id(), .framed_rpc = encoded};
            });
         }
         if (failure) {
            std::rethrow_exception(failure);
         }
         break;
      }
   } catch (...) {
      reservation.reset();
      throw;
   }
   reservation.reset();
   co_return written;
}

void node::impl::record_pubsub_send_failure(const peer_id& peer, const forge::exceptions::base& error,
                                           std::optional<std::uint64_t> expected_generation,
                                           std::shared_ptr<session_state> origin) {
   if (!expected_generation) { return; } // Local preparation/dial has not selected a send lifetime.
   const auto kind = p2p_code(error);
   const auto lock = std::scoped_lock{mutex};
   const auto node_stopped = stopped;
   const auto peer_state = pubsub_value.peers.find(peer);
   if (node_stopped || (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) ||
       peer_state == pubsub_value.peers.end() ||
       peer_state->second.generation != *expected_generation) {
      return;
   }
   // Keep the lifetime stable through synchronous store/routing mutation.
   // Same-generation terminal failures still count even after connected became false.
   if (!detail::remote_peer_attributable_failure(kind, node_stopped)) {
      return;
   }
   store.mark_failure(peer);
   for (auto& [_, state] : dht_profiles) {
      state->routing.mark_failure(peer);
   }
}

boost::asio::awaitable<void> node::impl::announce_pubsub_subscriptions(const peer_id& peer) {
   if (!options.capabilities.has(capabilities::pubsub)) {
      co_return;
   }
   {
      const auto lock = std::scoped_lock{mutex};
      if (!connect_pubsub_peer_locked(peer)) {
         co_return;
      }
   }
   auto subscriptions = local_pubsub_subscriptions();
   if (subscriptions.empty()) {
      co_return;
   }
   auto send_generation = std::optional<std::uint64_t>{};
   try {
      co_await send_pubsub_rpc(peer, pubsub::rpc{.subscriptions = std::move(subscriptions)}, send_generation);
   } catch (const forge::exceptions::base& error) {
      record_pubsub_send_failure(peer, error, send_generation);
   }
}

} // namespace forge::net::p2p
