#include <functional>
#include <memory>
#include <utility>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

import forge.net.tcp.connector;

import forge.asio.notification;

#include "details/connector_access.hxx"

namespace forge::net::tcp::detail {

void connector_access::fail_terminal_wait_for_test(connector& value) noexcept {
   value.fail_terminal_wait_for_test();
}

void connector_access::hold_attempt_completion_for_test(connector& value,
    std::shared_ptr<forge::asio::notification> entered, std::shared_ptr<forge::asio::notification> released) {
   value.hold_attempt_completion_for_test(std::move(entered), std::move(released));
}

std::size_t connector_access::pending_connects_for_test(const connector& value) noexcept {
   return value.pending_connects_for_test();
}

bool connector_access::holds_source_for_test(const connector& value) noexcept {
   return value.holds_source_for_test();
}

void connector_access::observe_reuse_fallback_for_test(connector& value,
    std::function<boost::asio::awaitable<void>(const boost::asio::ip::tcp::socket&, int,
                                              boost::system::error_code)> observer) {
   value.observe_reuse_fallback_for_test(std::move(observer));
}

connector connector_access::make(boost::asio::any_io_executor executor, options tcp_options, transport::endpoint local,
                                 connector::reuse_policy policy,
                                 std::function<bool()> source_open,
                                 std::shared_ptr<forge::asio::notification> source_closed,
                                 std::shared_ptr<forge::asio::notification> drained,
                                 std::shared_ptr<void> source_owner) {
   return connector{std::move(executor), tcp_options, std::move(local), policy, std::move(source_open),
                    std::move(source_closed), std::move(drained), std::move(source_owner)};
}

} // namespace forge::net::tcp::detail
