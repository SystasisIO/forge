module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.endpoint;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.identity;
import forge.multiformats.multiaddr;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.rendezvous;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

[[nodiscard]] constexpr std::size_t with_provisional_session(std::size_t active_limit) noexcept {
   return active_limit == (std::numeric_limits<std::size_t>::max)() ? active_limit : active_limit + 1;
}

static_assert(with_provisional_session((std::numeric_limits<std::size_t>::max)()) ==
              (std::numeric_limits<std::size_t>::max)());

[[nodiscard]] forge::net::p2p::path::policy parse_path_policy(path_policy value, bool relay_client_enabled,
                                                              std::size_t relay_max_candidates) {
   if (relay_max_candidates == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P relay candidate limit must be positive");
   }
   switch (value) {
   case path_policy::direct_only:
      return forge::net::p2p::path::policy{
          .allow_direct = true,
          .allow_hole_punch = false,
          .allow_relay = false,
          .max_relay_candidates = relay_max_candidates,
      };
   case path_policy::direct_preferred:
      return forge::net::p2p::path::policy{
          .allow_direct = true,
          .allow_hole_punch = relay_client_enabled,
          .allow_relay = relay_client_enabled,
          .max_relay_candidates = relay_max_candidates,
      };
   case path_policy::relay_only:
      return forge::net::p2p::path::policy{
          .allow_direct = false,
          .allow_hole_punch = false,
          .allow_relay = relay_client_enabled,
          .max_relay_candidates = relay_max_candidates,
      };
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P path policy");
}

[[nodiscard]] forge::net::p2p::dht::mode parse_dht_mode(dht_mode value) {
   switch (value) {
   case dht_mode::client:
      return forge::net::p2p::dht::mode::client;
   case dht_mode::server:
      return forge::net::p2p::dht::mode::server;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P DHT mode");
}

[[nodiscard]] std::vector<forge::net::p2p::dht::profile>
parse_dht_profiles(const std::vector<dht_profile_config>& configured) {
   auto profiles = std::vector<forge::net::p2p::dht::profile>{};
   auto protocols = std::set<forge::net::p2p::protocol_id>{};
   profiles.reserve(configured.size());
   for (const auto& item : configured) {
      auto profile = forge::net::p2p::dht::profile{};
      try {
         const auto mode = parse_dht_mode(item.mode);
         switch (item.kind) {
         case dht_profile_kind::amino_v1:
            if ((!item.protocol.empty() && item.protocol != forge::net::p2p::builtins::kad_dht.value) || !item.peers ||
                !item.providers || !item.values) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Amino DHT protocol and capabilities are fixed");
            }
            profile = forge::net::p2p::amino_v1(mode);
            break;
         case dht_profile_kind::custom:
            if (item.protocol.empty()) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config, "custom DHT profile requires a protocol ID");
            }
            if (item.values) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                                     "configured custom DHT values require a product-owned C++ validator and selector");
            }
            profile = forge::net::p2p::custom_dht_profile(forge::net::p2p::protocol_id{.value = item.protocol}, mode,
                                                          forge::net::p2p::dht::profile_capabilities{
                                                              .peers = item.peers,
                                                              .providers = item.providers,
                                                              .values = false,
                                                          });
            break;
         }
      } catch (const forge::plugins::net::p2p::node::exceptions::invalid_config&) {
         throw;
      } catch (const std::exception& error) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P DHT profile",
                               forge::exceptions::ctx("error", error.what()));
      }
      if (!protocols.insert(profile.protocol).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P DHT profile protocol IDs must be unique",
                               forge::exceptions::ctx("protocol", profile.protocol.value));
      }
      profiles.push_back(std::move(profile));
   }
   return profiles;
}

[[nodiscard]] forge::net::p2p::topology::mode parse_topology_mode(topology_mode value) {
   switch (value) {
   case topology_mode::managed:
      return forge::net::p2p::topology::mode::managed;
   case topology_mode::static_only:
      return forge::net::p2p::topology::mode::static_only;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P topology mode");
}

[[nodiscard]] forge::net::p2p::rendezvous::role parse_rendezvous_role(rendezvous_role value) {
   switch (value) {
   case rendezvous_role::disabled:
   case rendezvous_role::client:
      return forge::net::p2p::rendezvous::role::client;
   case rendezvous_role::server:
      return forge::net::p2p::rendezvous::role::server;
   case rendezvous_role::client_and_server:
      return forge::net::p2p::rendezvous::role::client_and_server;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P rendezvous role");
}

[[nodiscard]] std::vector<forge::net::p2p::topology::rendezvous_point>
parse_rendezvous_points(const std::vector<rendezvous_point_config>& configured) {
   auto points = std::vector<forge::net::p2p::topology::rendezvous_point>{};
   points.reserve(configured.size());
   for (const auto& item : configured) {
      auto parsed = parse_endpoint_list({item.endpoint});
      points.push_back(forge::net::p2p::topology::rendezvous_point{
          .endpoint = std::move(parsed.front()),
          .namespaces = item.namespaces,
      });
   }
   return points;
}

} // namespace

std::chrono::milliseconds to_ms(std::uint64_t value) {
   return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value)};
}

config decode_config(const forge::config::core::component_view& view) {
   if (view.source().try_get("plugins.p2p.node")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "retired plugin configuration: plugins.p2p.node; use plugins.net.p2p.node");
   }
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, forge::config::core::format_decode_diagnostics(
                                                            "invalid P2P node config", decoded.diagnostics));
   }
   return std::move(decoded.value);
}

parsed_policy parse_policy(const config& config) {
   const auto relay_max_candidates = static_cast<std::size_t>(config.relay_max_candidates);
   return parsed_policy{
       .path = parse_path_policy(config.path_policy, config.relay_client_enabled, relay_max_candidates),
       .relay_client_enabled = config.relay_client_enabled,
       .relay_server_enabled = config.relay_server_enabled,
       .relay_public_allowed = config.relay_public_allowed || config.relay_trust == relay_trust_policy::public_allowed,
       .relay_reservation_ttl = to_ms(config.relay_reservation_ttl_ms),
       .relay_max_candidates = relay_max_candidates,
   };
}

std::vector<forge::net::p2p::endpoint> parse_endpoint_list(const std::vector<std::string>& values) {
   auto out = std::vector<forge::net::p2p::endpoint>{};
   out.reserve(values.size());
   for (const auto& value : values) {
      try {
         out.push_back(forge::net::p2p::parse_endpoint(value));
      } catch (const std::exception& error) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P endpoint",
                               forge::exceptions::ctx("endpoint", value),
                               forge::exceptions::ctx("error", error.what()));
      }
   }
   return out;
}

void apply_config(plugin::impl& state, const config& config) {
   state.policy = parse_policy(config);
   state.api_options = forge::api::transport::options{
       .codec = forge::api::core::codec_id{.value = config.api_codec},
       .max_inflight = static_cast<std::size_t>(config.max_inflight_per_peer),
       .deadline = to_ms(config.api_deadline_ms),
       .max_frame_size = static_cast<std::uint32_t>(config.api_max_frame_size),
   };
   state.options.lifecycle.listen = parse_endpoint_list(config.listen);
   state.options.lifecycle.bootstrap.clear();
   for (auto& endpoint : parse_endpoint_list(config.bootstrap)) {
      state.options.lifecycle.bootstrap.push_back(forge::net::p2p::bootstrap_peer{.address = endpoint.to_multiaddr()});
   }
   state.options.lifecycle.requirement = config.bootstrap_requirement == bootstrap_requirement::require_connection
                                             ? forge::net::p2p::bootstrap_requirement::require_connection
                                             : forge::net::p2p::bootstrap_requirement::allow_disconnected;
   state.options.lifecycle.startup_budget = to_ms(config.bootstrap_startup_budget_ms);
   state.options.lifecycle.connect_timeout = to_ms(config.bootstrap_connect_timeout_ms);
   state.options.lifecycle.max_parallel_bootstrap = static_cast<std::size_t>(config.bootstrap_max_parallel);
   state.options.advertised_endpoints = parse_endpoint_list(config.advertised_endpoints);
   state.peer_store_name = config.peer_store;
   state.reset_incompatible_peer_state = config.peer_store_schema_policy == cache_schema_policy::reset;
   state.options.dht_profiles = parse_dht_profiles(config.dht_profiles);
   state.options.limits.topology = forge::net::p2p::topology::policy{
       .operating_mode = parse_topology_mode(config.topology_mode),
       .peers =
           forge::net::p2p::topology::watermarks{
               .low = static_cast<std::size_t>(config.topology_low),
               .target = static_cast<std::size_t>(config.topology_target),
               .high = static_cast<std::size_t>(config.topology_high),
           },
       .refresh_interval = to_ms(config.topology_refresh_interval_ms),
       .query_timeout = to_ms(config.topology_query_timeout_ms),
       .max_candidates = static_cast<std::size_t>(config.topology_max_candidates),
       .max_parallel_queries = static_cast<std::size_t>(config.topology_max_parallel_queries),
       .max_parallel_dials = static_cast<std::size_t>(config.topology_max_parallel_dials),
       .max_rendezvous_points = static_cast<std::size_t>(config.rendezvous_max_points),
       .max_peer_exchange_peers = static_cast<std::size_t>(config.peer_exchange_max_peers),
       .retry_jitter = config.topology_retry_jitter,
       .dht_enabled = !state.options.dht_profiles.empty(),
       .peer_exchange_enabled = config.peer_exchange_enabled,
       .rendezvous_points = parse_rendezvous_points(config.rendezvous_points),
   };
   if (config.rendezvous_role == rendezvous_role::disabled && !config.rendezvous_points.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "disabled P2P rendezvous cannot configure discovery points");
   }
   if (config.rendezvous_role == rendezvous_role::server && !config.rendezvous_points.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "server-only P2P rendezvous cannot configure client discovery points");
   }
   state.options.limits.rendezvous.operating_role = parse_rendezvous_role(config.rendezvous_role);
   try {
      forge::net::p2p::validate(state.options.limits.topology);
   } catch (const forge::plugins::net::p2p::node::exceptions::invalid_config&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P topology configuration",
                            forge::exceptions::ctx("error", error.what()));
   }
   state.certificate_secret = config.certificate_secret;
   state.private_key_secret = config.private_key_secret;
   state.options.capabilities = forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic};
   if (config.peer_exchange_enabled) {
      state.options.capabilities.add(forge::net::p2p::capabilities::peer_exchange);
   }
   if (config.rendezvous_role == rendezvous_role::server ||
       config.rendezvous_role == rendezvous_role::client_and_server) {
      state.options.capabilities.add(forge::net::p2p::capabilities::rendezvous);
   }
   if (state.policy.relay_client_enabled) {
      state.options.capabilities.add(forge::net::p2p::capabilities::hole_punching);
   }
   if (state.policy.relay_server_enabled) {
      state.options.capabilities.add(forge::net::p2p::capabilities::relay);
      state.options.capabilities.add(forge::net::p2p::capabilities::relay_reservation);
   }
   state.options.path_policy = state.policy.path;
   state.options.relay_policy.client_enabled = state.policy.relay_client_enabled;
   state.options.relay_policy.service_enabled = state.policy.relay_server_enabled;
   state.options.relay_policy.public_relay_allowed = state.policy.relay_public_allowed;
   state.options.relay_policy.max_candidates_per_refresh = state.policy.relay_max_candidates;
   state.options.relay_policy.target_reservations =
       std::min(state.options.relay_policy.target_reservations, state.policy.relay_max_candidates);
   if (state.options.relay_policy.target_reservations == 0) {
      state.options.relay_policy.target_reservations = 1;
   }
   state.options.limits.relay.reservation_ttl = state.policy.relay_reservation_ttl;
   const auto& peer_id = config.peer_id;
   state.options.explicit_peer_id =
       peer_id.empty() ? std::nullopt : std::make_optional(forge::net::p2p::peer_id{.value = peer_id});
   state.options.limits.max_sessions = static_cast<std::size_t>(config.max_sessions);
   state.options.limits.session_low_watermark =
       state.options.limits.max_sessions == 1 ? std::size_t{1} : state.options.limits.max_sessions - 1;
   auto& session_limits = state.options.limits.resources.system;
   const auto admission_limit = with_provisional_session(state.options.limits.max_sessions);
   session_limits.max_connections = admission_limit;
   session_limits.max_inbound_connections = admission_limit;
   session_limits.max_outbound_connections = admission_limit;
   state.options.limits.max_protocol_handlers = static_cast<std::size_t>(config.max_protocol_handlers);
   state.options.allow_insecure_test_mode = config.allow_insecure_test_mode;
   const auto has_identity = !state.certificate_secret.empty() && !state.private_key_secret.empty();
   if (!config.allow_insecure_test_mode && (state.peer_store_name.empty() || !has_identity)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "production P2P node requires peer store and identity secret references");
   }
   if (state.certificate_secret.empty() != state.private_key_secret.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "P2P certificate and private key secret references must be provided together");
   }
}

} // namespace forge::plugins::net::p2p::node
