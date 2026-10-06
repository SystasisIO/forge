module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.asio.runtime;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.transport.session;

#include "details/cancellation_latch.hxx"
#include "details/path_manager_dial_batch.hxx"
#include "details/coordinated_dial.hxx"

namespace forge::net::p2p::detail {
namespace {
bool same_tuple(const endpoint& left, const endpoint& right) noexcept {
   return !left.relayed && !right.relayed && left.transport.host_type == right.transport.host_type &&
       left.transport.protocol == right.transport.protocol && left.transport.host == right.transport.host &&
       left.transport.port == right.transport.port;
}
}

coordinated_dial::coordinated_dial(boost::asio::any_io_executor executor, endpoint remote_value,
    node::coordinated_connect_options options_value, std::uint64_t generation_value, upgrade_role role_value)
    : remote(std::move(remote_value)), options(std::move(options_value)), generation(generation_value), role(role_value),
      deadline(std::chrono::steady_clock::now() + options.timeout),
      cancellation(std::make_shared<cancellation_latch>()), transports(std::make_shared<cancellation_latch>()),
      _ready(std::move(executor), time_point::max()) {
   _joiners.reserve(max_joiners);
}

bool coordinated_dial::begin_inbound() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   if (_closed || _workers >= max_native_workers || std::chrono::steady_clock::now() >= deadline) { return false; }
   ++_workers;
   return true;
}

void coordinated_dial::end_inbound() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   if (_workers == 0) { std::terminate(); }
   --_workers;
   signal_locked();
}

void coordinated_dial::end_outbound(std::exception_ptr failure_value) noexcept {
   auto stop = false;
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_workers == 0) { std::terminate(); }
      if (failure_value && !_failure) { _failure = std::move(failure_value); }
      --_workers;
      // TCP refusal still leaves the exact leased inbound tuple eligible.
      if (!_winner.session.valid() && !remote.is_direct_tcp()) { _closed = true; stop = true; }
      signal_locked();
   }
   if (stop) { transports->request_stop(); }
}

bool coordinated_dial::install(direct::connection& connection, bool inbound) {
   if (!connection.session.valid() || connection.coordinated_owner.get() != this || connection.peer != options.expected_peer ||
       connection.authentication == peer_authentication::unverified || connection.role != role ||
       !connection.local_endpoint || !connection.remote_endpoint || !connection.admission ||
       !connection.admission->active() || !same_tuple(*connection.local_endpoint, options.local_source) ||
       !same_tuple(*connection.remote_endpoint, remote)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "coordinated connection lacks exact generation receipt");
   }
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_closed || _canceled || std::chrono::steady_clock::now() >= deadline) { return false; }
      connection.coordinated_owner.reset();
      connection.coordinated_inbound_worker = false;
      _winner = std::move(connection);
      _inbound_winner = inbound;
      _closed = true;
      signal_locked();
   }
   transports->request_stop();
   return true;
}

direct::connection coordinated_dial::take() {
   const auto lock = std::scoped_lock{_mutex};
   return std::move(_winner);
}
bool coordinated_dial::inbound_winner() const { const auto lock = std::scoped_lock{_mutex}; return _inbound_winner; }
bool coordinated_dial::stopped() const { const auto lock = std::scoped_lock{_mutex}; return _canceled; }
bool coordinated_dial::timed_out() const { const auto lock = std::scoped_lock{_mutex}; return _timed_out; }
std::exception_ptr coordinated_dial::failure() const { const auto lock = std::scoped_lock{_mutex}; return _failure; }

void coordinated_dial::request_cancel(bool expired) noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      _canceled = true;
      _timed_out = _timed_out || expired;
      _closed = true;
      signal_locked();
   }
   cancellation->request_stop();
   transports->request_stop();
}

void coordinated_dial::signal_locked() noexcept {
   if (_armed && _closed && _workers == 0 && !_signaled) {
      _signaled = true;
      try { if (_ready.cancel() != 1) { std::terminate(); } }
      catch (...) { std::terminate(); }
   }
}

void coordinated_dial::finish() noexcept {
   auto joiners = std::vector<std::weak_ptr<path_manager::dial_batch>>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_done || _finishing) { return; }
      if (_workers != 0) { std::terminate(); }
      _finishing = true;
   }
   // Resource release precedes the sticky terminal fact seen by late joiners.
   // Neither resource cleanup nor completion callbacks run under our mutex.
   permit.release();
   {
      const auto lock = std::scoped_lock{_mutex};
      _done = true;
      joiners = std::move(_joiners);
   }
   for (const auto& weak : joiners) { if (const auto waiter = weak.lock()) { waiter->complete_worker(); } }
}

boost::asio::awaitable<void> coordinated_dial::async_join() {
   namespace asio = boost::asio;
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   const auto waiter = std::make_shared<path_manager::dial_batch>(co_await asio::this_coro::executor);
   auto error = boost::system::error_code{};
   co_await waiter->async_run([&] {
      const auto lock = std::scoped_lock{_mutex};
      if (_done) { return; }
      if (_joiners.size() == max_joiners) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "coordinated terminal joiners are at capacity");
      }
      waiter->add_worker();
      _joiners.emplace_back(waiter);
   }, asio::redirect_error(asio::use_awaitable, error));
   if (const auto failure_value = waiter->failure()) { std::rethrow_exception(failure_value); }
   if (error && error != asio::error::operation_aborted) { throw boost::system::system_error{error}; }
}

} // namespace forge::net::p2p::detail
