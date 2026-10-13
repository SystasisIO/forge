module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.config.core.component;
import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.p2p.endpoint;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.rendezvous;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

#include "details/configuration_policy.hxx"

namespace forge::plugins::net::p2p::node {

void apply_lifecycle(forge::net::p2p::node::options& options, const config& value) {
   auto& lifecycle = options.lifecycle;
   const auto& policy = value.bootstrap_policy;
   lifecycle.listen = parse_endpoint_list(value.listen);
   lifecycle.bootstrap.clear();
   lifecycle.bootstrap.reserve(value.bootstrap.size());
   for (const auto& address : value.bootstrap) {
      try {
         lifecycle.bootstrap.push_back(forge::net::p2p::bootstrap_peer{
             .address = forge::multiformats::multiaddr::parse(address),
         });
      } catch (const std::exception& error) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P bootstrap multiaddr",
                               forge::exceptions::ctx("error", error.what()));
      }
   }
   switch (policy.requirement) {
   case bootstrap_requirement::allow_disconnected:
      lifecycle.requirement = forge::net::p2p::bootstrap_requirement::allow_disconnected;
      break;
   case bootstrap_requirement::require_connection:
      lifecycle.requirement = forge::net::p2p::bootstrap_requirement::require_connection;
      break;
   default: FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P bootstrap requirement");
   }
   lifecycle.startup_budget = to_ms(policy.startup_budget_ms);
   lifecycle.connect_timeout = to_ms(policy.connect_timeout_ms);
   apply_size(lifecycle.max_parallel_bootstrap, policy.max_parallel, "bootstrap-policy.max-parallel");
   apply_time(lifecycle.bootstrap_retry_initial_delay, policy.retry_initial_delay_ms);
   apply_time(lifecycle.bootstrap_retry_max_delay, policy.retry_max_delay_ms);
   if (policy.retry_jitter) { lifecycle.bootstrap_retry_jitter = *policy.retry_jitter; }
   apply_time(lifecycle.maintenance_interval, policy.maintenance_interval_ms);
}

void apply_topology(forge::net::p2p::node::options& options, const config& value, bool dht_enabled) {
   auto& topology = options.limits.topology;
   const auto& configured = value.topology;
   switch (configured.mode) {
   case topology_mode::managed: topology.operating_mode = forge::net::p2p::topology::mode::managed; break;
   case topology_mode::static_only: topology.operating_mode = forge::net::p2p::topology::mode::static_only; break;
   default: FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P topology mode");
   }
   apply_size(topology.peers.low, configured.peers.low, "topology.peers.low");
   apply_size(topology.peers.target, configured.peers.target, "topology.peers.target");
   apply_size(topology.peers.high, configured.peers.high, "topology.peers.high");
   topology.refresh_interval = to_ms(configured.refresh_interval_ms);
   topology.query_timeout = to_ms(configured.query_timeout_ms);
   apply_size(topology.max_candidates, configured.max_candidates, "topology.max-candidates");
   apply_size(topology.max_parallel_queries, configured.max_parallel_queries, "topology.max-parallel-queries");
   apply_size(topology.max_parallel_dials, configured.max_parallel_dials, "topology.max-parallel-dials");
   topology.retry_jitter = configured.retry_jitter;
   apply_size(topology.max_tagged_peers, configured.max_tagged_peers, "topology.max-tagged-peers");
   apply_size(topology.max_tags_per_peer, configured.max_tags_per_peer, "topology.max-tags-per-peer");
   apply_size(topology.max_tag_size, configured.max_tag_size, "topology.max-tag-size");
   topology.dht_enabled = dht_enabled;

   const auto& rendezvous = value.rendezvous;
   if ((rendezvous.role == rendezvous_role::disabled || rendezvous.role == rendezvous_role::server) &&
       !rendezvous.points.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "disabled or server-only P2P rendezvous cannot configure client discovery points");
   }
   switch (rendezvous.role) {
   case rendezvous_role::disabled:
   case rendezvous_role::client:
      options.limits.rendezvous.operating_role = forge::net::p2p::rendezvous::role::client;
      break;
   case rendezvous_role::server:
      options.limits.rendezvous.operating_role = forge::net::p2p::rendezvous::role::server;
      break;
   case rendezvous_role::client_and_server:
      options.limits.rendezvous.operating_role = forge::net::p2p::rendezvous::role::client_and_server;
      break;
   default: FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P rendezvous role");
   }
   topology.rendezvous_points.clear();
   topology.rendezvous_points.reserve(rendezvous.points.size());
   for (const auto& point : rendezvous.points) {
      auto endpoints = parse_endpoint_list({point.endpoint});
      topology.rendezvous_points.push_back({.endpoint = std::move(endpoints.front()), .namespaces = point.namespaces});
   }
   apply_size(topology.max_rendezvous_points, rendezvous.max_points, "rendezvous.max-points");
   apply_size(topology.max_rendezvous_namespaces, rendezvous.max_namespaces, "rendezvous.max-namespaces");
   auto& limits = options.limits.rendezvous;
   if (rendezvous.default_ttl_seconds) { limits.default_ttl = to_seconds(*rendezvous.default_ttl_seconds); }
   if (rendezvous.min_ttl_seconds) { limits.min_ttl = to_seconds(*rendezvous.min_ttl_seconds); }
   if (rendezvous.max_ttl_seconds) { limits.max_ttl = to_seconds(*rendezvous.max_ttl_seconds); }
   apply_size(limits.max_namespace_size, rendezvous.max_namespace_size, "rendezvous.max-namespace-size");
   apply_size(limits.max_registrations_per_peer, rendezvous.max_registrations_per_peer,
              "rendezvous.max-registrations-per-peer");
   apply_size(limits.max_discover_limit, rendezvous.max_discover_limit, "rendezvous.max-discover-limit");
   apply_size(limits.max_message_size, rendezvous.max_message_size, "rendezvous.max-message-size");

   const auto& pex = value.peer_exchange;
   topology.peer_exchange_enabled = pex.enabled;
   apply_size(topology.max_peer_exchange_peers, pex.max_peers, "peer-exchange.max-peers");
   apply_size(options.limits.max_peer_exchange_message_size, pex.max_message_size, "peer-exchange.max-message-size");
   apply_size(options.limits.max_peer_exchange_records, pex.max_records, "peer-exchange.max-records");
   apply_size(options.limits.max_peer_exchange_queue, pex.max_queue, "peer-exchange.max-queue");
}

} // namespace forge::plugins::net::p2p::node
