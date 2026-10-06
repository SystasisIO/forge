module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module forge.net.quic.options;

import forge.net.quic.endpoint;
import forge.net.quic.security;

export namespace forge::net::quic {

struct transport_limits {
   std::size_t max_connections = 1024;
   std::size_t max_streams_per_connection = 256;
   std::size_t max_queued_bytes = 16 * 1024 * 1024;
   std::size_t max_inbound_queued_bytes = 16 * 1024 * 1024;
   std::size_t max_inbound_queued_packets = 4096;
   std::uint64_t max_frame_size = 16 * 1024 * 1024;
};

// Callbacks may be invoked concurrently by successful client connections. They
// must be synchronous, nonblocking and internally thread-safe.
struct client_token_callbacks {
   std::function<std::optional<std::vector<std::uint8_t>>()> take;
   std::function<void(std::vector<std::uint8_t>)> store;
};

struct client_options {
   std::string alpn = "forge-p2p/1";
   std::chrono::milliseconds connect_timeout{10'000};
   std::chrono::milliseconds handshake_timeout{10'000};
   std::chrono::milliseconds idle_timeout{30'000};
   transport_limits limits{};
   security_options security{};
   std::string certificate_pem;
   std::string private_key_pem;
   std::function<bool(std::string_view)> test_failpoint;
   // Absent uses connector-owned caching. Both empty explicitly disables it.
   std::optional<client_token_callbacks> client_tokens;
   // Opaque owner lifetime for the native client UDP socket. QUIC never
   // interprets this value and releases it when the connection closes.
   std::shared_ptr<void> connection_lifetime;
};

struct punch_options {
   std::chrono::milliseconds timeout{5'000};
   std::size_t max_packets = 64;
   // Admission/lifetime is owned by the caller, never interpreted by QUIC.
   std::shared_ptr<void> lifetime;
};

struct server_options {
   std::string alpn = "forge-p2p/1";
   std::chrono::milliseconds handshake_timeout{10'000};
   std::chrono::milliseconds idle_timeout{30'000};
   transport_limits limits{};
   security_options security{.verify_peer = false};
   std::string certificate_pem;
   std::string private_key_pem;
   // Runs after a new UDP peer is accepted and before any TLS/QUIC state is
   // allocated. A false result rejects that connection independently of
   // resource admission.
   std::function<bool(const endpoint& local, const endpoint& remote)> inbound_connection_filter;
   std::function<std::shared_ptr<void>()> inbound_admission;
};

void validate(const client_options& options);
void validate(const server_options& options);

} // namespace forge::net::quic
