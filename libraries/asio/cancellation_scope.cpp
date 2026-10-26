module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/strand.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

module forge.asio.cancellation_scope;

#include "details/cancellation_scope_impl.hxx"

namespace forge::asio {

cancellation_scope::cancellation_scope(boost::asio::any_io_executor executor, std::span<const std::stop_token> sources,
                                       boost::asio::cancellation_state parent)
    : _impl(std::make_shared<impl>(std::move(executor), sources, parent)) {}

cancellation_scope::~cancellation_scope() = default;

std::stop_token cancellation_scope::stop_token() const noexcept {
   return _impl->stopping->get_token();
}

void cancellation_scope::request_stop() noexcept {
   _impl->stopping->request_stop();
}

std::shared_ptr<cancellation_scope::impl> cancellation_scope::claim() {
   if (_impl->claimed.exchange(true, std::memory_order_acq_rel)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "Cancellation scope already owns an invocation");
   }

   return _impl;
}

boost::asio::any_io_executor cancellation_scope::executor(const std::shared_ptr<impl>& state) {
   return state->executor;
}

std::shared_ptr<std::stop_source> cancellation_scope::source(const std::shared_ptr<impl>& state) {
   return state->stopping;
}

} // namespace forge::asio
