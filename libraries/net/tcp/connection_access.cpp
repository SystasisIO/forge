#include <memory>
#include <utility>
#include <boost/asio/ip/tcp.hpp>

import forge.net.tcp.connection;

#include "details/connection_access.hxx"

namespace forge::net::tcp::detail {

connection connection_access::make(boost::asio::ip::tcp::socket socket, options tcp_options,
                                   std::shared_ptr<void> lifetime, std::shared_ptr<connection_test_hooks> hooks) {
   return connection{std::move(socket), tcp_options, std::move(lifetime), std::move(hooks)};
}

} // namespace forge::net::tcp::detail
