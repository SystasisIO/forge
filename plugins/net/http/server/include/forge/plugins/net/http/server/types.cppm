module;

#include <boost/describe.hpp>

#include <cstdint>
#include <string>

export module forge.plugins.net.http.server.types;

import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::net::http::server {

enum class tls_mode : std::uint8_t {
   disabled,
   server,
   mutual,
};

struct config {
   std::string bind_address = "127.0.0.1";
   std::uint64_t port = 0;
   std::string api_base_path = "/";
   std::uint64_t max_request_body_bytes = 16 * 1024 * 1024;
   std::uint64_t max_header_bytes = 64 * 1024;
   std::uint64_t read_timeout_ms = 30'000;
   std::uint64_t idle_timeout_ms = 120'000;
   tls_mode tls_mode_value = tls_mode::disabled;
   std::string tls_certificate_chain_secret;
   std::string tls_private_key_secret;
   std::string tls_client_ca_secret;
   std::uint64_t tls_handshake_timeout_ms = 10'000;
   std::uint64_t tls_max_pending_handshakes = 64;
};

struct publish_options {
   std::string base_path;
   bool require_mutual_tls = false;
};

BOOST_DESCRIBE_ENUM(tls_mode, disabled, server, mutual)
BOOST_DESCRIBE_STRUCT(config, (),
                      (bind_address, port, api_base_path, max_request_body_bytes, max_header_bytes, read_timeout_ms,
                       idle_timeout_ms, tls_mode_value, tls_certificate_chain_secret, tls_private_key_secret,
                       tls_client_ca_secret, tls_handshake_timeout_ms, tls_max_pending_handshakes))
BOOST_DESCRIBE_STRUCT(publish_options, (), (base_path, require_mutual_tls))

} // namespace forge::plugins::net::http::server

export template <> struct forge::schema::rules<forge::plugins::net::http::server::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::http::server::config> define() {
      auto schema = forge::schema::object<forge::plugins::net::http::server::config>();
      schema.field<&forge::plugins::net::http::server::config::bind_address>("bind-address")
          .default_value("127.0.0.1")
          .description("HTTP server bind address");
      schema.field<&forge::plugins::net::http::server::config::port>("port")
          .default_value(std::uint64_t{0})
          .range(0, 65'535)
          .description("HTTP server TCP port; 0 asks the OS to choose an available port");
      schema.field<&forge::plugins::net::http::server::config::api_base_path>("api-base-path")
          .default_value("/")
          .description("Default base path for typed HTTP APIs");
      schema.field<&forge::plugins::net::http::server::config::max_request_body_bytes>("max-request-body-bytes")
          .default_value(std::uint64_t{16 * 1024 * 1024})
          .range(1, 1024ULL * 1024ULL * 1024ULL);
      schema.field<&forge::plugins::net::http::server::config::max_header_bytes>("max-header-bytes")
          .default_value(std::uint64_t{64 * 1024})
          .range(1, 16ULL * 1024ULL * 1024ULL);
      schema.field<&forge::plugins::net::http::server::config::read_timeout_ms>("read-timeout-ms")
          .default_value(std::uint64_t{30'000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::net::http::server::config::idle_timeout_ms>("idle-timeout-ms")
          .default_value(std::uint64_t{120'000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::net::http::server::config::tls_mode_value>("tls.mode")
          .default_value(forge::plugins::net::http::server::tls_mode::disabled);
      schema.field<&forge::plugins::net::http::server::config::tls_certificate_chain_secret>("tls.certificate-chain-secret")
          .default_value("");
      schema.field<&forge::plugins::net::http::server::config::tls_private_key_secret>("tls.private-key-secret")
          .default_value("");
      schema.field<&forge::plugins::net::http::server::config::tls_client_ca_secret>("tls.client-ca-secret")
          .default_value("");
      schema.field<&forge::plugins::net::http::server::config::tls_handshake_timeout_ms>("tls.handshake-timeout-ms")
          .default_value(std::uint64_t{10'000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::net::http::server::config::tls_max_pending_handshakes>("tls.max-pending-handshakes")
          .default_value(std::uint64_t{64})
          .range(1, 1'000'000);
      return schema;
   }
};
