module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.asio.notification;
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
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.types;
import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.scalar;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/configuration_policy.hxx"
#include "details/plugin_impl.hxx"
#include "details/protocol_registry.hxx"
#include "details/pubsub_policy.hxx"
#include "details/resource_policy.hxx"

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
            profile.protocol = forge::net::p2p::protocol_id{.value = item.protocol};
            profile.operating_mode = mode;
            profile.limits.operating_mode = mode;
            profile.capabilities = {.peers = item.peers, .providers = item.providers, .values = item.values};
            forge::net::p2p::validate_declaration(profile);
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
   try {
      return std::chrono::milliseconds{forge::schema::checked_integral_cast<std::chrono::milliseconds::rep>(value)};
   } catch (const forge::schema::exceptions::invalid_value&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P milliseconds exceed the native duration range");
   }
}

config decode_config(const forge::config::core::component_view& view) {
   for (const auto* retired : {"plugins.net.p2p.pubsub", "plugins.net.p2p.diagnostics",
                              "plugins.p2p.pubsub", "plugins.p2p.diagnostics"}) {
      if (view.source().try_get(retired)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "retired standalone P2P plugin configuration",
                               forge::exceptions::ctx("section", retired));
      }
   }
   if (view.source().try_get("plugins.p2p.node")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "retired plugin configuration: plugins.p2p.node; use plugins.net.p2p.node");
   }
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   auto enabled_path = std::string{view.section()};
   if (!enabled_path.empty()) { enabled_path += '.'; }
   enabled_path += "enabled";
   for (auto& diagnostic : decoded.diagnostics.entries) {
      // Only this exact selector is owned by application_shell, not node policy.
      if (diagnostic.code == "config.unknown" && diagnostic.path != enabled_path) {
         diagnostic.level = forge::schema::severity::error;
      }
   }
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, forge::config::core::format_decode_diagnostics(
                                                            "invalid P2P node config", decoded.diagnostics));
   }
   return std::move(decoded.value);
}

parsed_policy parse_policy(const config& config) {
   const auto relay_max_candidates = static_cast<std::size_t>(config.relay_max_candidates);
   auto policy = parsed_policy{
       .path = parse_path_policy(config.path_policy, config.relay_client_enabled, relay_max_candidates),
       .relay_client_enabled = config.relay_client_enabled,
       .relay_server_enabled = config.relay_server_enabled,
       .relay_public_allowed = config.relay_public_allowed || config.relay_trust == relay_trust_policy::public_allowed,
       .relay_reservation_ttl = to_ms(config.relay_reservation_ttl_ms),
       .relay_max_candidates = relay_max_candidates,
   };
   apply_path(policy.path, config.path_max_direct_endpoints);
   return policy;
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
   const auto previous = state.configuration_snapshot();
   detail::validate_pubsub_config(config.pubsub);
   if (state.phase.load(std::memory_order_acquire) != lifecycle_phase::idle) {
      state.commit_diagnostics_configuration(previous, config);
      return;
   }
   auto candidate = std::make_shared<detail::configuration_snapshot>(*previous);
   candidate->declarative = config;
   auto policy = parse_policy(config);
   auto options = forge::net::p2p::node::options{};
   auto api_options = forge::api::transport::options{
       .codec = forge::api::core::codec_id{.value = config.api_codec},
       .max_inflight = static_cast<std::size_t>(config.max_inflight_per_peer),
       .deadline = to_ms(config.api_deadline_ms),
       .max_frame_size = static_cast<std::uint32_t>(config.api_max_frame_size),
   };
   options.lifecycle.listen = parse_endpoint_list(config.listen);
   options.lifecycle.bootstrap.clear();
   for (auto& endpoint : parse_endpoint_list(config.bootstrap)) {
      options.lifecycle.bootstrap.push_back(forge::net::p2p::bootstrap_peer{.address = endpoint.to_multiaddr()});
   }
   options.lifecycle.requirement = config.bootstrap_requirement == bootstrap_requirement::require_connection
                                             ? forge::net::p2p::bootstrap_requirement::require_connection
                                             : forge::net::p2p::bootstrap_requirement::allow_disconnected;
   options.lifecycle.startup_budget = to_ms(config.bootstrap_startup_budget_ms);
   options.lifecycle.connect_timeout = to_ms(config.bootstrap_connect_timeout_ms);
   options.lifecycle.max_parallel_bootstrap = static_cast<std::size_t>(config.bootstrap_max_parallel);
   options.advertised_endpoints = parse_endpoint_list(config.advertised_endpoints);
   auto peer_store_name = config.peer_store;
   auto reset_incompatible_peer_state = config.peer_store_schema_policy == cache_schema_policy::reset;
   auto profiles = parse_dht_profiles(config.dht_profiles);
   for (const auto& prior : previous->dht_profiles) {
      if (prior.kind != forge::net::p2p::dht::profile_kind::custom || prior.value_policies.empty()) { continue; }
      const auto found = std::find_if(profiles.begin(), profiles.end(),
                                      [&](const auto& profile) { return profile.protocol == prior.protocol; });
      if (found == profiles.end() || found->kind != prior.kind || !found->capabilities.values) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "configuration cannot discard a DHT value contribution");
      }
      found->value_policies = prior.value_policies;
   }
   options.limits.topology = forge::net::p2p::topology::policy{
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
       .dht_enabled = !profiles.empty(),
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
   options.limits.rendezvous.operating_role = parse_rendezvous_role(config.rendezvous_role);
   try {
      forge::net::p2p::validate(options.limits.topology);
   } catch (const forge::plugins::net::p2p::node::exceptions::invalid_config&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P topology configuration",
                            forge::exceptions::ctx("error", error.what()));
   }
   auto certificate_secret = config.certificate_secret;
   auto private_key_secret = config.private_key_secret;
   options.capabilities = forge::net::p2p::capability_set{
       .bits = config.private_network ? 0U : forge::net::p2p::capabilities::direct_quic};
   if (config.peer_exchange_enabled) {
      options.capabilities.add(forge::net::p2p::capabilities::peer_exchange);
   }
   if (config.rendezvous_role == rendezvous_role::server ||
       config.rendezvous_role == rendezvous_role::client_and_server) {
      options.capabilities.add(forge::net::p2p::capabilities::rendezvous);
   }
   if (policy.relay_client_enabled) {
      options.capabilities.add(forge::net::p2p::capabilities::hole_punching);
   }
   if (policy.relay_server_enabled) {
      options.capabilities.add(forge::net::p2p::capabilities::relay);
      options.capabilities.add(forge::net::p2p::capabilities::relay_reservation);
   }
   options.path_policy = policy.path;
   options.relay_policy.client_enabled = policy.relay_client_enabled;
   options.relay_policy.service_enabled = policy.relay_server_enabled;
   options.relay_policy.auto_discovery_enabled = config.relay_auto_discovery_enabled;
   options.relay_policy.public_relay_allowed = policy.relay_public_allowed;
   options.relay_policy.max_candidates_per_refresh = policy.relay_max_candidates;
   options.relay_policy.target_reservations =
       std::min(options.relay_policy.target_reservations, policy.relay_max_candidates);
   if (options.relay_policy.target_reservations == 0) {
      options.relay_policy.target_reservations = 1;
   }
   options.limits.relay.reservation_ttl = policy.relay_reservation_ttl;
   const auto& peer_id = config.peer_id;
   options.explicit_peer_id =
       peer_id.empty() ? std::nullopt : std::make_optional(forge::net::p2p::peer_id{.value = peer_id});
   options.limits.max_sessions = static_cast<std::size_t>(config.max_sessions);
   options.limits.session_low_watermark =
       options.limits.max_sessions == 1 ? std::size_t{1} : options.limits.max_sessions - 1;
   auto& session_limits = options.limits.resources.system;
   const auto admission_limit = with_provisional_session(options.limits.max_sessions);
   session_limits.max_connections = admission_limit;
   session_limits.max_inbound_connections = admission_limit;
   session_limits.max_outbound_connections = admission_limit;
   detail::apply_resource_config(options.limits.resources, config.resources);
   apply_connections(options.limits, config.connections);
   apply_identify(options, config.identify);
   apply_peer_state(options.peer_state, config.peer_state);
   apply_tcp(options, config.tcp);
   apply_quic(options, config.quic);
   apply_private_network(options, config);
   apply_dns(options, config.dns);
   apply_dial(options, config.dial);
   options.limits.max_protocol_handlers = static_cast<std::size_t>(config.max_protocol_handlers);
   options.allow_insecure_test_mode = config.allow_insecure_test_mode;
   const auto has_identity = !certificate_secret.empty() && !private_key_secret.empty();
   if (!config.allow_insecure_test_mode && (peer_store_name.empty() || !has_identity)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "production P2P node requires peer store and identity secret references");
   }
   if (certificate_secret.empty() != private_key_secret.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "P2P certificate and private key secret references must be provided together");
   }
   options.connection_gater = previous->options.connection_gater;
   options.limits.pubsub = detail::pubsub_options_for(config.pubsub);
   if (previous->pubsub_contributed) {
      if (!config.pubsub.enabled) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "configuration cannot discard a PubSub contribution");
      }
      options.limits.pubsub = previous->options.limits.pubsub;
   }
   if (config.pubsub.enabled) {
      options.capabilities.add(forge::net::p2p::capabilities::pubsub);
   }
   candidate->pubsub = config.pubsub;
   candidate->diagnostics = config.diagnostics;
   candidate->options = std::move(options);
   candidate->api_options = std::move(api_options);
   candidate->policy = std::move(policy);
   candidate->dht_profiles = std::move(profiles);
   candidate->peer_store_name = std::move(peer_store_name);
   candidate->certificate_secret = std::move(certificate_secret);
   candidate->private_key_secret = std::move(private_key_secret);
   candidate->reset_incompatible_peer_state = reset_incompatible_peer_state;
   state.commit_configuration(previous, std::move(candidate));
}

} // namespace forge::plugins::net::p2p::node
