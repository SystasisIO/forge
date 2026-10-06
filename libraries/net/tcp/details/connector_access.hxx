#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

extern "C++" {
namespace forge::net::tcp::detail {

struct connector_access {
   // Fixture-only access; configure and inspect with the executor quiescent.
   static void fail_terminal_wait_for_test(connector& value) noexcept;
   static void hold_attempt_completion_for_test(connector& value,
       std::shared_ptr<forge::asio::notification> entered, std::shared_ptr<forge::asio::notification> released);
   static void observe_reuse_fallback_for_test(connector& value, std::function<boost::asio::awaitable<void>(
       const boost::asio::ip::tcp::socket&, int, boost::system::error_code)> observer);
   static std::size_t pending_connects_for_test(const connector& value) noexcept;
   static bool holds_source_for_test(const connector& value) noexcept;
   static connector make(boost::asio::any_io_executor executor, options tcp_options, transport::endpoint local,
                         connector::reuse_policy policy,
                         std::function<bool()> source_open, std::shared_ptr<forge::asio::notification> source_closed,
                         std::shared_ptr<forge::asio::notification> drained, std::shared_ptr<void> source_owner);
};

} // namespace forge::net::tcp::detail
}
