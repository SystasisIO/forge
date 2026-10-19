module;

#include <exception>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/signal_set.hpp>

module forge.asio.child_signal_registration;

#include "details/child_signal_registration_impl.hxx"

namespace forge::asio {

child_signal_registration::child_signal_registration() : _impl{std::make_unique<impl>()} {}

child_signal_registration::~child_signal_registration() {
   if (_impl->registered())
      std::terminate(); // Never implicitly overwrite a foreign/native disposition.
}

void child_signal_registration::acquire(boost::asio::any_io_executor executor) {
   _impl->acquire(std::move(executor), nullptr);
}

void child_signal_registration::acquire(boost::asio::any_io_executor executor, void (*after_install)()) {
   _impl->acquire(std::move(executor), after_install);
}

void child_signal_registration::release() {
   _impl->release();
}

bool child_signal_registration::registered() const noexcept {
   return _impl->registered();
}

} // namespace forge::asio
