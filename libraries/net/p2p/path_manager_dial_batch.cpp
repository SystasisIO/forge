module;

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;

#include "details/cancellation_latch.hxx"
#include "details/path_manager_dial_batch.hxx"

namespace forge::net::p2p::detail {

path_manager::dial_batch::dial_batch(boost::asio::any_io_executor executor)
    : cancellation(std::make_shared<cancellation_latch>()), _ready(std::move(executor), time_point::max()) {}

void path_manager::dial_batch::add_worker(bool wait_inbound) {
   const auto lock = std::scoped_lock{_mutex};
   if (_sealed || _workers >= max_parallel_dials) { throw std::logic_error{"invalid path dial batch admission"}; }
   ++_workers;
   _wait_inbound = _wait_inbound || wait_inbound;
}

void path_manager::dial_batch::complete_worker() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   if (_workers == 0) { std::terminate(); }
   --_workers;
   signal_ready_locked();
}

void path_manager::dial_batch::stop_waiting() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   _stopped = true;
   signal_ready_locked();
}

void path_manager::dial_batch::direct_arrived() noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      _succeeded = true;
      signal_ready_locked();
   }
   cancellation->request_stop();
}

void path_manager::dial_batch::fail(std::exception_ptr error) noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      if (!_failure) { _failure = std::move(error); }
      _stopped = true;
      signal_ready_locked();
   }
   cancellation->request_stop();
}

std::size_t path_manager::dial_batch::active() const {
   const auto lock = std::scoped_lock{_mutex};
   return _workers;
}

std::exception_ptr path_manager::dial_batch::failure() const {
   const auto lock = std::scoped_lock{_mutex};
   return _failure;
}

void path_manager::dial_batch::seal() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   _sealed = true;
   signal_ready_locked();
}

void path_manager::dial_batch::signal_ready_locked() noexcept {
   if (_sealed && _workers == 0 && !_signaled && (!_wait_inbound || _stopped || _succeeded)) {
      _signaled = true;
      // Current Boost exposes only the throwing overload. Readiness must
      // cancel the one prearmed wait; a failed wake cannot be ignored or
      // retried after native owners have retired.
      try {
         if (_ready.cancel() != 1) { std::terminate(); }
      } catch (...) { std::terminate(); }
   }
}

} // namespace forge::net::p2p::detail
