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
import forge.net.p2p.topology;
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

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::ensure_pubsub_direct_session(const peer_id& peer) {
   auto participant = detail::connection_singleflight_registry::lease{};
   auto start = std::optional<detail::connection_singleflight_registry::operation>{};
   auto tracked = detail::session_teardown::ticket{};
   auto existing = std::shared_ptr<session_state>{};
   {
      auto lock = std::scoped_lock{mutex};
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
   auto release_participant = [this, &participant](void*) noexcept {
      auto lock = std::scoped_lock{mutex};
      pubsub_value.connection_gates.leave(participant);
   };
   auto participant_guard = std::unique_ptr<void, decltype(release_participant)>{this, std::move(release_participant)};

   if (start) {
      auto self = shared_from_this();
      auto active = std::make_shared<detail::connection_singleflight_registry::operation>(std::move(*start));
      auto tracked_operation = std::make_shared<detail::session_teardown::ticket>(std::move(tracked));
      if (!launch_tracked([self, peer, active, tracked_operation]() mutable -> boost::asio::awaitable<void> {
             static_cast<void>(tracked_operation);
             try {
                static_cast<void>(
                    co_await self->ensure_direct_session(peer, self->options.limits.topology.query_timeout));
                auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.succeed(*active);
             } catch (const forge::exceptions::base& error) {
                auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.fail(*active, p2p_code(error), error.what());
             } catch (...) {
                auto lock = std::scoped_lock{self->mutex};
                self->pubsub_value.connection_gates.fail(*active, exceptions::code::internal,
                                                         "GossipSub peer connection failed internally");
             }
             co_return;
          })) {
         auto lock = std::scoped_lock{mutex};
         pubsub_value.connection_gates.fail(*active, exceptions::code::internal,
                                            "GossipSub peer connection could not be started");
      }
   }

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
   if (!result.succeeded) {
      FORGE_THROW_CODE(result.error.value_or(exceptions::code::internal), std::move(result.message));
   }
   if (auto connected = session_for_path(peer, path::kind::direct)) {
      co_return connected;
   }
   FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub direct session closed after connection singleflight");
}

boost::asio::awaitable<void> node::impl::send_pubsub_rpc(const peer_id& peer, const pubsub::rpc& value) {
   auto protocol = builtins::meshsub_v11;
   if (options.limits.pubsub.allow_v1_0_fallback) {
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
   const auto encoded = pubsub::codec::encode(value, options.limits.pubsub);
   auto reserved_bytes = encoded.size();
   reserve_pubsub_outbound_bytes(peer, reserved_bytes);
   auto release_bytes = [this, peer, &reserved_bytes](void*) noexcept {
      release_pubsub_outbound_bytes(peer, reserved_bytes);
   };
   auto reservation = std::unique_ptr<void, decltype(release_bytes)>{this, std::move(release_bytes)};
   try {
      auto session = co_await ensure_pubsub_direct_session(peer);
      while (true) {
         auto session_id = std::uint64_t{};
         auto write_gate = std::shared_ptr<forge::asio::gate>{};
         {
            auto lock = std::scoped_lock{mutex};
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
            if (selected_session == sessions.end() || selected_session->second != session || session->closed) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub direct session closed before publication");
            }
            if (current == pubsub_value.outbound.end()) {
               pubsub_value.outbound[peer] = pubsub_state::outbound_generation{
                   .session_id = session->id,
                   .write_gate = std::make_shared<forge::asio::gate>(),
               };
            }
            session_id = pubsub_value.outbound.at(peer).session_id;
            write_gate = pubsub_value.outbound.at(peer).write_gate;
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
            }
         }
         if (replace_generation) {
            write_ticket.release();
            continue;
         }

         try {
            if (!outbound) {
               const auto open_timeout =
                   attempt_timeout(options.limits.topology.query_timeout, node::open_options{}.direct_attempt_timeout,
                                   "GossipSub protocol open direct attempt");
               auto stream = co_await open_protocol_on_direct_session(peer, protocol, session, open_timeout);
               outbound = std::make_shared<forge::net::p2p::stream>(std::move(stream));
               auto stale = false;
               {
                  auto lock = std::scoped_lock{mutex};
                  const auto current = pubsub_value.outbound.find(peer);
                  stale = stopped || current == pubsub_value.outbound.end() ||
                          current->second.session_id != session_id || current->second.write_gate != write_gate;
                  if (!stale) {
                     current->second.stream = outbound;
                  }
               }
               if (stale) {
                  outbound->cancel();
                  FORGE_THROW_EXCEPTION(exceptions::closed,
                                        "GossipSub peer stream closed while opening publication stream");
               }
            }

            auto snapshot_pending = false;
            {
               auto lock = std::scoped_lock{mutex};
               const auto current = pubsub_value.outbound.find(peer);
               if (stopped || current == pubsub_value.outbound.end() || current->second.session_id != session_id ||
                   current->second.write_gate != write_gate || current->second.stream != outbound) {
                  FORGE_THROW_EXCEPTION(exceptions::closed, "GossipSub peer stream was replaced before publication");
               }
               snapshot_pending = current->second.snapshot_pending;
            }
            if (snapshot_pending) {
               auto subscriptions = local_pubsub_subscriptions();
               if (!subscriptions.empty()) {
                  const auto snapshot = pubsub::codec::encode(pubsub::rpc{.subscriptions = std::move(subscriptions)},
                                                              options.limits.pubsub);
                  reserve_pubsub_outbound_bytes(peer, snapshot.size());
                  reserved_bytes += snapshot.size();
                  co_await outbound->async_write(snapshot);
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
            co_await outbound->async_write(encoded);
         } catch (...) {
            auto lock = std::scoped_lock{mutex};
            invalidate_pubsub_outbound_locked(peer, session_id, write_gate, outbound);
            throw;
         }
         break;
      }
   } catch (...) {
      reservation.reset();
      throw;
   }
   reservation.reset();
}

void node::impl::record_pubsub_send_failure(const peer_id& peer, const forge::exceptions::base& error) {
   const auto kind = p2p_code(error);
   auto node_stopped = false;
   {
      auto lock = std::scoped_lock{mutex};
      node_stopped = stopped;
   }
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
   auto subscriptions = local_pubsub_subscriptions();
   if (subscriptions.empty()) {
      co_return;
   }
   try {
      co_await send_pubsub_rpc(peer, pubsub::rpc{.subscriptions = std::move(subscriptions)});
   } catch (const forge::exceptions::base& error) {
      record_pubsub_send_failure(peer, error);
   }
}

} // namespace forge::net::p2p
