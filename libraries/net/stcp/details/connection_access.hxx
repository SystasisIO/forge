#pragma once

#include "connection_test_hooks.hxx"

extern "C++" {
namespace forge::net::stcp::detail {

struct connection_access {
   static boost::asio::awaitable<connection>
   async_upgrade_client(tcp::connection source, client_options options, std::chrono::milliseconds timeout,
                        std::shared_ptr<connection_test_hooks> hooks);
};

} // namespace forge::net::stcp::detail
}
