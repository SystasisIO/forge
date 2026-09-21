module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.crypto.core.random;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/autonat_exchange.hxx"
#include "details/cancellation_latch.hxx"
#include "details/node_impl.hxx"
#include "details/host_event_source.hxx"
#include "details/observed_address_manager.hxx"
#include "details/owner_cancellation.hxx"
#include "details/reachability_manager.hxx"

namespace forge::net::p2p {
namespace {

bool same_addresses(const std::vector<endpoint>& left, const std::vector<endpoint>& right) {
   return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
      return a.to_multiaddr().to_bytes() == b.to_multiaddr().to_bytes();
   });
}

bool supports_reachability(const auto& session, const protocol_id& protocol) {
   return std::ranges::find(session.remote_protocols, protocol) != session.remote_protocols.end();
}

bool eligible_reachability_session(const auto& session) {
   return !session.closed && session.authentication != peer_authentication::unverified &&
       session.info.path == path::kind::direct && session.remote_endpoint &&
       session.info.identify_state == identify::state::identified;
}

unsigned reachability_session_rank(const auto& session, const reachability_policy& policy, bool allowed) {
   if (!eligible_reachability_session(session)) { return 0; }
   // Prefer enabled AutoNAT capabilities over ping-only sessions; ties keep the older control connection.
   return (allowed && policy.client_v2_enabled && supports_reachability(session, builtins::autonat_v2_dial_request) ? 4U : 0U) +
       (allowed && policy.client_v1_enabled && supports_reachability(session, builtins::autonat_v1) ? 2U : 0U) +
       (policy.ping_enabled && supports_reachability(session, builtins::ping) ? 1U : 0U);
}

boost::asio::awaitable<detail::reachability_manager::probe_result> exchange_owned(
    auto self, detail::reachability_manager::observer observer, bool v2, std::vector<endpoint> candidates,
    std::shared_ptr<cancellation_latch> cancellation) {
   // The Internet-wide v1 verdict is meaningful only for an entirely public candidate set.
   const auto internet_scope = !candidates.empty() && std::ranges::all_of(candidates, [](const auto& candidate) {
      return host_addresses::classify_endpoint_scope(candidate) == host_addresses::endpoint_scope::public_address;
   });
   auto result = co_await self->exchange_reachability_owned(self, observer.peer, observer.remote, v2,
       std::move(candidates), std::move(cancellation), observer.session_id);
   const auto verified = v2 && result.value == reachability::state::publicly_reachable;
   co_return detail::reachability_manager::probe_result{std::move(result), verified, internet_scope};
}

} // namespace

std::shared_ptr<node::impl::session_state> node::impl::reachability_session_locked(
    const peer_id& peer, std::optional<protocol_id> protocol) const {
   const auto allowed = !private_network_enabled() ||
       options.private_network->internet_egress == private_network::internet_egress_policy::allow_internet;
   auto selected = std::shared_ptr<session_state>{};
   auto best = 0U;
   for (const auto& [id, session] : sessions) {
      if (id != session->id || session->info.remote_peer != peer || !eligible_reachability_session(*session)) { continue; }
      const auto rank = protocol ? (supports_reachability(*session, *protocol) ? 1U : 0U)
                                : reachability_session_rank(*session, options.reachability_policy, allowed);
      if (rank == 0 || (!protocol && rank < 2)) { continue; }
      if (!selected || rank > best || (rank == best && id < selected->id)) {
         selected = session;
         best = rank;
      }
   }
   return selected;
}

std::vector<std::shared_ptr<node::impl::session_state>> node::impl::reachability_sessions_locked() const {
   const auto allowed = !private_network_enabled() ||
       options.private_network->internet_egress == private_network::internet_egress_policy::allow_internet;
   auto selected = std::map<peer_id, std::shared_ptr<session_state>>{};
   for (const auto& [id, session] : sessions) {
      const auto rank = reachability_session_rank(*session, options.reachability_policy, allowed);
      if (id != session->id || rank == 0) { continue; }
      auto found = selected.find(session->info.remote_peer);
      if (found == selected.end()) {
         if (selected.size() < options.reachability_policy.max_observers) {
            selected.emplace(session->info.remote_peer, session);
         }
         continue;
      }
      // Never combine capabilities from different connections of the same peer.
      const auto previous_rank = reachability_session_rank(*found->second, options.reachability_policy, allowed);
      if (rank > previous_rank || (rank == previous_rank && id < found->second->id)) { found->second = session; }
   }
   auto result = std::vector<std::shared_ptr<session_state>>{};
   result.reserve(selected.size());
   for (auto& [_, session] : selected) { result.push_back(std::move(session)); }
   return result;
}

void node::impl::initialize_reachability() {
   const auto& policy = options.reachability_policy;
   observed_addresses = std::make_shared<detail::observed_address_manager>(detail::observed_address_manager::options{
       .max_observations = policy.max_observations, .max_candidates = policy.max_candidates,
       .min_observers = policy.min_observers, .max_confirmed_per_local = policy.max_confirmed_per_local,
       .ttl = policy.observation_ttl});
   host_event_source = std::make_shared<detail::host_event_source>(policy.max_event_subscribers);
   const auto weak = weak_from_this();
   reachability_manager_value = std::make_shared<detail::reachability_manager>(runtime.context().get_executor(), policy,
       detail::reachability_manager::callbacks{
           .observers = [weak] {
              auto result = std::vector<detail::reachability_manager::observer>{};
              const auto self = weak.lock();
              if (!self) { return result; }
              const auto lock = std::scoped_lock{self->mutex};
              const auto autonat_allowed = !self->private_network_enabled() ||
                  self->options.private_network->internet_egress == private_network::internet_egress_policy::allow_internet;
              for (const auto& session : self->reachability_sessions_locked()) {
                 const auto supports = [&](const auto& protocol) {
                    return std::ranges::find(session->remote_protocols, protocol) != session->remote_protocols.end();
                 };
                 result.push_back({session->info.remote_peer, *session->remote_endpoint,
                     autonat_allowed && supports(builtins::autonat_v1),
                     autonat_allowed && supports(builtins::autonat_v2_dial_request), supports(builtins::ping), session->id});
              }
              return result;
           },
           .candidates = [weak] {
              const auto self = weak.lock();
              if (!self) { return detail::reachability_state::candidate_snapshot{}; }
              self->publish_host_state({});
              const auto lock = std::scoped_lock{self->mutex};
              return self->reachability_manager_value->candidates();
           },
           .exchange = [weak](auto observer, bool v2, auto candidates, auto cancellation) {
              const auto self = weak.lock();
              if (!self) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability owner is closed"); }
              return exchange_owned(self, std::move(observer), v2, std::move(candidates), std::move(cancellation));
           },
           .ping = [weak](peer_id peer, std::shared_ptr<cancellation_latch> cancellation) {
              const auto self = weak.lock();
              if (!self) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability owner is closed"); }
              return ping_reachability_owned(self, std::move(peer), std::move(cancellation));
           },
           .changed = [weak](host_event state) {
              if (const auto self = weak.lock()) { self->publish_host_state(std::move(state)); }
           },
           .finished = [weak]() noexcept {
              if (const auto self = weak.lock()) { self->finish_reachability(); }
           },
       });
   const auto lock = std::scoped_lock{mutex};
   sync_reachability_addresses_locked();
}

void node::impl::start_reachability() {
   try {
      reachability_manager_value->start(lifecycle);
      {
         const auto lock = std::scoped_lock{mutex};
         if (stopped || session_admission_closed || reachability_finished) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability stopped during startup");
         }
         reachability_started = true;
         refresh_reachability_locked();
         if (reachability_finished) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability stopped during startup refresh");
         }
      }
      publish_host_state({});
      notify_reachability_changed();
   } catch (...) {
      finish_reachability();
      reachability_manager_value->request_stop();
      throw;
   }
}

void node::impl::stop_reachability() noexcept {
   finish_reachability();
   if (reachability_manager_value) { reachability_manager_value->request_stop(); }
   if (host_event_source) { host_event_source->close(); }
}

void node::impl::finish_reachability() noexcept {
   try {
      {
         const auto lock = std::scoped_lock{mutex};
         close_reachability_results_locked();
         refresh_reachability_locked();
      }
      publish_host_state({});
   } catch (...) {
      const auto lock = std::scoped_lock{mutex};
      invalidate_reachability_locked();
   }
}

void node::impl::close_reachability_results_locked() noexcept {
   reachability_started = false;
   reachability_finished = true;
   reachability_identify_dirty |= !confirmed_observed_addresses.empty();
   confirmed_observed_addresses.clear();
   if (reachability_manager_value) { reachability_manager_value->close_results(); }
}

boost::asio::awaitable<void> node::impl::join_reachability() {
   if (reachability_manager_value) { co_await reachability_manager_value->async_join(); }
}

void node::impl::observe_address(const std::shared_ptr<session_state>& session, const identify::document& document) {
   if (!observed_addresses || !document.observed_endpoint) { return; }
   {
      const auto lock = std::scoped_lock{mutex};
      const auto found = sessions.find(session->id);
      if (found == sessions.end() || found->second != session || session->closed ||
          session->authentication == peer_authentication::unverified || session->info.path != path::kind::direct ||
          !session->local_endpoint || !session->remote_endpoint) { return; }
      static_cast<void>(observed_addresses->observe(session->id, session->info.remote_peer,
          *session->local_endpoint, *session->remote_endpoint, *document.observed_endpoint,
          direct_registry.local_endpoints(), std::chrono::steady_clock::now()));
      refresh_reachability_locked();
   }
   publish_host_state(reachability_manager_value->current());
   notify_reachability_changed();
}

void node::impl::remove_address_observation_locked(std::uint64_t session) noexcept {
   try {
      if (observed_addresses) { observed_addresses->remove(session); }
      refresh_reachability_locked();
   } catch (...) {
      invalidate_reachability_locked();
   }
   notify_reachability_changed();
}

void node::impl::invalidate_reachability_locked() noexcept {
   reachability_identify_dirty |= !confirmed_observed_addresses.empty();
   confirmed_observed_addresses.clear();
   if (reachability_manager_value) { reachability_manager_value->invalidate_addresses(); }
   // Allocation failure cannot leave queued confirmations looking current.
   if (host_event_source) { host_event_source->close(); }
}

void node::impl::sync_reachability_addresses_locked() {
   if (!reachability_manager_value || reachability_finished || stopped || session_admission_closed) { return; }
   try {
      auto values = local_endpoints_for_control_locked();
      std::erase_if(values, [this](const auto& value) {
         const auto scope = host_addresses::classify_endpoint_scope(value);
         return (!value.is_direct_tcp() && !value.is_direct_quic()) || value.transport.port == 0 ||
                scope == host_addresses::endpoint_scope::unroutable || scope == host_addresses::endpoint_scope::dns ||
                scope == host_addresses::endpoint_scope::loopback || scope == host_addresses::endpoint_scope::link_local ||
                (private_network_enabled() && !value.is_direct_tcp());
      });
      if (values.size() > options.reachability_policy.max_candidates) {
         values.resize(options.reachability_policy.max_candidates);
      }
      try {
         static_cast<void>(reachability_manager_value->set_addresses(values));
      } catch (const exceptions::closed&) {
         // Stop closes state before the drained manager can run its finished
         // callback. A refresh already in flight must observe that terminal
         // transition, not turn normal shutdown into a parent failure.
         close_reachability_results_locked();
      }
   } catch (...) {
      invalidate_reachability_locked();
      throw;
   }
}

void node::impl::refresh_reachability_locked() {
   if (!observed_addresses || !reachability_manager_value || !host_event_source) { return; }
   try {
      // Retain bounded pre-start evidence, but do not advertise it. Re-evaluate
      // TTL at activation and every refresh before obtaining the NAT snapshot.
      auto confirmed = reachability_started && !reachability_finished && !stopped && !session_admission_closed
          ? observed_addresses->confirmed(std::chrono::steady_clock::now()) : std::vector<endpoint>{};
      if (!same_addresses(confirmed_observed_addresses, confirmed)) {
         confirmed_observed_addresses = std::move(confirmed);
         reachability_identify_dirty = true;
      }
      sync_reachability_addresses_locked();
      auto state = reachability_manager_value->current();
      state.phase = lifecycle.phase();
      state.confirmed_addresses = confirmed_observed_addresses;
      const auto previous = host_event_source->current();
      const auto unchanged = previous.phase == state.phase && previous.effective == state.effective &&
          previous.autonat_v1 == state.autonat_v1 &&
          same_addresses(previous.confirmed_addresses, state.confirmed_addresses) &&
          std::ranges::equal(previous.autonat_v2, state.autonat_v2, [](const auto& left, const auto& right) {
             return left.value == right.value && left.address.to_string() == right.address.to_string();
          });
      if (!unchanged) { host_event_source->publish(std::move(state)); }
   } catch (...) {
      invalidate_reachability_locked();
      throw;
   }
}

void node::impl::notify_reachability_changed() noexcept {
   if (reachability_manager_value) { reachability_manager_value->notify_addresses_changed(); }
}

void node::impl::publish_host_state(host_event) {
   auto push = false;
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped || session_admission_closed) { return; }
      refresh_reachability_locked();
      if (reachability_identify_dirty) {
         reachability_identify_dirty = false;
         push = advance_identify_generation_locked() && schedule_identify_push_locked();
      }
   }
   if (push) { launch_identify_pushes(); }
}

host_event node::impl::current_host_state() const {
   // The generation belongs to this complete published value, including TTL
   // transitions. Never combine it with newer, not-yet-published manager state.
   return host_event_source->current();
}
forge::net::p2p::diagnostics::reachability_state node::impl::reachability_diagnostics() const {
   const auto stats = reachability_manager_value->stats();
   const auto host = current_host_state();
   const auto& policy = options.reachability_policy;
   const auto lock = std::scoped_lock{mutex};
   const auto allowed = !private_network_enabled() ||
       options.private_network->internet_egress == private_network::internet_egress_policy::allow_internet;
   return {.host = host,
       .client_v1_enabled = allowed && policy.client_v1_enabled,
       .client_v2_enabled = allowed && policy.client_v2_enabled,
       .service_v1_enabled = allowed && policy.service_v1_enabled,
       .service_v2_enabled = allowed && policy.service_v2_enabled,
       .internet_egress_allowed = allowed, .pending_probes = stats.pending_probes,
       .pending_pings = stats.pending_pings, .waiting_probes = stats.waiters,
       .active_handlers = autonat_handlers_active, .max_pending_probes = policy.max_pending_probes,
       .max_parallel_pings = policy.max_parallel_pings, .ping_successes = stats.ping_successes,
       .ping_failures = stats.ping_failures, .probe_errors = stats.probe_errors};
}
host_event_subscription node::impl::subscribe_host_events() const { return host_event_source->subscribe(); }

boost::asio::awaitable<reachability::state> node::impl::probe_reachability_owned(std::shared_ptr<impl> self, peer_id observer) {
   if (self->private_network_enabled() && self->options.private_network->internet_egress !=
       private_network::internet_egress_policy::allow_internet) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P private-network AutoNAT requires explicit Internet egress");
   }
   self->start_reachability();
   auto session = std::shared_ptr<session_state>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      session = self->reachability_session_locked(observer);
   }
   if (!session) {
      const auto connected = co_await self->ensure_direct_session(observer, self->options.reachability_policy.timeout);
      co_await self->identify_session(connected);
   }
   auto candidate = detail::reachability_manager::observer{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (!session) { session = self->reachability_session_locked(observer); }
      if (!session) {
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P observer has no enabled AutoNAT version");
      }
      const auto found = self->sessions.find(session->id);
      if (found == self->sessions.end() || found->second != session || session->closed) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability observer session retired before probe");
      }
      if (!eligible_reachability_session(*session)) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P reachability observer is no longer qualified");
      }
      const auto supports = [&](const auto& protocol) {
         return std::ranges::find(session->remote_protocols, protocol) != session->remote_protocols.end();
      };
      candidate = {observer, *session->remote_endpoint, supports(builtins::autonat_v1),
                   supports(builtins::autonat_v2_dial_request), supports(builtins::ping), session->id};
   }
   const auto result = co_await self->reachability_manager_value->async_probe(std::move(candidate));
   co_return result.value;
}

boost::asio::awaitable<void> node::impl::select_reachability_stream_owned(std::shared_ptr<impl> self,
    std::shared_ptr<session_state> session, protocol_id protocol, detail::stream_admission_handler admission,
    opened_direct_stream& opened) {
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto found = self->sessions.find(session->id);
      if (self->stopped || self->session_admission_closed || session->closed ||
          found == self->sessions.end() || found->second != session) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability observer session retired before open");
      }
      if (!eligible_reachability_session(*session)) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P reachability observer is no longer qualified");
      }
      if (!supports_reachability(*session, protocol)) {
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P reachability observer protocol was withdrawn");
      }
   }
   opened.stream = co_await self->open_session_stream(session, protocol, false, std::move(admission));
   const auto lock = std::scoped_lock{self->mutex};
   const auto found = self->sessions.find(session->id);
   if (self->stopped || self->session_admission_closed || session->closed ||
       found == self->sessions.end() || found->second != session) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability observer session retired during open");
   }
   if (!eligible_reachability_session(*session)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P reachability observer changed during open");
   }
   if (!supports_reachability(*session, protocol)) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P reachability observer protocol changed during open");
   }
}

boost::asio::awaitable<node::impl::opened_direct_stream> node::impl::open_reachability_stream_owned(
    std::shared_ptr<impl> self, peer_id peer, protocol_id protocol, std::chrono::milliseconds timeout,
    std::shared_ptr<cancellation_latch> cancellation, std::uint64_t session_id) {
   auto tracked = self->lifecycle.track();
   if (!tracked.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability lifecycle is closed");
   }
   auto session = std::shared_ptr<session_state>{};
   auto opened = opened_direct_stream{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->stopped || self->session_admission_closed) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability node is stopping");
      }
      const auto found = self->sessions.find(session_id);
      if (found == self->sessions.end() || found->second->closed) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability observer is no longer connected");
      }
      session = found->second;
      if (session->id != session_id || session->info.remote_peer != peer || !eligible_reachability_session(*session)) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P reachability requires an identified authenticated session");
      }
      if (std::ranges::find(session->remote_protocols, protocol) == session->remote_protocols.end()) {
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P observer does not support this reachability protocol");
      }
      opened.remote_endpoint = session->remote_endpoint;
      opened.direct_endpoint = session->direct_endpoint;
   }
   // Background health traffic must never reconnect, penalize a peer, or count
   // a topology/path dial merely for opening a stream on its existing session.
   auto deadline = operation_deadline{self->runtime.context(), timeout};
   auto parent = cancellation_latch::subscribe(cancellation, [stop = deadline.stopping()] noexcept {
      static_cast<void>(stop.request_stop());
   });
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   deadline.arm([stop] noexcept { stop->request_stop(); });
   auto failure = std::exception_ptr{};
   try {
      co_await detail::async_run_with_owner_cancellation(stop,
          [self, session, protocol, stop, &opened](boost::asio::cancellation_slot slot) {
             return select_reachability_stream_owned(self, session, protocol,
                 detail::make_owner_stream_admission(slot, stop, detail::owner_stream_lifetime::negotiation), opened);
          }, {.lifecycle_stop = tracked.stop_source()});
   } catch (...) {
      failure = std::current_exception();
   }
   static_cast<void>(deadline.finish());
   const auto stopped = self->lifecycle.stop_requested();
   if (failure || deadline.timed_out() || deadline.stopped() || stopped || !opened.stream.valid()) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      opened.stream.request_cancel();
      try { co_await opened.stream.async_close(); }
      catch (...) { if (!failure) { failure = std::current_exception(); } }
      if (deadline.timed_out()) { throw_operation_timeout("P2P reachability stream open"); }
      if (deadline.stopped()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P reachability stream open canceled");
      }
      if (stopped) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability stream open stopped"); }
      if (failure) { std::rethrow_exception(failure); }
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P reachability stream open did not start");
   }
   self->increment_opened_protocol();
   co_return opened;
}

boost::asio::awaitable<reachability::result> node::impl::exchange_reachability_owned(std::shared_ptr<impl> self,
    peer_id observer, endpoint remote, bool v2, std::vector<endpoint> candidates,
    std::shared_ptr<cancellation_latch> cancellation, std::uint64_t session_id) {
   if (candidates.empty()) { co_return reachability::result{}; }
   // Each v2 request is one address. This also respects the pinned Rust index-zero limitation.
   if (v2) { candidates.resize(1); }
   else if (candidates.size() > 16) { candidates.resize(16); }
   const auto start = std::chrono::steady_clock::now();
   const auto timeout = self->options.reachability_policy.timeout;
   auto opened = co_await open_reachability_stream_owned(self, observer,
       v2 ? builtins::autonat_v2_dial_request : builtins::autonat_v1, timeout, cancellation, session_id);
   auto nonce = std::uint64_t{};
   auto registered = false;
   const auto cleanup = boost::scope::scope_exit{[&] {
      if (registered) { self->forget_autonat_v2_nonce(observer, nonce); }
   }};
   auto result = reachability::result{};
   auto failure = std::exception_ptr{};
   try {
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "AutoNAT canceled after stream open");
      }
      if (!opened.remote_endpoint || host_addresses::observer_group(remote) != host_addresses::observer_group(*opened.remote_endpoint)) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "AutoNAT observer connection changed during probe");
      }
      const auto remaining = remaining_timeout(start, timeout, "AutoNAT stream open");
      nonce = v2 ? random_nonce() : 0;
      if (v2) {
         self->remember_autonat_v2_nonce(observer, nonce);
         registered = true;
      }
      result = co_await detail::async_exchange_autonat(std::move(opened.stream), self->local,
          std::move(candidates), v2, nonce,
          [self, observer, nonce](std::shared_ptr<cancellation_latch> stop) {
             return self->async_autonat_v2_observation(observer, nonce, std::move(stop));
          }, self->runtime.context(), remaining, std::move(cancellation));
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      opened.stream.request_cancel();
      try { co_await opened.stream.async_close(); } catch (...) {}
      std::rethrow_exception(failure);
   }
   self->increment_reachability_check(result.value);
   co_return result;
}

boost::asio::awaitable<void> node::impl::ping_reachability_owned(std::shared_ptr<impl> self, peer_id peer,
    std::shared_ptr<cancellation_latch> cancellation) {
   const auto start = std::chrono::steady_clock::now();
   const auto timeout = self->options.reachability_policy.ping_timeout;
   auto session_id = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto session = self->reachability_session_locked(peer, builtins::ping);
      if (!session) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P ping observer is no longer connected"); }
      session_id = session->id;
   }
   auto opened = co_await open_reachability_stream_owned(self, peer, builtins::ping, timeout, cancellation, session_id);
   auto deadline = std::optional<operation_deadline>{};
   auto failure = std::exception_ptr{};
   try {
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P periodic ping canceled after stream open");
      }
      deadline.emplace(self->runtime.context(), remaining_timeout(start, timeout, "P2P periodic ping stream open"));
      auto parent = cancellation_latch::subscribe(cancellation, [stop = deadline->stopping()] noexcept {
         static_cast<void>(stop.request_stop());
      });
      deadline->arm([&opened] noexcept { opened.stream.request_cancel(); });
      const auto payload = forge::crypto::core::random_bytes(32);
      co_await opened.stream.async_write(payload);
      auto response = std::vector<std::uint8_t>{};
      while (response.size() < payload.size()) {
         const auto chunk = co_await opened.stream.async_read();
         if (chunk.empty() || chunk.size() > payload.size() - response.size()) {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P ping response size mismatch");
         }
         response.insert(response.end(), chunk.begin(), chunk.end());
      }
      if (response != payload) { FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P ping payload mismatch"); }
      co_await opened.stream.async_close();
      static_cast<void>(deadline->finish());
   } catch (...) {
      failure = std::current_exception();
   }
   if (deadline) { static_cast<void>(deadline->finish()); }
   if (failure || (deadline && (deadline->timed_out() || deadline->stopped()))) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      opened.stream.request_cancel();
      try { co_await opened.stream.async_close(); }
      catch (...) { if (!failure) { failure = std::current_exception(); } }
   }
   if (deadline && deadline->timed_out()) { throw_operation_timeout("P2P periodic ping"); }
   if (deadline && deadline->stopped()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P periodic ping canceled");
   }
   if (failure) { std::rethrow_exception(failure); }
}

} // namespace forge::net::p2p
