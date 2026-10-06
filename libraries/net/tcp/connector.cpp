module;

#include <forge/exceptions/macros.hpp>
#include "details/socket_reuse.hxx"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.tcp.connector;

import forge.asio.notification;

#include "details/connector_impl.hxx"

namespace forge::net::tcp {

connector::connector() = default;
connector::connector(boost::asio::any_io_executor executor, options tcp_options)
    : impl_(std::make_shared<impl>(std::move(executor), tcp_options)) {
   impl_->start_terminal_worker();
}
connector::~connector() = default;
connector::connector(boost::asio::any_io_executor executor, options tcp_options, transport::endpoint local,
                     reuse_policy policy,
                     std::function<bool()> source_open, std::shared_ptr<forge::asio::notification> source_closed,
                     std::shared_ptr<forge::asio::notification> drained, std::shared_ptr<void> source_owner)
    : impl_(std::make_shared<impl>(std::move(executor), tcp_options)) {
   impl_->local = std::move(local);
   impl_->reuse = policy;
   impl_->source_open = std::move(source_open);
   impl_->source_closed = std::move(source_closed);
   impl_->terminal_completed = std::move(drained);
   impl_->source_owner = std::move(source_owner);
   impl_->start_terminal_worker();
}
connector::connector(connector&&) noexcept = default;
connector& connector::operator=(connector&&) noexcept = default;

void connector::fail_terminal_wait_for_test() noexcept {
   if (impl_) {
      impl_->fail_terminal_wait_for_test.store(true);
   }
}

void connector::hold_attempt_completion_for_test(std::shared_ptr<forge::asio::notification> entered,
                                                 std::shared_ptr<forge::asio::notification> released) {
   impl_->before_attempt_release_for_test =
       [entered = std::move(entered), released = std::move(released)]() -> boost::asio::awaitable<void> {
          entered->notify();
          co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
          static_cast<void>(co_await released->async_wait(0));
       };
}

std::size_t connector::pending_connects_for_test() const noexcept {
   return impl_ ? impl_->sockets->size() : 0;
}

void connector::observe_reuse_fallback_for_test(std::function<boost::asio::awaitable<void>(
    const boost::asio::ip::tcp::socket&, int, boost::system::error_code)> observer) {
   impl_->before_reuse_fallback_for_test = std::move(observer);
}

bool connector::holds_source_for_test() const noexcept {
   return impl_ && static_cast<bool>(impl_->source_owner);
}

bool connector::valid() const noexcept {
   return impl_ && impl_->valid();
}

boost::asio::awaitable<connection> connector::async_connect_connection(transport::endpoint remote,
                                                                       transport::connect_options,
                                                                       std::shared_ptr<void> lifetime) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connector");
   }
   auto state = impl_;
   co_return co_await state->async_connect_connection(std::move(remote), std::move(lifetime));
}

boost::asio::awaitable<transport::stream_connection> connector::async_connect(transport::endpoint remote,
                                                                              transport::connect_options options) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connector");
   }
   auto state = impl_;
   co_return co_await state->async_connect(std::move(remote), options);
}

void connector::cancel() {
   request_cancel();
}

void connector::request_cancel() noexcept {
   if (impl_) {
      impl_->request_cancel();
   }
}

boost::asio::awaitable<void> connector::async_stop() {
   if (auto state = impl_) {
      co_await state->async_stop();
   }
}

transport::stream_connector connector::as_transport() const {
   if (!valid()) {
      return {};
   }
   return transport::detail::stream_connector_access::make(impl_);
}

} // namespace forge::net::tcp
