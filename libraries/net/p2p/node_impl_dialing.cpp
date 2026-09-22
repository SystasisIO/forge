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
#include <stop_token>

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
import forge.net.dns.resolver;
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
#include "details/dial_scheduler.hxx"
#include "details/cancellation_latch.hxx"
#include "details/node_impl.hxx"
#include "details/path_selector.hxx"
#include "details/peer_exchange_codec.hxx"
#include "details/peer_failure.hxx"
#include "details/resource_stream.hxx"
#include "details/session_lifecycle.hxx"
#include "details/session_retirement.hxx"

namespace forge::net::p2p {

namespace {

[[nodiscard]] bool persistent_root(const detail::source_root& root,
                                   std::span<const detail::direct_dial_provenance> input_provenance) {
   if (root.input_indices.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P direct dial root has no input provenance");
   }
   auto persistent = false;
   for (const auto index : root.input_indices) {
      if (index >= input_provenance.size()) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P direct dial root provenance index is out of bounds");
      }
      persistent = persistent || input_provenance[index] == detail::direct_dial_provenance::persistent;
   }
   return persistent;
}

} // namespace

boost::asio::awaitable<node::session_info>
node::impl::async_connect_owned(std::shared_ptr<impl> self, forge::multiformats::multiaddr address,
                                node::connect_options value) {
   for (const auto& component : address.components()) {
      if (component.code == forge::multiformats::protocol_code::ws ||
          component.code == forge::multiformats::protocol_code::wss ||
          component.code == forge::multiformats::protocol_code::p2p_circuit) {
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol,
                               "P2P direct connect requires a TCP or QUIC address");
      }
   }
   if (self->private_network_enabled()) {
      if (value.relay_peer) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P private-network connect does not permit a relay peer");
      }
      value.allow_relay = false;
      value.allow_hole_punch = false;
   }
   auto roots = std::vector<forge::multiformats::multiaddr>{std::move(address)};
   auto session = co_await self->connect_direct(std::move(roots), std::move(value));
   co_await self->identify_session(session);
   co_return self->session_info_for(session);
}

void node::impl::record_direct_session_failure(const std::shared_ptr<session_state>& session) {
   // A cached DNS child is not evidence that its source root has exhausted all
   // addresses. Only the scheduler can attribute a fresh resolved root failure.
   if (session->direct_endpoint) {
      const auto concrete = session->direct_endpoint->to_multiaddr();
      for (const auto& root : session->direct_roots) {
         if (root.to_string() == concrete.to_string()) {
            store.mark_address_failure(session->info.remote_peer, root, path::kind::direct,
                endpoint_backoff_until(session->info.remote_peer, root, path::kind::direct));
         }
      }
   }
   increment_direct_failure();
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::connect_direct(forge::net::p2p::endpoint endpoint, node::connect_options value,
                           std::shared_ptr<cancellation_latch> cancellation) {
   auto roots = std::vector<forge::multiformats::multiaddr>{endpoint.to_multiaddr()};
   return connect_direct(std::move(roots), std::move(value), std::move(cancellation));
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::connect_direct(std::vector<forge::multiformats::multiaddr> roots, node::connect_options value,
                           std::shared_ptr<cancellation_latch> cancellation) {
   auto direct_roots = std::vector<detail::direct_dial_root>{};
   direct_roots.reserve(roots.size());
   for (auto& root : roots) {
      direct_roots.push_back({.address = std::move(root), .provenance = detail::direct_dial_provenance::persistent});
   }
   return connect_direct(std::move(direct_roots), std::move(value), std::move(cancellation));
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::connect_direct(std::vector<detail::direct_dial_root> roots, node::connect_options value,
                           std::shared_ptr<cancellation_latch> cancellation) {
   auto addresses = std::vector<forge::multiformats::multiaddr>{};
   auto input_provenance = std::vector<detail::direct_dial_provenance>{};
   addresses.reserve(roots.size());
   input_provenance.reserve(roots.size());
   for (auto& root : roots) {
      addresses.push_back(std::move(root.address));
      input_provenance.push_back(root.provenance);
   }
   auto self = shared_from_this();
   validate_operation_timeout(value.timeout, "P2P connect timeout");
   validate_operation_timeout(value.direct_attempt_timeout, "P2P direct attempt timeout");
   auto operation = lifecycle.track();
   if (!operation.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped before logical dial");
   }
   const auto deadline = std::chrono::steady_clock::now() + value.timeout;
   auto stop = std::stop_source{};
   auto commit_stop = std::make_shared<cancellation_latch>();
   auto parent = cancellation_latch::subscribe(cancellation, [stop, commit_stop]() mutable noexcept {
      stop.request_stop();
      commit_stop->request_stop();
   });
   auto ticket = teardown.track([stop, commit_stop]() mutable noexcept {
      stop.request_stop();
      commit_stop->request_stop();
   });
   if (!ticket.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped before logical dial admission");
   }
   auto dial = resources.reserve_dial();
   if (!dial) {
      if (dial.outcome() == resource_manager::transition_result::policy_rejected) {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.backpressure_rejections;
         ++metrics_value.connection_rejections;
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P logical dial limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P logical dial resource admission failed");
   }
   auto operation_peer = value.expected_peer;
   const auto bind_peer = [&](const peer_id& peer) {
      const auto transition = dial->bind(peer);
      if (transition != resource_manager::transition_result::accepted) {
         if (transition == resource_manager::transition_result::policy_rejected) {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.backpressure_rejections;
            ++metrics_value.connection_rejections;
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P per-peer dial limit reached");
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P logical dial resource transition failed");
      }
   };
   auto terminal_roots = std::vector<detail::dial_root_outcome>{};
   auto callbacks = detail::dial_scheduler::operation_callbacks{
       .start_attempt = [self, value](endpoint target, std::optional<peer_id> expected,
                                    std::chrono::steady_clock::time_point attempt_deadline,
                                    std::shared_ptr<cancellation_latch> child,
                                    direct::tcp_transport_progress_handler progress)
           -> boost::asio::awaitable<detail::direct_attempt> {
          if (!expected && target.peer) {
             // A suffixless DNSADDR root may resolve to different peers. Gate
             // the candidate identity without binding the whole logical dial.
             self->connection_gate->peer_dial(*target.peer);
             expected = target.peer;
          }
          if (expected) {
             self->connection_gate->address_dial(*expected, target);
          }
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
              attempt_deadline - std::chrono::steady_clock::now());
          if (remaining <= std::chrono::milliseconds::zero()) {
             FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P direct attempt deadline expired");
          }
          auto attempt_options = value;
          attempt_options.expected_peer = std::move(expected);
          attempt_options.timeout = remaining;
          self->record_path_attempt(path::kind::direct);
          co_return co_await self->connect_direct_attempt(
              std::move(target), std::move(attempt_options), std::move(child), std::move(progress));
       },
       .discard_attempt = [self](detail::direct_attempt& attempt) -> boost::asio::awaitable<void> {
          co_await self->async_close_direct_attempt(attempt);
       },
       .is_owner_stopping = [self]() noexcept {
          const auto lock = std::scoped_lock{self->mutex};
          return self->stopped || self->session_admission_closed;
       },
       .observe_terminal_root_outcomes = [&terminal_roots](std::vector<detail::dial_root_outcome> outcomes) noexcept {
          terminal_roots.swap(outcomes);
       },
       .prepare_peer = [&](const std::optional<peer_id>& peer) {
          operation_peer = peer;
          if (peer) {
             connection_gate->peer_dial(*peer);
             bind_peer(*peer);
          }
       },
   };
   auto result = std::optional<detail::dial_result>{};
   auto failure = std::exception_ptr{};
   try {
      result.emplace(co_await dial_scheduler->async_dial(
          detail::dial_scheduler::request{
              .roots = std::move(addresses),
              .expected_peer = value.expected_peer,
              .logical_deadline = deadline,
              .attempt_timeout = value.direct_attempt_timeout,
              .stop = stop.get_token(),
              .max_attempts = value.max_direct_endpoints,
              .tcp_only = private_network_enabled(),
          },
          std::move(callbacks)));
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      if (operation_peer) {
         for (const auto& outcome : terminal_roots) {
            if (outcome.outcome == dialing::outcome::failure) {
               if (persistent_root(outcome.root, input_provenance)) {
                  store.mark_address_failure(*operation_peer, outcome.root.canonical, path::kind::direct,
                      endpoint_backoff_until(*operation_peer, outcome.root.canonical, path::kind::direct));
               }
               increment_direct_failure();
            }
         }
      }
      try {
         std::rethrow_exception(failure);
      } catch (const forge::exceptions::base& error) {
         auto owner_closed = false;
         {
            const auto lock = std::scoped_lock{mutex};
            owner_closed = stopped || session_admission_closed;
         }
         if (owner_closed && exceptions::code_of(error) == exceptions::code::closed) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P admitted logical dial canceled by node shutdown");
         }
         throw;
      }
   }

   // Materialize provenance and finish peer admission before handing off the
   // unpublished winner. Any failure still owns its native close barrier.
   auto winner_roots = std::vector<forge::multiformats::multiaddr>{};
   try {
      if (stop.stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P logical dial canceled before publication");
      }
      if (!dial->bound()) {
         bind_peer(result->attempt.connection.peer);
      }
      winner_roots.reserve(result->winner_roots.size());
      for (auto& root : result->winner_roots) {
         if (persistent_root(root, input_provenance)) {
            winner_roots.push_back(std::move(root.canonical));
         }
      }
      for (const auto& outcome : result->root_outcomes) {
         if (outcome.outcome == dialing::outcome::failure) {
            if (persistent_root(outcome.root, input_provenance)) {
               store.mark_address_failure(result->attempt.connection.peer, outcome.root.canonical, path::kind::direct,
                   endpoint_backoff_until(result->attempt.connection.peer, outcome.root.canonical, path::kind::direct));
            }
            increment_direct_failure();
         }
      }
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      co_await async_close_direct_attempt(result->attempt);
      std::rethrow_exception(failure);
   }
   co_return co_await commit_direct_attempt(
       std::move(result->attempt), std::move(winner_roots), deadline, std::move(commit_stop));
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::ensure_direct_session(const peer_id& peer, std::chrono::milliseconds timeout,
                                  std::size_t max_direct_endpoints, std::chrono::milliseconds direct_attempt_timeout,
                                  std::shared_ptr<cancellation_latch> cancellation) {
   if (cancellation && cancellation->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P direct session acquisition canceled");
   }
   if (auto existing = session_for_path(peer, path::kind::direct)) {
      co_return existing;
   }
   const auto record = store.find(peer);
   if (!record) {
      FORGE_THROW_EXCEPTION(exceptions::peer_not_found, "P2P peer has no known direct endpoint");
   }
   auto preferred = path_selector::rank_direct(*record, std::chrono::system_clock::now());
   auto roots = std::vector<forge::multiformats::multiaddr>{};
   roots.reserve(preferred.size());
   for (auto& candidate : preferred) {
      roots.push_back(std::move(candidate.address));
   }
   if (roots.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::peer_not_found, "P2P peer has no known direct endpoint");
   }
   co_return co_await connect_direct(
       std::move(roots),
       node::connect_options{.expected_peer = peer, .allow_relay = false, .timeout = timeout,
                             .direct_attempt_timeout = direct_attempt_timeout,
                             .max_direct_endpoints = max_direct_endpoints},
       std::move(cancellation));
}

} // namespace forge::net::p2p
