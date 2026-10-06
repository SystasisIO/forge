#include <concepts>

import forge.net.tcp.connector;
import forge.net.tcp.listener;
import forge.net.tcp.options;
import forge.net.transport.endpoint;

static_assert(requires(const forge::net::tcp::listener& listener, forge::net::transport::endpoint local) {
   { listener.make_coordinated_connector(local) } -> std::same_as<forge::net::tcp::connector>;
});

int main() {
   const auto options = forge::net::tcp::options{};
   return options.read_chunk_size == 0 ? 1 : 0;
}
