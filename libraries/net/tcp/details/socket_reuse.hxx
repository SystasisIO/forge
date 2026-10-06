#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/ip/address.hpp>

namespace forge::net::tcp::detail {

// Used only before bind, on both listener and coordinated dial sockets.
boost::system::error_code enable_port_reuse(int native_socket) noexcept;
bool is_assigned_local_address(const boost::asio::ip::address& address) noexcept;

} // namespace forge::net::tcp::detail
