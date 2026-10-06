module;

#include <cstddef>
#include <chrono>

export module forge.net.tcp.options;

export namespace forge::net::tcp {

struct options {
   std::size_t read_chunk_size = 64 * 1024;
   bool no_delay = true;
   bool keep_alive = false;
   bool reuse_address = true;
   // Opt in before binding a listener that will supply coordinated connectors.
   bool reuse_port = false;
   std::chrono::milliseconds connect_timeout{10'000};
   std::size_t max_pending_connects = 256;
};

} // namespace forge::net::tcp
