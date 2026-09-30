module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.resolver.types;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.transport.options;
import forge.api.transport.connection;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::net::p2p::resolver {

struct config {
   std::string protocol_id = "/forge/api/resolver/2";
   std::uint64_t cache_ttl_ms = 60'000;
   std::uint64_t query_deadline_ms = 5'000;
   std::uint64_t open_deadline_ms = 10'000;
   std::uint64_t request_deadline_ms = 0;
   std::uint64_t max_cached_peers = 4'096;
   std::uint64_t max_apis_per_peer = 1'024;
   std::uint64_t max_methods_per_api = 256;
   std::uint64_t max_errors_per_method = 64;
   std::uint64_t max_managed_peers = 64;
   std::uint64_t max_managed_remotes = 1'024;
   std::uint64_t max_managed_waiters = 1'024;
};

struct publish_options {
   forge::api::transport::options transport{};
};

struct resolve_options {
   std::chrono::milliseconds query_deadline{0};
   std::chrono::milliseconds open_deadline{0};
   std::chrono::milliseconds request_deadline{0};
   bool force_refresh = false;
};

struct managed_remote_options {
   resolve_options resolution{};
   std::uint32_t max_connect_rounds = 3;
   std::chrono::milliseconds initial_backoff{100};
   std::chrono::milliseconds max_backoff{5'000};
};

struct error {
   std::string name;
   forge::api::core::error_identity identity;
   forge::api::core::status status_code = forge::api::core::status::internal;
   bool retryable = false;

   bool operator==(const error&) const = default;
};

struct method {
   std::string name;
   forge::api::core::method_kind kind = forge::api::core::method_kind::unary;
   std::vector<error> errors;

   bool operator==(const method&) const = default;
};

struct entry {
   forge::api::core::api_id id;
   forge::api::core::api_version version;
   std::string protocol;
   forge::api::core::codec_id codec{.value = "forge.raw"};
   std::uint64_t max_inflight = 0;
   std::uint64_t max_frame_size = 0;
   std::vector<method> methods;

   bool operator==(const entry&) const = default;
};

struct resolution {
   entry api;

   bool operator==(const resolution&) const = default;
};

struct resolved_connection {
   forge::api::transport::connection connection;
   forge::api::core::api_ref selected;
};

struct query {
   std::vector<forge::api::core::api_ref> apis;

   bool operator==(const query&) const = default;
};

struct response {
   std::vector<entry> apis;

   bool operator==(const response&) const = default;
};

BOOST_DESCRIBE_STRUCT(config, (),
                      (protocol_id, cache_ttl_ms, query_deadline_ms, open_deadline_ms, request_deadline_ms,
                       max_cached_peers, max_apis_per_peer, max_methods_per_api, max_errors_per_method,
                       max_managed_peers, max_managed_remotes, max_managed_waiters))
BOOST_DESCRIBE_STRUCT(error, (), (name, identity, status_code, retryable))
BOOST_DESCRIBE_STRUCT(method, (), (name, kind, errors))
BOOST_DESCRIBE_STRUCT(entry, (), (id, version, protocol, codec, max_inflight, max_frame_size, methods))
BOOST_DESCRIBE_STRUCT(query, (), (apis))
BOOST_DESCRIBE_STRUCT(response, (), (apis))

} // namespace forge::plugins::net::p2p::resolver

export template <> struct forge::schema::rules<forge::plugins::net::p2p::resolver::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::resolver::config> define() {
      auto schema = forge::schema::object<forge::plugins::net::p2p::resolver::config>();
      schema.field<&forge::plugins::net::p2p::resolver::config::protocol_id>("protocol-id")
          .default_value("/forge/api/resolver/2")
          .description("P2P protocol id used for FORGE API metadata resolution");
      schema.field<&forge::plugins::net::p2p::resolver::config::cache_ttl_ms>("cache-ttl-ms")
          .default_value(std::uint64_t{60'000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::query_deadline_ms>("query-deadline-ms")
          .default_value(std::uint64_t{5'000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::open_deadline_ms>("open-deadline-ms")
          .default_value(std::uint64_t{10'000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::request_deadline_ms>("request-deadline-ms")
          .default_value(std::uint64_t{0})
          .range(0, 86'400'000)
          .description("optional connection-wide request deadline; zero preserves method-owned deadlines");
      schema.field<&forge::plugins::net::p2p::resolver::config::max_cached_peers>("max-cached-peers")
          .default_value(std::uint64_t{4'096})
          .range(1, 1'000'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::max_apis_per_peer>("max-apis-per-peer")
          .default_value(std::uint64_t{1'024})
          .range(1, 1'000'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::max_methods_per_api>("max-methods-per-api")
          .default_value(std::uint64_t{256})
          .range(1, 1'000'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::max_errors_per_method>("max-errors-per-method")
          .default_value(std::uint64_t{64})
          .range(0, 1'000'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::max_managed_peers>("managed.max-peers")
          .default_value(std::uint64_t{64})
          .range(1, 1'024);
      schema.field<&forge::plugins::net::p2p::resolver::config::max_managed_remotes>("managed.max-remotes")
          .default_value(std::uint64_t{1'024})
          .range(1, 1'000'000);
      schema.field<&forge::plugins::net::p2p::resolver::config::max_managed_waiters>("managed.max-waiters")
          .default_value(std::uint64_t{1'024})
          .range(1, 1'000'000);
      return schema;
   }
};
