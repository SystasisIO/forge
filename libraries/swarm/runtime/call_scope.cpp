module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.engine;

import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"
#include "details/call_scope.hxx"

namespace forge::swarm::runtime::detail {

call_scope::call_scope(std::shared_ptr<operation_state> value, std::string key)
   : owner{std::move(value)}, peer{std::move(key)},
     deadline{std::min(std::chrono::steady_clock::now() + owner->config.callback_timeout,
        owner->total_deadline.value_or(std::chrono::steady_clock::time_point::max()))},
     _capacity{owner->budget->reserve(owner->number, peer,
       {1, 2, 4U * protocol::max_wire_message_size + 2U * protocol::max_bitmap_size, 0})} {
   owner->enter();
}
call_scope::~call_scope() {
   _grant_stop.reset(); _owner_stop.reset();
   _input.reset(); _output.reset(); grant.reset();
   _capacity.release();
   owner->leave();
}
void call_scope::bind(std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
   std::shared_ptr<forge::api::core::detail::stream_endpoint> output) {
   _input = std::move(input); _output = std::move(output);
   auto weak = weak_from_this();
   _owner_stop = std::make_unique<std::stop_callback<std::function<void()>>>(owner->stop.get_token(),
      [weak] { if (auto self = weak.lock()) self->interrupt(); });
}
void call_scope::permit(std::shared_ptr<quota> value) {
   {
      auto lock = std::scoped_lock{_mutex};
      grant = std::move(value);
      deadline = std::min(grant->expires(), owner->total_deadline.value_or(std::chrono::steady_clock::time_point::max()));
      _idle_active = true; _last_useful = std::chrono::steady_clock::now();
   }
   auto weak = weak_from_this();
   _grant_stop = std::make_unique<std::stop_callback<std::function<void()>>>(grant->stop_token(),
      [weak] { if (auto self = weak.lock()) self->interrupt(); });
   _changed.notify();
}
void call_scope::check() const {
   owner->check();
   if (stop.stop_requested()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm call canceled or grant expired");
   }
}
void call_scope::begin_transfer() {
   {
      auto lock = std::scoped_lock{_mutex};
      deadline = owner->total_deadline.value_or(std::chrono::steady_clock::time_point::max());
      _idle_active = true; _last_useful = std::chrono::steady_clock::now();
   }
   _changed.notify();
}
void call_scope::progress() noexcept {
   {
      auto lock = std::scoped_lock{_mutex};
      _last_useful = std::chrono::steady_clock::now();
      if (_paused) _pause_started = _last_useful;
   }
   _changed.notify();
}
call_scope::idle_pause call_scope::pause_idle() { return idle_pause{shared_from_this()}; }
call_scope::idle_pause::idle_pause(std::shared_ptr<call_scope> owner) : _owner{std::move(owner)} {
   { auto lock = std::scoped_lock{_owner->_mutex};
     if (_owner->_paused++ == 0) _owner->_pause_started = std::chrono::steady_clock::now(); }
   _owner->_changed.notify();
}
call_scope::idle_pause::~idle_pause() { if (_owner) _owner->resume_idle(); }
call_scope::idle_pause::idle_pause(idle_pause&& other) noexcept : _owner{std::exchange(other._owner, {})} {}
void call_scope::resume_idle() noexcept {
   {
      auto lock = std::scoped_lock{_mutex};
      if (--_paused == 0) _last_useful += std::chrono::steady_clock::now() - _pause_started;
   }
   _changed.notify();
}
void call_scope::rebind(forge::api::auth::authenticated_caller identity) {
   auto key = "authenticated:" + std::to_string(static_cast<unsigned>(identity.source)) + ":" + identity.fingerprint.str();
   _capacity.rebind(key);
   peer = std::move(key);
}
void call_scope::interrupt() noexcept {
   stop.request_stop();
   try {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm call interrupted");
   } catch (...) {
      if (_input) _input->fail(std::current_exception());
      if (_output) _output->fail(std::current_exception());
   }
   _changed.notify();
}
void call_scope::start() {
   auto self = shared_from_this();
   boost::asio::co_spawn(owner->scheduler.runtime_context().context(), watch(self),
      [self](std::exception_ptr failure) {
         if (failure) {
            { auto lock = std::scoped_lock{self->_mutex}; self->_watch_failure = failure; }
            self->interrupt();
         }
         self->_watch_done = true;
         self->_changed.notify();
      });
}
boost::asio::awaitable<void> call_scope::watch(std::shared_ptr<call_scope> self) {
   while (true) {
      const auto epoch = self->_changed.epoch();
      if (self->_finished || self->stop.stop_requested()) co_return;
      std::chrono::steady_clock::time_point deadline;
      bool expired;
      {
         auto lock = std::scoped_lock{self->_mutex}; deadline = self->deadline;
         if (self->_idle_active && self->_paused == 0)
            deadline = std::min(deadline, self->_last_useful + self->owner->config.idle_timeout);
         expired = std::chrono::steady_clock::now() >= deadline;
      }
      if (expired) { self->interrupt(); co_return; }
      co_await self->_changed.async_wait_until(epoch, deadline);
   }
}
boost::asio::awaitable<void> call_scope::finish() {
   _finished = true;
   _changed.notify();
   while (true) {
      const auto epoch = _changed.epoch();
      if (_watch_done) {
         std::exception_ptr failure;
         { auto lock = std::scoped_lock{_mutex}; failure = _watch_failure; }
         if (failure) std::rethrow_exception(failure);
         co_return;
      }
      co_await _changed.async_wait(epoch);
   }
}

} // namespace forge::swarm::runtime::detail
