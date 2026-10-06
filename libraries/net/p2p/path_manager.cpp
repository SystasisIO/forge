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
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/scope/scope_exit.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.asio.runtime;
import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.transport.session;
import forge.net.yamux.exceptions;

#include "details/cancellation_latch.hxx"
#include "details/host_addresses.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/owner_cancellation.hxx"
#include "details/path_manager.hxx"
#include "details/path_manager_dial_batch.hxx"
#include "details/direct_transport.hxx"

namespace forge::net::p2p::detail {

path_manager::path_manager(std::shared_ptr<lifecycle_wakeup> wakeup) : _wakeup(std::move(wakeup)) {}

path_manager::claim path_manager::begin(peer_id peer, std::uint64_t session_id, role side,
                                        time_point deadline, time_point now) {
   const auto lock = std::scoped_lock{_mutex};
   if (_stopping || deadline <= now) { return {}; }
   if (const auto existing = _active.find(peer); existing != _active.end()) {
      return {.owner = existing->second};
   }
   std::erase_if(_backoffs, [now](const auto& item) { return item.second <= now; });
   if (_active.size() >= max_parallel_operations || _backoffs.size() >= max_retained_peers ||
       _backoffs.contains(peer)) { return {}; }
   auto owner = std::make_shared<operation>(operation{
       .peer = peer, .session_id = session_id, .side = side, .deadline = deadline,
       .cancellation = std::make_shared<cancellation_latch>(),
   });
   owner->terminal_waiters.reserve(max_cancel_waiters);
   // Preallocate the cooldown slot; completion never allocates or drops a backoff.
   _backoffs.emplace(peer, time_point::max());
   try { _active.emplace(peer, owner); }
   catch (...) { _backoffs.erase(peer); throw; }
   return {.owner = std::move(owner), .leader = true};
}

bool path_manager::begin_exchange(const std::shared_ptr<operation>& owner, time_point now) {
   return static_cast<bool>(start_exchange(owner, true, now));
}

std::shared_ptr<path_manager::exchange> path_manager::start_exchange(
    const std::shared_ptr<operation>& owner, bool count_attempt, time_point now) {
   const auto ticket = std::make_shared<exchange>(exchange{
       owner->session_id, owner->side, std::make_shared<cancellation_latch>()});
   const auto lock = std::scoped_lock{_mutex};
   if (_stopping || owner->completed || owner->closing || owner->current || owner->pending || owner->handed_over ||
       owner->attempts >= max_attempts || owner->deadline <= now || owner->cancellation->stop_requested()) { return {}; }
   owner->current = ticket;
   if (count_attempt) { ++owner->attempts; }
   return ticket;
}

boost::asio::awaitable<bool> path_manager::async_accept_exchange(const std::shared_ptr<operation>& owner,
    const std::shared_ptr<exchange>& incoming, const peer_id& local) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   // Normalize through the existing peer-ID codec, not display/address spelling.
   const auto local_wins = peer_id::from_bytes(local.to_bytes()) < peer_id::from_bytes(owner->peer.to_bytes());
   auto losing = std::shared_ptr<exchange>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_stopping || owner->completed || owner->closing || owner->pending || owner->attempts >= max_attempts ||
          owner->deadline <= std::chrono::steady_clock::now() || owner->cancellation->stop_requested() ||
          incoming->side != role::responder) { co_return false; }
      if (owner->current) {
         if (owner->current->side != role::initiator || owner->current->session_id == incoming->session_id ||
             local_wins) { co_return false; }
         losing = owner->current;
      }
      owner->pending = incoming;
      // The original worker becomes passive; only this owner's bounded incoming
      // exchanges may use its remaining attempts, original deadline and waiters.
      owner->handed_over = true;
   }
   if (losing) { losing->cancellation->request_stop(); }
   _wakeup->notify();
   auto stopped = cancellation_latch::subscribe(owner->cancellation, [wakeup = _wakeup] { wakeup->notify(); });
   auto child = cancellation_latch::subscribe(incoming->cancellation, [wakeup = _wakeup] { wakeup->notify(); });
   for (;;) {
      const auto epoch = _wakeup->epoch();
      {
         const auto lock = std::scoped_lock{_mutex};
         if (_stopping || owner->completed || owner->closing || owner->pending != incoming ||
             owner->cancellation->stop_requested() || incoming->cancellation->stop_requested() ||
             owner->deadline <= std::chrono::steady_clock::now()) { co_return false; }
         if (!owner->current) {
            owner->current = std::move(owner->pending);
            ++owner->attempts;
            co_return true;
         }
      }
      // The old exchange guard releases only after stream and native dial join.
      // Keep pending charged until its caller also closes the admitted stream.
      co_await _wakeup->async_wait_until(epoch, owner->deadline);
   }
}

void path_manager::end_exchange(const std::shared_ptr<operation>& owner, time_point now) noexcept {
   auto ticket = std::shared_ptr<exchange>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      ticket = owner->current;
   }
   end_exchange(owner, ticket, now);
}

void path_manager::end_exchange(const std::shared_ptr<operation>& owner,
                                const std::shared_ptr<exchange>& ticket, time_point now) noexcept {
   auto terminal = std::vector<std::weak_ptr<dial_batch>>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (owner->current == ticket) { owner->current.reset(); }
      if (owner->pending == ticket) { owner->pending.reset(); }
      if (owner->completed && !owner->current && !owner->pending) { terminal = retire_locked(owner, now); }
   }
   _wakeup->notify();
   for (const auto& waiter : terminal) {
      if (const auto pending = waiter.lock()) { pending->complete_worker(); }
   }
}

path_manager::progress path_manager::inspect(const std::shared_ptr<operation>& owner) const {
   const auto lock = std::scoped_lock{_mutex};
   return {.attempts = owner->attempts, .exchanging = static_cast<bool>(owner->current || owner->pending),
           .handed_over = owner->handed_over,
           .completed = owner->completed, .result = owner->result};
}

bool path_manager::seal(const std::shared_ptr<operation>& owner, bool original_only) {
   const auto lock = std::scoped_lock{_mutex};
   if (original_only && owner->handed_over) { return false; }
   owner->closing = true;
   return true;
}

void path_manager::finish(const std::shared_ptr<operation>& owner, hole_punch::status result, time_point now) noexcept {
   auto terminal = std::vector<std::weak_ptr<dial_batch>>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (owner->completed) { return; }
      owner->completed = true;
      owner->closing = true;
      owner->result = result;
      if (!owner->current && !owner->pending) { terminal = retire_locked(owner, now); }
   }
   _wakeup->notify();
   for (const auto& waiter : terminal) {
      if (const auto pending = waiter.lock()) { pending->complete_worker(); }
   }
}

std::vector<std::weak_ptr<path_manager::dial_batch>>
path_manager::retire_locked(const std::shared_ptr<operation>& owner, time_point now) noexcept {
   const auto found = _active.find(owner->peer);
   if (found == _active.end() || found->second != owner) { return {}; }
   if (!_stopping && owner->result != hole_punch::status::succeeded) {
      const auto backoff = _backoffs.find(owner->peer);
      if (backoff != _backoffs.end()) { backoff->second = now + failure_backoff; }
   } else { _backoffs.erase(owner->peer); }
   _active.erase(found);
   return std::move(owner->terminal_waiters);
}

boost::asio::awaitable<bool> path_manager::async_cancel(peer_id peer) {
   namespace asio = boost::asio;
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   const auto terminal = std::make_shared<dial_batch>(co_await asio::this_coro::executor);
   auto owner = std::shared_ptr<operation>{};
   auto error = boost::system::error_code{};
   co_await terminal->async_run([&] {
      {
         const auto lock = std::scoped_lock{_mutex};
         const auto found = _active.find(peer);
         if (found == _active.end() || found->second->completed) { return; }
         auto& waiters = found->second->terminal_waiters;
         std::erase_if(waiters, [](const auto& waiter) { return waiter.expired(); });
         if (waiters.size() == max_cancel_waiters) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P path cancel waiters are at capacity");
         }
         // The slot was reserved with operation admission, and the native
         // terminal wait is already installed. Nothing allocates to join after
         // this owner-level cancellation claim is accepted.
         terminal->add_worker();
         waiters.emplace_back(terminal);
         owner = found->second;
      }
      owner->cancellation->request_stop();
   }, asio::redirect_error(asio::use_awaitable, error));
   if (const auto failure = terminal->failure()) { std::rethrow_exception(failure); }
   if (error && error != asio::error::operation_aborted) { throw boost::system::system_error{error}; }
   co_return static_cast<bool>(owner);
}

void path_manager::request_stop() noexcept {
   auto owners = std::array<std::shared_ptr<operation>, max_parallel_operations>{};
   auto count = std::size_t{};
   {
      const auto lock = std::scoped_lock{_mutex};
      _stopping = true;
      _backoffs.clear();
      for (const auto& [_, owner] : _active) { owners[count++] = owner; }
   }
   // Latch callbacks may close streams and re-enter the manager or node.
   for (auto index = std::size_t{}; index < count; ++index) { owners[index]->cancellation->request_stop(); }
   _wakeup->notify();
}

boost::asio::awaitable<hole_punch::status> path_manager::async_wait(std::shared_ptr<operation> owner,
                                                                  time_point deadline) {
   for (;;) {
      const auto epoch = _wakeup->epoch();
      const auto state = inspect(owner);
      if (state.completed) { co_return state.result; }
      if (std::chrono::steady_clock::now() >= deadline) { co_return hole_punch::status::failed; }
      co_await _wakeup->async_wait_until(epoch, deadline);
   }
}

boost::asio::awaitable<void> path_manager::async_join() {
   for (;;) {
      const auto epoch = _wakeup->epoch();
      if (active() == 0) { co_return; }
      co_await _wakeup->async_wait(epoch);
   }
}

boost::asio::awaitable<bool> path_manager::async_delay(std::shared_ptr<operation> owner, time_point target,
                                                     std::shared_ptr<exchange> ticket) {
   auto stopped = cancellation_latch::subscribe(owner->cancellation, [wakeup = _wakeup] { wakeup->notify(); });
   auto child = cancellation_latch::subscribe(ticket ? ticket->cancellation : nullptr,
       [wakeup = _wakeup] { wakeup->notify(); });
   for (;;) {
      const auto epoch = _wakeup->epoch();
      const auto now = std::chrono::steady_clock::now();
      if (owner->cancellation->stop_requested() || (ticket && ticket->cancellation->stop_requested()) ||
          now >= owner->deadline) { co_return false; }
      if (now >= target) { co_return true; }
      // Sticky epochs cover cancellation before wait installation; unrelated
      // session notifications never shorten the measured RTT/2 delay.
      co_await _wakeup->async_wait_until(epoch, std::min(target, owner->deadline));
   }
}

boost::asio::awaitable<void> path_manager::async_run_exchange(
    const std::shared_ptr<operation>& owner, exchange_work work, std::shared_ptr<exchange> ticket) {
   const auto stop = std::make_shared<worker_stop_bridge>();
   auto parent = cancellation_latch::subscribe(owner->cancellation, [stop] { stop->request_stop(); });
   auto child = cancellation_latch::subscribe(ticket ? ticket->cancellation : nullptr, [stop] { stop->request_stop(); });
   if (std::chrono::steady_clock::now() >= owner->deadline) { stop->request_stop(); }
   // The bridge covers raw stream admission as well as negotiation and I/O.
   // Its task slot cancels a pending open, never the shared relay session.
   co_await async_run_with_owner_cancellation(stop,
       [&work, stop](boost::asio::cancellation_slot slot) -> boost::asio::awaitable<void> {
          co_await work(slot, stop);
       });
   parent.reset();
}

boost::asio::awaitable<void> path_manager::async_close_exchange(const std::shared_ptr<resource_stream>& resource,
                                                               std::exception_ptr failure) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   if (failure) { resource->request_cancel(); }
   // Protocol-owned streams never escape to applications. A dispatcher's
   // retained shared_ptr cannot defer their native terminal barrier.
   try { co_await resource->async_close(); }
   catch (...) { if (!failure) { failure = std::current_exception(); } }
   if (failure) { std::rethrow_exception(failure); }
}

boost::asio::awaitable<void> path_manager::async_close_completed_exchange(
    const std::shared_ptr<resource_stream>& resource) {
   try { co_await async_close_exchange(resource); }
   catch (const forge::net::yamux::exceptions::stream_reset&) {
      // async_close has already crossed the lower stream's terminal barrier.
   }
}

boost::asio::awaitable<bool> path_manager::async_wait_dials(const std::shared_ptr<dial_batch>& batch,
    const std::shared_ptr<operation>& owner, time_point deadline, std::function<bool()> has_direct,
    std::function<void()> launch, std::shared_ptr<exchange> ticket) {
   namespace asio = boost::asio;
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   auto stopped = cancellation_latch::subscribe(batch->cancellation,
       [weak = std::weak_ptr{batch}] { if (const auto current = weak.lock()) { current->stop_waiting(); } });
   auto parent = cancellation_latch::subscribe(owner->cancellation,
       [cancellation = batch->cancellation] { cancellation->request_stop(); });
   auto child = cancellation_latch::subscribe(ticket ? ticket->cancellation : nullptr,
       [cancellation = batch->cancellation] { cancellation->request_stop(); });
   {
      const auto lock = std::scoped_lock{_mutex};
      owner->dials = batch;
   }
   auto detach = boost::scope::scope_exit{[this, &owner, &batch] {
      const auto lock = std::scoped_lock{_mutex};
      if (owner->dials.lock() == batch) { owner->dials.reset(); }
   }};
   auto error = boost::system::error_code{};
   co_await batch->async_run([&] {
      if (has_direct()) { batch->direct_arrived(); }
      if (owner->cancellation->stop_requested() || (ticket && ticket->cancellation->stop_requested()) ||
          std::chrono::steady_clock::now() >= deadline) {
         batch->cancellation->request_stop();
      }
      if (!batch->cancellation->stop_requested()) { launch(); }
      if (has_direct()) { batch->direct_arrived(); }
   }, asio::redirect_error(asio::use_awaitable, error));
   // Readiness requires a sealed batch with no native workers and a completed
   // inbound window. There is no allocating post-publication drain retry.
   if (const auto failure = batch->failure()) { std::rethrow_exception(failure); }
   if (error && error != asio::error::operation_aborted) { throw boost::system::system_error{error}; }
   co_return has_direct() && !owner->cancellation->stop_requested() &&
       (!ticket || !ticket->cancellation->stop_requested()) &&
       std::chrono::steady_clock::now() < std::min(deadline, owner->deadline);
}

void path_manager::notify_direct(const peer_id& peer, path::kind kind, peer_authentication authentication) noexcept {
   if (kind != path::kind::direct || authentication == peer_authentication::unverified) { return; }
   auto batch = std::shared_ptr<dial_batch>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      const auto found = _active.find(peer);
      if (!_stopping && found != _active.end() && !found->second->completed) {
         batch = found->second->dials.lock();
      }
   }
   // Stopping loser transports can call back into node/manager admission.
   if (batch) { batch->direct_arrived(); }
}

std::size_t path_manager::active() const {
   const auto lock = std::scoped_lock{_mutex};
   return _active.size();
}

std::size_t path_manager::retained() const {
   const auto lock = std::scoped_lock{_mutex};
   return _stopping ? _active.size() : _backoffs.size();
}

path_manager::time_point path_manager::dial_deadline(time_point owner_deadline, time_point now) noexcept {
   return std::min(owner_deadline, now + hole_punch::options{}.timeout);
}

endpoint path_manager::coordinated_source(std::span<const endpoint> listeners, const endpoint& candidate,
                                           const std::optional<endpoint>& carrier_local,
                                           const std::optional<endpoint>& routed_source) {
   // Preserve the complete authenticated carrier tuple, not its interface
   // combined with an unrelated listener's port. Ownership is checked below.
   if (carrier_local && carrier_local->transport.protocol == candidate.transport.protocol &&
       carrier_local->transport.host_type == candidate.transport.host_type) {
      return direct::select_coordinated_source(listeners, candidate, carrier_local);
   }
   // Only the node caller performs kernel source lookup. This pure helper
   // verifies its complete result; it never guesses a wildcard's interface.
   if (!routed_source) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DCUtR requires an owned routed local source");
   }
   return direct::select_coordinated_source(listeners, candidate, routed_source);
}

bool path_manager::eligible(path::kind kind, peer_authentication authentication,
                            bool inbound_connection, role side) noexcept {
   return kind == path::kind::relay && authentication != peer_authentication::unverified &&
          inbound_connection == (side == role::initiator);
}

std::vector<endpoint> path_manager::direct_endpoints(const std::vector<endpoint>& values,
                                                    const peer_id& expected, std::size_t limit) {
   auto out = std::vector<endpoint>{};
   auto keys = std::set<std::string>{};
   limit = std::min(limit, hole_punch::options{}.max_observed_endpoints);
   for (const auto& value : values) {
      if (out.size() >= limit) { break; }
      if (value.relayed || (value.peer && *value.peer != expected) ||
          (!value.is_direct_tcp() && !value.is_direct_quic()) || value.transport.port == 0 ||
          host_addresses::has_interface_zone(value)) { continue; }
      try {
         auto canonical = parse_endpoint(value.to_string());
         if (canonical.relayed || (canonical.peer && *canonical.peer != expected)) { continue; }
         const auto scope = host_addresses::classify_endpoint_scope(canonical);
         if (scope == host_addresses::endpoint_scope::unroutable ||
             scope == host_addresses::endpoint_scope::link_local) { continue; }
         canonical.peer = expected;
         if (keys.insert(canonical.to_string()).second) { out.push_back(std::move(canonical)); }
      } catch (...) {
         // An invalid advertised address cannot broaden the authenticated target.
      }
   }
   return out;
}

} // namespace forge::net::p2p::detail
