#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

import forge.net.stcp.connection;

#include "details/connection_access.hxx"

namespace forge::net::stcp::detail {

boost::asio::awaitable<connection>
connection_access::async_upgrade_client(tcp::connection source, client_options options,
                                         std::chrono::milliseconds timeout,
                                         std::shared_ptr<connection_test_hooks> hooks) {
   return connection::async_upgrade_native(std::move(source), std::move(options), std::optional{timeout}, {},
                                            std::move(hooks));
}

} // namespace forge::net::stcp::detail
