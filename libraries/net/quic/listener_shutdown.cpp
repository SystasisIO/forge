#include "details/listener_shutdown.hxx"

#include <cassert>

namespace forge::net::quic::detail {

listener_shutdown::listener_shutdown(boost::asio::any_io_executor executor, std::size_t connections)
    : _ready(executor, boost::asio::steady_timer::time_point::max()),
      _finished(std::move(executor), boost::asio::steady_timer::time_point::max()),
      _completed(connections, false), _remaining(connections) {}

void listener_shutdown::cancel(boost::asio::steady_timer& timer) noexcept {
   try {
      static_cast<void>(timer.cancel());
   } catch (...) {
      // Native completion cannot silently lose its already registered wait.
      std::terminate();
   }
}

void listener_shutdown::signal_ready() noexcept {
   if (!_ready_signaled && (_aborted || (_admitted && _stop_complete && _operations_complete && _remaining == 0))) {
      _ready_signaled = true;
      cancel(_ready);
   }
}

void listener_shutdown::admit() noexcept {
   auto lock = std::scoped_lock{_mutex};
   assert(!_aborted);
   _admitted = true;
   signal_ready();
}

void listener_shutdown::complete_stop() noexcept {
   auto lock = std::scoped_lock{_mutex};
   assert(_admitted && !_aborted);
   _stop_complete = true;
   signal_ready();
}

void listener_shutdown::complete_connection(std::size_t index) noexcept {
   auto lock = std::scoped_lock{_mutex};
   assert(index < _completed.size());
   if (_aborted || _completed[index]) { return; }
   _completed[index] = true;
   assert(_remaining != 0);
   --_remaining;
   signal_ready();
}

void listener_shutdown::set_operations_idle(bool idle) noexcept {
   auto lock = std::scoped_lock{_mutex};
   if (_aborted) { return; }
   assert(!_ready_signaled || idle);
   _operations_complete = idle;
   signal_ready();
}

void listener_shutdown::remember(std::exception_ptr error) noexcept {
   auto lock = std::scoped_lock{_mutex};
   if (!_error) { _error = std::move(error); }
}

void listener_shutdown::abort_preparation(std::exception_ptr error) noexcept {
   auto lock = std::scoped_lock{_mutex};
   assert(!_admitted);
   if (!_error) { _error = std::move(error); }
   _aborted = true;
   signal_ready();
}

void listener_shutdown::finish() noexcept {
   auto lock = std::scoped_lock{_mutex};
   assert(_ready_signaled);
   _finished_signaled = true;
   cancel(_finished);
}

bool listener_shutdown::admitted() const noexcept {
   auto lock = std::scoped_lock{_mutex};
   return _admitted;
}

std::exception_ptr listener_shutdown::failure() const noexcept {
   auto lock = std::scoped_lock{_mutex};
   return _error;
}

} // namespace forge::net::quic::detail
