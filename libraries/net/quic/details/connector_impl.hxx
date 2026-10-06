#pragma once

namespace forge::net::quic {
namespace asio = boost::asio;
detail::engine_client_options map_options(const client_options& options);
struct connector::impl {
   explicit impl(forge::asio::runtime& runtime_value);
   impl(forge::asio::runtime& runtime_value, detail::engine_listener& source, endpoint local);

   forge::asio::runtime& runtime;
   detail::engine_connector engine;
   std::shared_ptr<detail::client_token_cache> client_tokens;
};
} // namespace forge::net::quic
