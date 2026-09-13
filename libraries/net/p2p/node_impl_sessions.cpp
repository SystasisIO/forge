module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
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
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
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
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/direct_transport.hxx"
#include "details/cancellation_latch.hxx"
#include "details/node_impl.hxx"
#include "details/owner_cancellation.hxx"
#include "details/path_selector.hxx"
#include "details/peer_exchange_codec.hxx"
#include "details/peer_failure.hxx"
#include "details/resource_stream.hxx"
#include "details/session_lifecycle.hxx"
#include "details/session_retirement.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] bool listener_is_active(const direct::registry& registry, const forge::net::p2p::endpoint& endpoint) {
   const auto active = registry.local_endpoints();
   return std::ranges::any_of(active, [&](const auto& candidate) {
      return candidate.transport.host_type == endpoint.transport.host_type &&
             candidate.transport.protocol == endpoint.transport.protocol &&
             candidate.transport.host == endpoint.transport.host && candidate.transport.port == endpoint.transport.port;
   });
}

boost::asio::awaitable<std::exception_ptr> async_close_terminal(forge::net::transport::session& connection) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   try {
      co_await connection.async_close();
   } catch (...) {
      // transport::session reports failures only after terminal cleanup. Keep
      // the owner through that barrier, then make cancellation idempotent.
      detail::request_session_cancel(connection);
      co_return std::current_exception();
   }
   co_return std::exception_ptr{};
}

std::shared_ptr<node::impl::session_state>
node::impl::retire_session_locked(const std::shared_ptr<session_state>& session, bool track_close) noexcept {
   const auto active = sessions.find(session->id);
   if (active == sessions.end()) {
      return {};
   }

   // Both maps use the same node type. Extracting and inserting the node transfers
   // node ownership without allocating after connection_manager has committed a prune.
   auto node = sessions.extract(active);
   auto transferred = retiring_sessions.insert(std::move(node));
   if (!transferred.inserted) {
      std::terminate();
   }
   auto retired = transferred.position->second;
   remove_address_observation_locked(retired->id);
   if (track_close) {
      try {
         static_cast<void>(
             retired->retirement.track(teardown.track([retired] { detail::request_session_cancel(retired->connection); })));
      } catch (...) {
         // The map owns the session before tracking is attempted, so a callback
         // allocation failure becomes a quarantined retirement rather than a split registry.
         retired->retirement.quarantine();
      }
   }
   return retired;
}

boost::asio::awaitable<void> node::impl::async_retire_session(const std::shared_ptr<session_state>& session,
                                                              bool allow_untracked) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto start = session->retirement.begin_close(allow_untracked);
   while (start == detail::session_retirement::close_start::in_flight) {
      co_await session->retirement.async_wait_not_in_flight();
      start = session->retirement.begin_close(allow_untracked);
   }
   if (start == detail::session_retirement::close_start::untracked) {
      detail::request_session_cancel(session->connection);
      session->retirement.quarantine();
      co_return;
   }
   if (start != detail::session_retirement::close_start::started) {
      co_return;
   }

   const auto failure = co_await async_close_terminal(session->connection);
   // The terminal model can retain the direct-attempt teardown ticket through
   // its lower native transport, so destroy it before releasing that ticket.
   auto transport = std::move(session->connection);
   session->connection = {};
   transport = {};

   auto teardown_ticket = detail::session_teardown::ticket{};
   session->resource.release();
   session->native_lifetime.reset();
   if (failure) {
      const auto lock = std::scoped_lock{mutex};
      if (session_admission_closed && !session_shutdown_error) { session_shutdown_error = failure; }
   }
   forget_retired_session(session);
   if (session->retirement.complete_terminal(teardown_ticket)) {
      teardown_ticket.release();
   }
}

boost::asio::awaitable<void> node::impl::async_retire_sessions_gracefully() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto retiring = std::vector<std::shared_ptr<session_state>>{};
   {
      const auto lock = std::scoped_lock{mutex};
      session_admission_closed = true;
      retiring.reserve(sessions.size() + retiring_sessions.size());
      for (const auto& [_, session] : retiring_sessions) { retiring.push_back(session); }
      while (!sessions.empty()) {
         const auto session = sessions.begin()->second;
         session->closed = true;
         retiring.push_back(retire_session_locked(session, false));
         connections.forget(session->id);
         ++metrics_value.sessions_closed;
      }
      metrics_value.active_sessions = 0;
   }
   // Keep the shared listener socket alive until every established session has
   // sent its close and joined native cleanup. Concurrent callers join the same
   // retirement owners, including closes started before this snapshot.
   for (const auto& session : retiring) {
      try {
         co_await async_retire_session(session, true);
      } catch (...) {
         const auto lock = std::scoped_lock{mutex};
         if (!session_shutdown_error) { session_shutdown_error = std::current_exception(); }
      }
   }
   const auto lock = std::scoped_lock{mutex};
   if (session_shutdown_error) { std::rethrow_exception(session_shutdown_error); }
}

void node::impl::forget_retired_session(const std::shared_ptr<session_state>& session) noexcept {
   const auto lock = std::scoped_lock{mutex};
   const auto found = retiring_sessions.find(session->id);
   if (found != retiring_sessions.end() && found->second == session) {
      retiring_sessions.erase(found);
   }
}

void node::impl::launch_pruned_session_teardown(const std::shared_ptr<session_state>& session) noexcept {
   if (!session->retirement.tracked()) {
      detail::request_session_cancel(session->connection);
      session->retirement.quarantine();
      return;
   }
   try {
      auto self = shared_from_this();
      boost::asio::co_spawn(
          runtime.context(),
          [self = std::move(self), session]() mutable -> boost::asio::awaitable<void> {
             co_await self->async_retire_session(session, false);
          },
          boost::asio::detached);
   } catch (...) {
      detail::request_session_cancel(session->connection);
      session->retirement.quarantine();
   }
}

boost::asio::awaitable<void> node::impl::remember_session(std::shared_ptr<node::impl::session_state> session,
                                                          connection_manager::direction direction,
                                                          std::function<void()> before_publish) {
   enum class rejection {
      none,
      admission,
      stopped,
   };
   enum class admission_rejection {
      none,
      canceled,
      closed,
      unexpected,
   };

   auto admission_ticket = forge::asio::gate::ticket{};
   auto admission_rejected = admission_rejection::none;
   auto admission_failure = std::exception_ptr{};
   try {
      admission_ticket = co_await session_admission_gate.acquire();
   } catch (const forge::asio::exceptions::canceled&) {
      admission_rejected = admission_rejection::canceled;
   } catch (const forge::asio::exceptions::rejected&) {
      admission_rejected = admission_rejection::closed;
   } catch (...) {
      admission_rejected = admission_rejection::unexpected;
      admission_failure = std::current_exception();
   }
   if (admission_rejected != admission_rejection::none) {
      detail::mark_rejected_session(session);
      detail::cancel_rejected_session(session);
      admission_ticket.release();
      co_await async_discard_session(session);
      if (admission_rejected == admission_rejection::canceled) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P session admission was canceled");
      }
      if (admission_rejected == admission_rejection::closed) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
      }
      std::rethrow_exception(admission_failure);
   }

   const auto resource_direction = direction == connection_manager::direction::inbound
                                       ? resource_manager::session_direction::inbound
                                       : resource_manager::session_direction::outbound;
   if (!session->resource.established()) {
      const auto transition = session->resource.establish(resource_manager::session_scope{
          .peer = session->info.remote_peer,
          .direction = resource_direction,
      });
      if (transition != resource_manager::transition_result::accepted) {
         detail::cancel_rejected_session(session);
         if (transition == resource_manager::transition_result::policy_rejected) {
            {
               auto lock = std::scoped_lock{mutex};
               ++metrics_value.backpressure_rejections;
               ++metrics_value.connection_rejections;
            }
            admission_ticket.release();
            co_await async_discard_session(session);
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P established session limit reached");
         }
         admission_ticket.release();
         co_await async_discard_session(session);
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P session resource transition failed");
      }
   }

   auto rejected = rejection::none;
   auto rejection_reason = std::string{};
   auto pruned_ids = std::vector<std::uint64_t>{};
   auto staged_session_id = std::optional<std::uint64_t>{};
   auto registry_failure = std::exception_ptr{};
   try {
      refresh_connection_scores();
      const auto network_score = [&] {
         if (const auto record = store.find(session->info.remote_peer)) {
            return record->score;
         }
         return 0.0;
      }();
      {
         auto lock = std::scoped_lock{mutex};
         if (stopped || session_admission_closed) {
            detail::mark_rejected_session(session);
            rejected = rejection::stopped;
         } else {
            if (before_publish) {
               before_publish();
            }
            const auto generated_id = session->id == 0;
            const auto assigned_id = generated_id ? next_session_id : session->id;
            staged_session_id = assigned_id;
            const auto id_is_retiring = retiring_sessions.contains(assigned_id);
            const auto [candidate, inserted] = id_is_retiring
                                                   ? std::pair{sessions.end(), false}
                                                   : sessions.emplace(assigned_id, session);
            if (!inserted) {
               detail::mark_rejected_session(session);
               ++metrics_value.backpressure_rejections;
               ++metrics_value.connection_rejections;
               rejected = rejection::admission;
               rejection_reason = id_is_retiring ? "P2P session id is still retiring" : "P2P duplicate session id";
            }
            const auto now = std::chrono::steady_clock::now();
            if (rejected == rejection::none) {
               auto admission = connections.remember(
                   connection_manager::session_record{
                       .id = assigned_id,
                       .peer = session->info.remote_peer,
                       .direction = direction,
                       .opened_at = now,
                       .last_used_at = now,
                       .network_score = network_score,
                   },
                   now);
               if (!admission.accepted) {
                  sessions.erase(candidate);
                  detail::mark_rejected_session(session);
                  ++metrics_value.backpressure_rejections;
                  ++metrics_value.connection_rejections;
                  rejected = rejection::admission;
                  rejection_reason = std::move(admission.reason);
               } else {
                  // Both registries are now updated under one mutex. The manager
                  // has already staged its allocations, so this commit only transfers nodes.
                  session->id = assigned_id;
                  session->direction = direction;
                  if (generated_id) {
                     ++next_session_id;
                  }
                  pruned_ids = std::move(admission.pruned);
                  for (const auto id : pruned_ids) {
                     auto found = sessions.find(id);
                     if (found == sessions.end()) {
                        continue;
                     }
                     found->second->closed = true;
                     const auto retired = retire_session_locked(found->second, true);
                     if (retired) {
                        invalidate_pubsub_outbound_locked(retired->info.remote_peer, retired->id);
                     }
                  }
                  ++metrics_value.sessions_opened;
                  ++metrics_value.handshakes_completed;
               }
            }
         }

         metrics_value.active_sessions = sessions.size();
         metrics_value.sessions_pruned += pruned_ids.size();
         metrics_value.sessions_closed += pruned_ids.size();
      }
   } catch (...) {
      {
         auto lock = std::scoped_lock{mutex};
         if (staged_session_id) {
            connections.forget(*staged_session_id);
            sessions.erase(*staged_session_id);
         }
      }
      detail::mark_rejected_session(session);
      detail::cancel_rejected_session(session);
      registry_failure = std::current_exception();
   }

   if (registry_failure) {
      admission_ticket.release();
      co_await async_discard_session(session);
      std::rethrow_exception(registry_failure);
   }

   if (rejected == rejection::stopped || rejected == rejection::admission) {
      detail::cancel_marked_session(session);
   }

   // identify_service owns a separate mutex; never take it while holding the
   // node mutex used by session admission and teardown.
   for (const auto id : pruned_ids) {
      identify_service.forget(id);
   }

   for (const auto id : pruned_ids) {
      auto pruned = std::shared_ptr<session_state>{};
      {
         const auto lock = std::scoped_lock{mutex};
         if (const auto found = retiring_sessions.find(id); found != retiring_sessions.end()) {
            pruned = found->second;
         }
      }
      if (pruned) {
         detail::request_session_cancel(pruned->connection);
         launch_pruned_session_teardown(pruned);
      }
   }

   if (rejected == rejection::stopped) {
      admission_ticket.release();
      co_await async_discard_session(session);
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
   }
   if (rejected == rejection::admission) {
      admission_ticket.release();
      co_await async_discard_session(session);
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                            rejection_reason.empty() ? "P2P session admission rejected" : rejection_reason);
   }

   co_return;
}

void node::impl::forget_session(const peer_id& peer) {
   auto removed_sessions = std::vector<std::shared_ptr<session_state>>{};
   {
      auto lock = std::scoped_lock{mutex};
      auto removed = std::size_t{0};
      for (auto it = sessions.begin(); it != sessions.end();) {
         if (it->second->info.remote_peer != peer) {
            ++it;
            continue;
         }
         const auto session = it->second;
         ++it;
         session->closed = true;
         if (const auto retired = retire_session_locked(session, true)) {
            removed_sessions.push_back(retired);
            connections.forget(retired->id);
         }
         ++removed;
      }
      if (removed != 0) {
         metrics_value.active_sessions = sessions.size();
         metrics_value.sessions_closed += removed;
      }
      erase_inbound_relay_reservation_locked(peer);
      invalidate_pubsub_outbound_locked(peer);
      forget_pubsub_peer_locked(peer);
   }
   for (const auto& session : removed_sessions) {
      launch_pruned_session_teardown(session);
      identify_service.forget(session->id);
   }
}

void node::impl::forget_session(const std::shared_ptr<node::impl::session_state>& session) {
   auto removed = false;
   {
      auto lock = std::scoped_lock{mutex};
      const auto found = sessions.find(session->id);
      if (found == sessions.end() || found->second != session) {
         return;
      }
      session->closed = true;
      if (!retire_session_locked(session, true)) {
         return;
      }
      removed = true;
      connections.forget(session->id);
      metrics_value.active_sessions = sessions.size();
      ++metrics_value.sessions_closed;
      const auto peer = session->info.remote_peer;
      const auto peer_still_connected = std::ranges::any_of(
          sessions, [&](const auto& item) { return item.second->info.remote_peer == peer && !item.second->closed; });
      if (!peer_still_connected) {
         erase_inbound_relay_reservation_locked(peer);
         forget_pubsub_peer_locked(peer);
      }
      invalidate_pubsub_outbound_locked(session->info.remote_peer, session->id);
   }
   if (removed) {
      launch_pruned_session_teardown(session);
      identify_service.forget(session->id);
   }
}

[[nodiscard]] std::shared_ptr<node::impl::session_state> node::impl::session_for(const peer_id& peer) const {
   auto lock = std::scoped_lock{mutex};
   return session_for_locked(peer);
}

[[nodiscard]] std::shared_ptr<node::impl::session_state> node::impl::session_for_locked(const peer_id& peer) const {
   auto selected = std::shared_ptr<session_state>{};
   for (const auto& [_, session] : sessions) {
      if (session->info.remote_peer == peer && !session->closed) {
         selected = session;
      }
   }
   if (selected) {
      connections.touch(selected->id, std::chrono::steady_clock::now());
   }
   return selected;
}

[[nodiscard]] std::shared_ptr<node::impl::session_state>
node::impl::session_for_path(const peer_id& peer, path::kind kind, std::optional<peer_id> relay_peer) const {
   auto lock = std::scoped_lock{mutex};
   return session_for_path_locked(peer, kind, relay_peer);
}

[[nodiscard]] std::shared_ptr<node::impl::session_state>
node::impl::session_for_path_locked(const peer_id& peer, path::kind kind,
                                    const std::optional<peer_id>& relay_peer) const {
   auto selected = std::shared_ptr<session_state>{};
   for (const auto& [_, session] : sessions) {
      if (session->info.remote_peer != peer || session->info.path != kind || session->closed) {
         continue;
      }
      if (relay_peer && session->info.relay_peer != relay_peer) {
         continue;
      }
      selected = session;
   }
   if (selected) {
      connections.touch(selected->id, std::chrono::steady_clock::now());
   }
   return selected;
}

node::session_info node::impl::session_info_for(const std::shared_ptr<session_state>& session) const {
   auto lock = std::scoped_lock{mutex};
   return session->info;
}

boost::asio::awaitable<detail::direct_attempt>
node::impl::connect_direct_attempt(forge::net::p2p::endpoint endpoint, node::connect_options connect_options_value,
                                   std::shared_ptr<cancellation_latch> cancellation,
                                   direct::tcp_transport_progress_handler tcp_transport_progress) {
   validate_operation_timeout(connect_options_value.timeout, "P2P connect timeout");
   require_private_direct_tcp(endpoint, "connect");
   const auto deadline_at = std::chrono::steady_clock::now() + connect_options_value.timeout;
   auto endpoint_copy = endpoint;
   auto reservation = resources.reserve_session(resource_manager::session_direction::outbound);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.backpressure_rejections;
         ++metrics_value.connection_rejections;
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P pending outbound session limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P outbound session resource admission failed");
   }
   auto descriptor = reservation->reserve_file_descriptors(1);
   if (!descriptor) {
      auto lock = std::scoped_lock{mutex};
      ++metrics_value.backpressure_rejections;
      ++metrics_value.connection_rejections;
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P outbound TCP/QUIC file descriptor limit reached");
   }
   auto staged_resources = std::make_shared<detail::direct_attempt_resources>();
   staged_resources->session = std::move(*reservation);
   staged_resources->file_descriptor = std::move(*descriptor);
   auto operation_cancellation = std::make_shared<cancellation_latch>();
   auto parent_subscription = cancellation_latch::subscribe(
       cancellation, [operation_cancellation] noexcept { operation_cancellation->request_stop(); });
   staged_resources->teardown_ticket = teardown.track(
       [operation_cancellation] noexcept { operation_cancellation->request_stop(); });
   if (!staged_resources->teardown_ticket.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped before direct attempt transport start");
   }
   try {
      auto started = std::chrono::steady_clock::now();
      auto authenticated_admission = [this, staged_resources](const peer_id& authenticated_peer) {
         const auto transition = staged_resources->session.establish(resource_manager::session_scope{
             .peer = authenticated_peer,
             .direction = resource_manager::session_direction::outbound,
         });
         if (transition != resource_manager::transition_result::accepted) {
            if (transition != resource_manager::transition_result::policy_rejected) {
               FORGE_THROW_EXCEPTION(exceptions::internal, "P2P outbound session resource transition failed");
            }
            {
               auto lock = std::scoped_lock{mutex};
               ++metrics_value.backpressure_rejections;
               ++metrics_value.connection_rejections;
            }
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                  "P2P established outbound session limit reached");
         }
      };
      auto result = co_await direct_registry.async_connect(std::move(endpoint), connect_options_value,
                                                           std::move(operation_cancellation), staged_resources,
                                                           std::move(authenticated_admission),
                                                           std::move(tcp_transport_progress));
      auto attempt = detail::direct_attempt{};
      attempt.connection = std::move(result);
      attempt.resources = std::move(staged_resources);
      attempt.target = std::move(endpoint_copy);
      attempt.started_at = started;
      co_return attempt;
   } catch (...) {
      // The unpublished connection scope ends on operation failure; the lower
      // transport retains its FD and teardown ticket until native terminal cleanup.
      staged_resources->session.release();
      try {
         throw;
      } catch (const forge::exceptions::base& error) {
         auto stopped_before_deadline = false;
         {
            auto lock = std::scoped_lock{mutex};
            stopped_before_deadline = stop_requested_at && *stop_requested_at < deadline_at;
         }
         if (p2p_code(error) == exceptions::code::timeout && stopped_before_deadline) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P direct connect stopped with its node");
         }
         FORGE_THROW_CODE(p2p_code(error), error.what());
      }
   }
}

boost::asio::awaitable<void> node::impl::async_close_direct_attempt(detail::direct_attempt& attempt) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto transport = std::move(attempt.connection.session);
   attempt.connection.session = {};
   co_await async_close_terminal(transport);
   transport = {};
   attempt.connection.native_lifetime.reset();
   attempt.resources.reset();
}

boost::asio::awaitable<void> node::impl::async_discard_session(const std::shared_ptr<session_state>& session) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto transport = std::move(session->connection);
   session->connection = {};
   co_await async_close_terminal(transport);
   transport = {};
   session->resource.release();
   session->native_lifetime.reset();
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::commit_direct_attempt(detail::direct_attempt attempt,
                                   std::vector<forge::multiformats::multiaddr> roots,
                                   std::chrono::steady_clock::time_point deadline_at,
                                   std::shared_ptr<cancellation_latch> cancellation) {
   auto session = std::shared_ptr<session_state>{};
   auto commit_failure = std::exception_ptr{};
   try {
      if (!attempt.resources || !attempt.resources->session.active()) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P direct attempt lost its resource state");
      }
      const auto peer = attempt.connection.peer;
      session = std::make_shared<session_state>();
      session->info = node::session_info{
          .remote_peer = peer,
          .path = path::kind::direct,
      };
      session->authentication = attempt.connection.authentication;
      session->direct_endpoint = attempt.target;
      session->direct_roots = std::move(roots);
      session->local_endpoint = attempt.connection.local_endpoint;
      session->remote_endpoint = attempt.connection.remote_endpoint;
      session->native_lifetime = attempt.resources;
      session->resource = std::move(attempt.resources->session);
      session->connection = std::move(attempt.connection.session);
      attempt.resources.reset();
   } catch (...) {
      commit_failure = std::current_exception();
   }
   if (commit_failure) {
      co_await async_close_direct_attempt(attempt);
      std::rethrow_exception(commit_failure);
   }

   auto admission_deadline = std::optional<operation_deadline>{};
   auto admission_cancellation = cancellation_latch::subscription{};
   auto published = false;
   try {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          deadline_at - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
         FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P dial deadline expired before publication");
      }
      admission_deadline.emplace(runtime.context(), remaining);
      auto bridge = std::make_shared<detail::worker_stop_bridge>();
      admission_cancellation = cancellation_latch::subscribe(
          cancellation, [stop = admission_deadline->stopping()] noexcept {
             static_cast<void>(stop.request_stop());
          });
      admission_deadline->arm([bridge] noexcept { bridge->request_stop(); });
      co_await detail::async_run_with_owner_cancellation(
          bridge, [this, session, &admission_deadline, &published, deadline_at, cancellation](
                      boost::asio::cancellation_slot) -> boost::asio::awaitable<void> {
             co_await remember_session(session, connection_manager::direction::outbound,
                 [&admission_deadline, deadline_at, cancellation] {
                    if (cancellation && cancellation->stop_requested()) {
                       FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P dial canceled before publication");
                    }
                    if (std::chrono::steady_clock::now() >= deadline_at || admission_deadline->timed_out()) {
                       FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P dial deadline expired before publication");
                    }
                    if (!admission_deadline->finish() || admission_deadline->stopped()) {
                       FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P dial stopped before publication");
                    }
                 });
             published = true;
          });
      if (!published) {
         if (admission_deadline->timed_out() || std::chrono::steady_clock::now() >= deadline_at) {
            FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P dial deadline expired during admission");
         }
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P session admission did not publish a winner");
      }
      if (!launch_session_accept_loop(session)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P session accept loop could not start");
      }
      for (const auto& root : session->direct_roots) {
         store.mark_address_success(
             session->info.remote_peer, root, path::kind::direct,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - attempt.started_at));
      }
      launch_identify(session);
      co_await announce_pubsub_subscriptions(session->info.remote_peer);
   } catch (...) {
      commit_failure = std::current_exception();
   }
   if (commit_failure) {
      forget_session(session);
      co_await async_retire_session(session, true);
      if (!published && admission_deadline && admission_deadline->timed_out()) {
         FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P dial deadline expired during admission");
      }
      if (!published && cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P dial canceled during admission");
      }
      std::rethrow_exception(commit_failure);
   }
   co_return session;
}

void node::impl::launch_accept_loop(forge::net::p2p::endpoint local_endpoint) {
   auto self = shared_from_this();
   static_cast<void>(launch_tracked([self, local_endpoint = std::move(local_endpoint)]() -> asio::awaitable<void> {
      while (true) {
         {
            auto lock = std::scoped_lock{self->mutex};
            if (self->stopped || !listener_is_active(self->direct_registry, local_endpoint)) {
               co_return;
            }
         }
         try {
            auto connection = co_await self->direct_registry.async_accept(local_endpoint);
            if (!connection.admission || !connection.admission->active()) {
               co_await direct::async_discard_unpublished(connection);
               {
                  auto lock = std::scoped_lock{self->mutex};
                  ++self->metrics_value.connection_rejections;
               }
               continue;
            }
            auto stopped = false;
            {
               auto lock = std::scoped_lock{self->mutex};
               stopped = self->stopped;
            }
            if (stopped) {
               co_await direct::async_discard_unpublished(connection);
               co_return;
            }
            auto accepted = std::make_shared<direct::connection>(std::move(connection));
            auto admission = std::make_shared<resource_manager::session_reservation>(std::move(*accepted->admission));
            accepted->admission.reset();
            if (!self->launch_tracked_cleanup([self, accepted, admission]() mutable -> asio::awaitable<void> {
                   co_await self->handle_inbound_connection(std::move(*accepted), std::move(*admission));
                })) {
               accepted->admission.emplace(std::move(*admission));
               co_await direct::async_discard_unpublished(*accepted);
            }
         } catch (const forge::exceptions::base& error) {
            auto lock = std::scoped_lock{self->mutex};
            if (self->stopped || !listener_is_active(self->direct_registry, local_endpoint)) {
               co_return;
            }
            const auto kind = p2p_code(error);
            if (kind == exceptions::code::connection_rejected || kind == exceptions::code::backpressure_rejected) {
               continue;
            }
            ++self->metrics_value.handshakes_failed;
         } catch (const std::exception&) {
            auto lock = std::scoped_lock{self->mutex};
            if (self->stopped || !listener_is_active(self->direct_registry, local_endpoint)) {
               co_return;
            }
            ++self->metrics_value.handshakes_failed;
         } catch (...) {
            auto lock = std::scoped_lock{self->mutex};
            if (self->stopped || !listener_is_active(self->direct_registry, local_endpoint)) {
               co_return;
            }
            ++self->metrics_value.handshakes_failed;
         }
      }
   }));
}

boost::asio::awaitable<void> node::impl::handle_inbound_connection(direct::connection connection,
                                                                   resource_manager::session_reservation reservation) {
   enum class failure_kind {
      none,
      typed,
      untyped,
   };

   auto failure = failure_kind::none;
   auto typed_failure = std::optional<exceptions::code>{};
   try {
      auto node_stopped = false;
      {
         auto lock = std::scoped_lock{mutex};
         node_stopped = stopped;
      }
      if (node_stopped) {
         connection.admission.emplace(std::move(reservation));
         co_await direct::async_discard_unpublished(connection);
         co_return;
      }
      auto remote = connection.peer;
      auto session = std::make_shared<session_state>();
      session->info = node::session_info{
          .remote_peer = remote,
          .path = path::kind::direct,
      };
      session->authentication = connection.authentication;
      session->direct_endpoint = connection.local_endpoint;
      session->local_endpoint = connection.local_endpoint;
      session->remote_endpoint = connection.remote_endpoint;

      // Complete all copying before transferring terminal transport ownership.
      session->connection = std::move(connection.session);
      session->resource = std::move(reservation);
      session->native_lifetime = std::move(connection.native_lifetime);
      co_await remember_session(session, connection_manager::direction::inbound);
      launch_session_accept_loop(session);
      launch_identify(session);
      co_await announce_pubsub_subscriptions(remote);
      co_return;
   } catch (const forge::exceptions::base& error) {
      typed_failure = p2p_code(error);
      failure = failure_kind::typed;
   } catch (const std::exception&) {
      failure = failure_kind::untyped;
   } catch (...) {
      failure = failure_kind::untyped;
   }

   if (reservation.active()) {
      connection.admission.emplace(std::move(reservation));
   }
   co_await direct::async_discard_unpublished(connection);

   auto lock = std::scoped_lock{mutex};
   if (failure == failure_kind::typed && typed_failure &&
       detail::suppress_inbound_handshake_failure(*typed_failure, stopped)) {
      co_return;
   }
   if (failure != failure_kind::none) {
      // The listener owns detached accepts; failed handshakes are reflected in metrics.
      ++metrics_value.handshakes_failed;
   }
   co_return;
}

bool node::impl::launch_session_accept_loop(std::shared_ptr<node::impl::session_state> session) {
   auto self = shared_from_this();
   return launch_tracked([self, session = std::move(session)]() mutable -> asio::awaitable<void> {
      while (true) {
         {
            auto lock = std::scoped_lock{self->mutex};
            if (self->stopped || session->closed) {
               co_return;
            }
         }
         try {
            auto stream = co_await session->connection.async_accept_stream();
            auto reservation =
                self->resources.reserve_stream(session->info.remote_peer, resource_manager::session_direction::inbound);
            if (!reservation) {
               stream.cancel();
               if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
                  auto lock = std::scoped_lock{self->mutex};
                  ++self->metrics_value.backpressure_rejections;
                  ++self->metrics_value.protocol_rejections;
                  continue;
               }
               FORGE_THROW_EXCEPTION(exceptions::internal, "P2P inbound stream resource admission failed");
            }
            auto accepted = std::make_shared<forge::net::transport::stream>(std::move(stream));
            auto admission = std::make_shared<resource_manager::stream_reservation>(std::move(*reservation));
            if (!self->launch_tracked([self, session, accepted, admission]() mutable -> asio::awaitable<void> {
                   if (session->info.path == path::kind::relay) {
                      co_await self->handle_relayed_yamux_stream(session, std::move(*accepted), std::move(*admission));
                   } else {
                      co_await self->handle_incoming_stream(session, std::move(*accepted), std::move(*admission));
                   }
                })) {
               accepted->cancel();
            }
         } catch (...) {
            self->forget_session(session);
            co_return;
         }
      }
   });
}

boost::asio::awaitable<void> node::impl::handle_incoming_stream(std::shared_ptr<node::impl::session_state> session,
                                                                forge::net::transport::stream raw,
                                                                resource_manager::stream_reservation reservation) {
   try {
      auto admitted =
          co_await accept_resource_stream(session->info.remote_peer, std::move(raw), std::move(reservation));
      detail::stream_access::set_authentication(admitted.stream, session->authentication);
      if (admitted.protocol == builtins::ping) {
         co_await handle_ping(std::move(admitted.stream));
      } else if (admitted.protocol == builtins::identify) {
         co_await handle_identify(session, std::move(admitted.stream));
      } else if (admitted.protocol == builtins::identify_push) {
         co_await handle_identify_push(session, std::move(admitted.stream), std::move(admitted.resource));
      } else if (admitted.protocol == builtins::peer_exchange) {
         auto request = co_await peer_exchange_codec::async_read(admitted.stream, codec_for(options));
         if (request.kind != peer_exchange_message::type::peer_exchange_request) {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P peer exchange expected request");
         }
         co_await handle_peer_exchange(std::move(admitted.stream), request.request_id, request.max_frame_size);
      } else if (admitted.protocol == builtins::autonat_v2_dial_request) {
         co_await handle_autonat_v2_dial_request(session, std::move(admitted.stream));
      } else if (admitted.protocol == builtins::autonat_v2_dial_back) {
         co_await handle_autonat_v2_dial_back(session, std::move(admitted.stream));
      } else if (admitted.protocol == builtins::autonat_v1) {
         co_await handle_autonat_v1(session, std::move(admitted.stream));
      } else if (admitted.protocol == builtins::relay_hop) {
         co_await handle_relay_hop(session, std::move(admitted.stream));
      } else if (admitted.protocol == builtins::relay_stop) {
         co_await handle_relay_stop(session, std::move(admitted.stream));
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
            FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "unsupported negotiated P2P protocol");
         }
         increment_protocol_accepted();
         co_await (*handler)(node::incoming_protocol_stream{
             .session = session_info_for(session),
             .protocol = admitted.protocol,
             .stream = std::move(admitted.stream),
         });
      }
      co_await detail::async_close_unescaped(admitted.resource);
   } catch (const std::exception&) {
      increment_protocol_rejected();
   } catch (...) {
      increment_protocol_rejected();
   }
}

} // namespace forge::net::p2p
