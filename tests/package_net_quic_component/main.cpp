#include <concepts>
#include <type_traits>

#include <boost/asio/awaitable.hpp>

import forge.asio.runtime;
import forge.net.quic.connector;
import forge.net.quic.endpoint;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.transport;

static_assert(std::is_constructible_v<forge::net::quic::connector, forge::asio::runtime&,
                                    forge::net::quic::listener&, forge::net::quic::endpoint>);
static_assert(requires(forge::net::quic::listener& listener, forge::net::quic::endpoint local,
                      forge::net::quic::endpoint remote) {
   { listener.async_punch(local, remote) } -> std::same_as<boost::asio::awaitable<std::size_t>>;
   { listener.async_stop() } -> std::same_as<boost::asio::awaitable<void>>;
});

int main() {
   const auto limits = forge::net::quic::from_transport_limits({});
   return limits.max_connections == 0 ? 1 : 0;
}
