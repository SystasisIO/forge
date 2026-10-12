module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.multiformats.multiaddr;
import forge.net.dns.resolver;
import forge.net.p2p.dht;
import forge.net.p2p.dialing;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.quic.connection;
import forge.net.quic.connector;
import forge.net.quic.exceptions;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.tcp.connection;
import forge.net.transport.limits;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/dial_scheduler.hxx"
#include "details/node_impl.hxx"
#include "details/quic_profile.hxx"
#include "details/stream_upgrade.hxx"

namespace forge::net::p2p {

void validate_policy(const node::options& options) {
   auto topology_policy = options.limits.topology;
   normalize_legacy_discovery(options.limits.discovery, topology_policy);
   validate(options.peer_state);
   pubsub::validate(options.limits.pubsub);
   if (!options.private_network) {
      try {
         direct::quic_profile::validate_policy(options.transport_limits);
      } catch (const forge::net::quic::exceptions::invalid_options& error) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, error.what());
      }
   }
   if (options.explicit_peer_id && !valid_peer_id(*options.explicit_peer_id)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid explicit P2P peer id");
   }
   static_cast<void>(detail::stream_security_protocols(options.stream_security));
   if (options.stream_security == node::stream_security::tls && options.relay_policy.client_enabled) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS-only stream security does not support relay clients");
   }
   validate(options.mdns);
   if (options.mdns.enabled && topology_policy.operating_mode == topology::mode::static_only) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS requires managed topology");
   }
   validate(options.reachability_policy);
   detail::dns_address_expander::validate_policy(options.dns_resolution);
   detail::dial_scheduler::validate_policy(detail::dial_scheduler_policy(
       options.dns_resolution, options.direct_dial, options.private_network.has_value()));
   try {
      forge::net::dns::validate(options.dns_resolver);
   } catch (const forge::net::dns::exceptions::invalid_options& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "invalid P2P DNS resolver options: " + std::string{error.what()});
   }
   if (options.private_network) {
      private_network::validate_policy(*options.private_network);
      if (options.capabilities.has(capabilities::direct_quic) || options.capabilities.has(capabilities::relay) ||
          options.capabilities.has(capabilities::relay_reservation) ||
          options.capabilities.has(capabilities::hole_punching)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P private-network profile permits only direct TCP transport");
      }
      if (options.relay_policy.service_enabled || options.relay_policy.client_enabled ||
          options.relay_policy.auto_discovery_enabled) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P private-network profile requires every relay policy to be disabled");
      }
      if (!options.path_policy.allow_direct || options.path_policy.allow_relay || options.path_policy.allow_hole_punch) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P private-network profile requires a direct-only path policy");
      }
      const auto require_direct_tcp = [](const endpoint& value, std::string_view use) {
         if (!value.is_direct_tcp()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                  "P2P private-network " + std::string{use} + " must be a direct TCP endpoint");
         }
      };
      for (const auto& value : options.advertised_endpoints) {
         require_direct_tcp(value, "advertised endpoint");
      }
      for (const auto& value : options.lifecycle.listen) {
         require_direct_tcp(value, "lifecycle listener");
      }
      for (const auto& point : topology_policy.rendezvous_points) {
         require_direct_tcp(point.endpoint, "rendezvous point");
      }
   }
   const auto relay_duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration);
   // Milliseconds can fit their own representation but overflow the clocks at use.
   constexpr auto maximum_clock_delay = std::min(
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()),
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::duration::max()));
   for (const auto delay : {options.limits.session_grace_period, options.limits.session_prune_silence,
                            options.limits.dial_backoff_base, options.limits.dial_backoff_step,
                            options.limits.dial_backoff_max, options.identify.timeout}) {
      if (delay > maximum_clock_delay) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P connection or Identify duration exceeds the native clock range");
      }
   }
   validate(topology_policy);
   for (const auto& point : topology_policy.rendezvous_points) {
      for (const auto& namespace_name : point.namespaces) {
         if (namespace_name.size() > options.limits.rendezvous.max_namespace_size) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                  "P2P topology rendezvous namespace exceeds the configured limit");
         }
      }
   }
   if (options.limits.max_sessions == 0 || options.limits.session_low_watermark == 0 ||
       options.limits.session_low_watermark > options.limits.max_sessions ||
       options.limits.session_grace_period.count() < 0 || options.limits.session_prune_silence.count() <= 0 ||
       options.limits.dial_backoff_base.count() <= 0 || options.limits.dial_backoff_step.count() <= 0 ||
       options.limits.dial_backoff_max.count() <= 0 ||
       options.limits.dial_backoff_base > options.limits.dial_backoff_max ||
       options.limits.max_protocol_handlers == 0 ||
       options.limits.max_peer_exchange_message_size < peer_exchange_codec::minimum_message_size ||
       options.limits.max_peer_exchange_message_size > std::numeric_limits<std::uint32_t>::max() ||
       options.limits.max_peer_exchange_records == 0 ||
       options.limits.max_peer_exchange_records > std::numeric_limits<std::uint32_t>::max() ||
       options.limits.max_peer_exchange_queue == 0 || options.limits.relay.max_active_relays == 0 ||
       options.limits.relay.max_reservations == 0 || options.limits.relay.max_streams_per_reservation == 0 ||
       options.limits.relay.max_relay_bytes == 0 || options.limits.relay.max_queued_bytes == 0 ||
       relay_duration.count() <= 0 ||
       std::chrono::duration_cast<std::chrono::milliseconds>(relay_duration) != options.limits.relay.max_duration ||
       options.limits.relay.reservation_ttl.count() <= 0 || options.limits.resources.max_relay_reservations == 0 ||
       options.limits.resources.max_dial_attempts == 0 || options.limits.resources.max_dial_attempts_per_peer == 0 ||
       options.limits.resources.max_malformed_messages_per_peer == 0 ||
       options.limits.rendezvous.default_ttl.count() <= 0 || options.limits.rendezvous.min_ttl.count() <= 0 ||
       options.limits.rendezvous.max_ttl.count() <= 0 ||
       options.limits.rendezvous.min_ttl > options.limits.rendezvous.max_ttl ||
       options.limits.rendezvous.max_namespace_size == 0 || options.limits.rendezvous.max_registrations_per_peer == 0 ||
       options.limits.rendezvous.max_discover_limit == 0 || options.limits.rendezvous.max_message_size == 0 ||
       options.limits.pubsub.limits.validation_retry_initial_delay.count() <= 0 ||
       options.limits.pubsub.limits.validation_retry_max_delay <
           options.limits.pubsub.limits.validation_retry_initial_delay ||
       options.limits.pubsub.limits.max_validation_attempts == 0 ||
       options.limits.pubsub.limits.max_validation_redeliveries == 0 ||
       options.limits.pubsub.limits.max_validation_requests == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P node limits");
   }
   if (!options.path_policy.allow_direct && !options.path_policy.allow_hole_punch && !options.path_policy.allow_relay) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P path policy must allow at least one path kind");
   }
   if (options.path_policy.max_direct_endpoints == 0 || options.path_policy.max_relay_candidates == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P path policy limits must be positive");
   }
   if (options.relay_policy.target_reservations == 0 || options.relay_policy.refresh_margin.count() <= 0 ||
       options.relay_policy.max_candidates_per_refresh == 0 || options.relay_policy.max_parallel_reservations == 0 ||
       options.relay_policy.candidate_backoff.count() <= 0 ||
       options.relay_policy.max_candidates_per_refresh > 4096 ||
       options.relay_policy.target_reservations > options.relay_policy.max_candidates_per_refresh ||
       options.relay_policy.target_reservations > options.limits.relay.max_reservations ||
       options.relay_policy.max_parallel_reservations > options.relay_policy.max_candidates_per_refresh ||
       options.relay_policy.refresh_margin > std::chrono::hours{24} ||
       options.relay_policy.candidate_backoff > std::chrono::hours{24}) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P AutoRelay policy limits must be positive");
   }
   const auto& relay_limits = options.limits.relay;
   if (relay_limits.max_reservations_per_ip == 0 || relay_limits.max_circuits_per_peer == 0 ||
       relay_limits.max_service_requests == 0 || relay_limits.max_service_requests > 65'536 ||
       relay_limits.max_service_requests_per_peer == 0 || relay_limits.max_service_requests_per_ip == 0 ||
       relay_limits.service_request_window.count() <= 0 || relay_limits.service_request_window > std::chrono::hours{1} ||
       relay_limits.handshake_timeout.count() <= 0 || relay_limits.handshake_timeout > std::chrono::minutes{5}) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P relay service admission limits are invalid");
   }
   const auto& lifecycle = options.lifecycle;
   if (lifecycle.listen.size() > 1'024 || lifecycle.startup_budget.count() <= 0 ||
       lifecycle.startup_budget > std::chrono::minutes{10} || lifecycle.max_parallel_bootstrap == 0 ||
       lifecycle.max_parallel_bootstrap > 256 || lifecycle.connect_timeout.count() <= 0 ||
       lifecycle.connect_timeout > std::chrono::minutes{5} || lifecycle.bootstrap_retry_initial_delay.count() <= 0 ||
       lifecycle.bootstrap_retry_max_delay < lifecycle.bootstrap_retry_initial_delay ||
       lifecycle.bootstrap_retry_max_delay > std::chrono::hours{1} ||
       !std::isfinite(lifecycle.bootstrap_retry_jitter) || lifecycle.bootstrap_retry_jitter < 0.0 ||
       lifecycle.bootstrap_retry_jitter > 0.20 || lifecycle.maintenance_interval.count() <= 0 ||
       lifecycle.maintenance_interval > std::chrono::minutes{10}) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P node lifecycle options");
   }
   validate_bootstrap(lifecycle.bootstrap, lifecycle.requirement == bootstrap_requirement::require_connection,
                      options.dns_resolution, options.private_network.has_value());
   constexpr auto max_dht_profiles = std::size_t{64};
   if (options.dht_profiles.size() > max_dht_profiles) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P DHT profile count exceeds the supported limit");
   }
   auto dht_protocols = std::set<protocol_id>{};
   for (const auto& profile : options.dht_profiles) {
      forge::net::p2p::validate(profile);
      if (!dht_protocols.insert(profile.protocol).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P DHT profile protocol IDs must be unique");
      }
   }
   for (const auto& [protocol, persistence] : options.dht_record_persistence) {
      if (!dht_protocols.contains(protocol)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P DHT persistence must reference one configured profile");
      }
   }
   const auto& identify = options.identify;
   if (identify.timeout.count() <= 0 || identify.max_message_size == 0 || identify.max_total_message_size == 0 ||
       identify.max_own_message_size == 0 || identify.max_own_message_size > identify.max_message_size ||
       identify.max_message_parts == 0 ||
       identify.max_message_size > (std::numeric_limits<std::size_t>::max)() / identify.max_message_parts ||
       identify.max_total_message_size < identify.max_message_size ||
       identify.max_total_message_size > identify.max_message_size * identify.max_message_parts ||
       identify.max_protocols == 0 || identify.max_listen_endpoints == 0 || identify.max_protocol_size == 0 ||
       identify.max_version_size == 0 || identify.max_public_key_size == 0 ||
       identify.max_signed_peer_record_size == 0 || identify.max_signed_peer_record_size > identify.max_message_size ||
       identify.max_push_operations == 0 || options.protocol_version.size() > identify.max_version_size ||
       options.agent_version.size() > identify.max_version_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P Identify limits");
   }
}

void validate(const node::options& options) {
   validate_policy(options);
   if (options.private_network) {
      private_network::validate(*options.private_network);
   }
   if (!options.allow_insecure_test_mode && (options.certificate_pem.empty() || options.private_key_pem.empty())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "production P2P node requires mTLS certificate and private key");
   }
   if (options.certificate_pem.empty() != options.private_key_pem.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P certificate and private key must be provided together");
   }
   if (!options.allow_insecure_test_mode && options.explicit_peer_id && !options.certificate_pem.empty() &&
       *options.explicit_peer_id != make_peer_id_from_certificate_pem(options.certificate_pem)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity,
                            "explicit P2P peer id does not match the configured certificate");
   }
   if (options.allow_insecure_test_mode && options.certificate_pem.empty() && !options.explicit_peer_id) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "insecure P2P test node without certificate requires explicit peer id");
   }
   if (!options.allow_insecure_test_mode && !options.peer_state.persistence) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "production P2P node requires peer persistence");
   }
   for (const auto& profile : options.dht_profiles) {
      if (!options.allow_insecure_test_mode) {
         const auto persistence = options.dht_record_persistence.find(profile.protocol);
         if (persistence == options.dht_record_persistence.end() || !persistence->second) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                  "production P2P DHT profile requires durable record persistence");
         }
      }
   }
   for (const auto& [protocol, persistence] : options.dht_record_persistence) {
      if (!persistence) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P DHT persistence must reference one configured profile");
      }
   }
}

} // namespace forge::net::p2p
