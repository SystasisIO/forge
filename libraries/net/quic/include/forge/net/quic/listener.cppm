module;

#include <memory>
#include <cstddef>

#include <boost/asio/awaitable.hpp>

export module forge.net.quic.listener;

import forge.asio.runtime;
import forge.net.quic.endpoint;
import forge.net.quic.options;
export import forge.net.quic.connection;

export namespace forge::net::quic {

namespace detail {
struct listener_handle;
struct listener_access;
} // namespace detail

class listener {
 public:
   listener(forge::asio::runtime& runtime, endpoint bind_endpoint, server_options options);
   ~listener();

   listener(const listener&) = delete;
   listener& operator=(const listener&) = delete;

   [[nodiscard]] endpoint local_endpoint() const;
   boost::asio::awaitable<connection> async_accept();
   // Send responder probes only. Authentication/accept correlation belongs to the caller.
   boost::asio::awaitable<std::size_t> async_punch(endpoint local, endpoint remote, punch_options options = {});
   boost::asio::awaitable<void> async_stop();
   void stop();

 private:
   friend struct detail::listener_access;
   struct impl;
   std::unique_ptr<impl> impl_;
};

namespace detail {
struct listener_access {
   static listener_handle get(listener& value);
};
} // namespace detail

} // namespace forge::net::quic
