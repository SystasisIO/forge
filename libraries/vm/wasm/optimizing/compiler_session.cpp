module;

#include <coroutine>
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <forge/exceptions/macros.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>
#include <variant>

module forge.vm.wasm.optimizing.compiler_session;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"

namespace forge::vm::wasm::optimizing {

compiler_session::ticket::ticket() noexcept = default;
compiler_session::ticket::ticket(std::shared_ptr<state> value) : _state{std::move(value)} {}
compiler_session::ticket::~ticket() {
   static_cast<void>(cancel());
}
compiler_session::ticket::ticket(ticket&&) noexcept = default;
compiler_session::ticket& compiler_session::ticket::operator=(ticket&& other) noexcept {
   if (this != &other) {
      static_cast<void>(cancel());
      _state = std::move(other._state);
   }
   return *this;
}
bool compiler_session::ticket::valid() const noexcept {
   return _state != nullptr;
}
bool compiler_session::ticket::cancel() noexcept {
   return _state && _state->cancel();
}

boost::asio::awaitable<compiler_session::completion>
compiler_session::ticket::wait(std::chrono::steady_clock::time_point deadline, std::stop_token stop) && {
   return wait_owned(std::exchange(_state, {}), deadline, std::move(stop));
}

boost::asio::awaitable<compiler_session::completion>
compiler_session::ticket::wait_owned(std::shared_ptr<state> value, std::chrono::steady_clock::time_point deadline,
                                     std::stop_token stop) {
   if (!value)
      FORGE_THROW_EXCEPTION(asio::exceptions::invalid_state, "compiler ticket is empty");
   const auto observed = value->changed.epoch();
   {
      const auto lock = std::scoped_lock{value->mutex};
      if (value->completed) {
         if (value->cancelled)
            FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler ticket was cancelled");
         if (value->result.error)
            std::rethrow_exception(value->result.error);
         co_return *value->result.value;
      }
   }
   try {
      const auto current = co_await value->changed.async_wait_until(observed, deadline, stop);
      if (current == observed && value->cancel()) {
         ::forge::exceptions::throw_timeout_error("compiler ticket deadline exceeded", {});
      }
      const auto lock = std::scoped_lock{value->mutex};
      if (value->cancelled)
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler ticket was cancelled");
      if (value->result.error)
         std::rethrow_exception(value->result.error);
      co_return *value->result.value;
   } catch (const boost::system::system_error& error) {
      static_cast<void>(value->cancel());
      if (error.code() == boost::asio::error::operation_aborted)
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler ticket wait was cancelled");
      throw;
   } catch (...) {
      static_cast<void>(value->cancel());
      throw;
   }
}

compiler_session::compiler_session(asio::runtime& runtime, asio::compute::executor compute, session_options options)
    : _impl{std::make_unique<impl>(runtime, std::move(compute), std::move(options))} {}
compiler_session::~compiler_session() = default;

boost::asio::awaitable<void> compiler_session::initialize(target_profile expected, std::stop_token stop) {
   return _impl->initialize(std::move(expected), stop);
}

std::optional<artifact> compiler_session::ready(const crypto::digest::sha256& identity,
                                                const compile_configuration& expected) const {
   return _impl->ready(identity, expected);
}
compiler_session::enqueue_result compiler_session::enqueue(const crypto::digest::sha256& hint,
                                                           compile_request&& request, asio::task::priority priority) {
   return _impl->enqueue(hint, std::move(request), priority);
}
void compiler_session::request_stop() {
   _impl->request_stop();
}
boost::asio::awaitable<void> compiler_session::shutdown(std::chrono::steady_clock::time_point escalation) {
   return _impl->shutdown(escalation);
}

} // namespace forge::vm::wasm::optimizing
