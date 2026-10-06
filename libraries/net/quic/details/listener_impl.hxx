#pragma once

namespace forge::net::quic {
namespace asio = boost::asio;
detail::engine_server_options map_options(const server_options& options);
struct listener::impl {
   impl(forge::asio::runtime& runtime_value, endpoint bind_endpoint_value, server_options options_value);

   forge::asio::runtime& runtime;
   std::shared_ptr<detail::engine_listener> engine;
};
} // namespace forge::net::quic
