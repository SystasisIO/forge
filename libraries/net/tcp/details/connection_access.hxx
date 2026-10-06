#pragma once

#include "connection_test_hooks.hxx"

extern "C++" {
namespace forge::net::tcp::detail {

struct connection_access {
   static connection make(boost::asio::ip::tcp::socket socket, options tcp_options,
                          std::shared_ptr<void> lifetime, std::shared_ptr<connection_test_hooks> hooks);
};

} // namespace forge::net::tcp::detail
}
