module;

#include <memory>
#include <functional>
#include <cstddef>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

namespace forge::net::tcp::detail {
struct connector_access;
}

export module forge.net.tcp.connector;

import forge.asio.notification;

export import forge.net.tcp.connection;
export import forge.net.tcp.exceptions;
export import forge.net.tcp.options;
export import forge.net.transport.connector;

export namespace forge::net::tcp {

class connector {
 public:
   enum class reuse_policy { preferred, required };

   connector();
   explicit connector(boost::asio::any_io_executor executor, options tcp_options = {});
   ~connector();

   connector(connector&&) noexcept;
   connector& operator=(connector&&) noexcept;

   connector(const connector&) = delete;
   connector& operator=(const connector&) = delete;

   [[nodiscard]] bool valid() const noexcept;

   boost::asio::awaitable<connection> async_connect_connection(transport::endpoint remote,
                                                               transport::connect_options connect_options = {},
                                                               std::shared_ptr<void> lifetime = {});
   boost::asio::awaitable<transport::stream_connection> async_connect(transport::endpoint remote,
                                                                      transport::connect_options connect_options = {});
   void cancel();
   void request_cancel() noexcept;
   boost::asio::awaitable<void> async_stop();

   // Shares the native owner. Destroying or replacing this facade does not
   // cancel other views; explicit cancel/async_stop stops the shared connector.
   [[nodiscard]] transport::stream_connector as_transport() const;

 private:
   friend struct detail::connector_access;
   void fail_terminal_wait_for_test() noexcept;
   void hold_attempt_completion_for_test(std::shared_ptr<forge::asio::notification> entered,
                                         std::shared_ptr<forge::asio::notification> released);
   void observe_reuse_fallback_for_test(std::function<boost::asio::awaitable<void>(
       const boost::asio::ip::tcp::socket&, int, boost::system::error_code)> observer);
   [[nodiscard]] std::size_t pending_connects_for_test() const noexcept;
   [[nodiscard]] bool holds_source_for_test() const noexcept;
   connector(boost::asio::any_io_executor executor, options tcp_options, transport::endpoint local,
             reuse_policy policy,
             std::function<bool()> source_open, std::shared_ptr<forge::asio::notification> source_closed,
             std::shared_ptr<forge::asio::notification> drained, std::shared_ptr<void> source_owner);
   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::net::tcp
