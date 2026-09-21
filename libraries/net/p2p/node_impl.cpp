module;

#include "details/rendezvous_time.hxx"

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
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
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.symmetric.chacha20_poly1305;
import forge.crypto.pki.der;
import forge.crypto.asymmetric.ed25519;
import forge.crypto.digest.hmac;
import forge.crypto.asymmetric;
import forge.multiformats.multiaddr;
import forge.net.dns.resolver;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.exceptions;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.private_network;
import forge.net.p2p.reachability;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.quic.exceptions;
import forge.crypto.core.random;
import forge.crypto.asymmetric.rsa;
import forge.crypto.asymmetric.x25519;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.exceptions;
import forge.net.transport.exceptions;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.exceptions;
import forge.net.yamux.session;

#include "details/lifecycle_wakeup.hxx"
#include "details/cancellation_latch.hxx"
#include "details/dial_scheduler.hxx"
#include "details/node_impl.hxx"
#include "details/owner_cancellation.hxx"
#include "details/peer_exchange_learning.hxx"
#include "details/protocol_capabilities.hxx"
#include "details/session_lifecycle.hxx"
#include "details/topology_peer_exchange_claims.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] exceptions::code map_transport_error(forge::net::transport::exceptions::code kind) noexcept {
   using transport_kind = forge::net::transport::exceptions::code;
   switch (kind) {
   case transport_kind::invalid_endpoint:
      return exceptions::code::invalid_options;
   case transport_kind::closed:
      return exceptions::code::closed;
   case transport_kind::canceled:
      return exceptions::code::canceled;
   case transport_kind::frame_too_large:
   case transport_kind::protocol_error:
   case transport_kind::invalid_buffer:
      return exceptions::code::codec_error;
   case transport_kind::unsupported_protocol:
      return exceptions::code::unsupported_protocol;
   case transport_kind::duplicate_registration:
      return exceptions::code::invalid_options;
   }
   return exceptions::code::internal;
}

[[nodiscard]] exceptions::code map_quic_error(forge::net::quic::exceptions::code kind) noexcept {
   using quic_kind = forge::net::quic::exceptions::code;
   switch (kind) {
   case quic_kind::invalid_endpoint:
   case quic_kind::invalid_options:
      return exceptions::code::invalid_options;
   case quic_kind::connect_timeout:
   case quic_kind::handshake_timeout:
   case quic_kind::idle_timeout:
      return exceptions::code::timeout;
   case quic_kind::tls_failed:
   case quic_kind::peer_verification_failed:
      return exceptions::code::peer_verification_failed;
   case quic_kind::alpn_mismatch:
   case quic_kind::unsupported:
      return exceptions::code::unsupported_protocol;
   case quic_kind::frame_too_large:
   case quic_kind::malformed_frame:
      return exceptions::code::codec_error;
   case quic_kind::backpressure_rejected:
      return exceptions::code::backpressure_rejected;
   case quic_kind::connection_rejected:
      return exceptions::code::connection_rejected;
   case quic_kind::connection_closed:
   case quic_kind::stream_closed:
   case quic_kind::stream_reset:
      return exceptions::code::closed;
   case quic_kind::canceled:
      return exceptions::code::canceled;
   case quic_kind::dependency_unavailable:
   case quic_kind::internal:
      return exceptions::code::internal;
   }
   return exceptions::code::internal;
}

[[nodiscard]] exceptions::code map_yamux_error(forge::net::yamux::exceptions::code kind) noexcept {
   using yamux_kind = forge::net::yamux::exceptions::code;
   switch (kind) {
   case yamux_kind::invalid_options:
      return exceptions::code::invalid_options;
   case yamux_kind::protocol_error:
      return exceptions::code::protocol_error;
   case yamux_kind::resource_limit:
      return exceptions::code::backpressure_rejected;
   case yamux_kind::stream_reset:
   case yamux_kind::closed:
      return exceptions::code::closed;
   case yamux_kind::canceled:
      return exceptions::code::canceled;
   }
   return exceptions::code::internal;
}

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error) {
   const auto code = exceptions::code_of(error);
   if (code) {
      return *code;
   }
   const auto transport_code = forge::net::transport::exceptions::code_of(error);
   if (transport_code) {
      return map_transport_error(*transport_code);
   }
   if (const auto quic_code = forge::net::quic::exceptions::code_of(error)) {
      return map_quic_error(*quic_code);
   }
   if (const auto yamux_code = forge::net::yamux::exceptions::code_of(error)) {
      return map_yamux_error(*yamux_code);
   }
   return exceptions::code::internal;
}

[[noreturn]] void rethrow_transport_as_p2p(const forge::exceptions::base& error) {
   FORGE_THROW_CODE(p2p_code(error), error.what());
}

[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept {
   return exceptions::is(error, exceptions::code::closed) || exceptions::is(error, exceptions::code::canceled) ||
          forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::closed) ||
          forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::canceled) ||
          forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::connection_closed) ||
          forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::stream_closed) ||
          forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::stream_reset) ||
          forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::canceled) ||
          forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::stream_reset) ||
          forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed) ||
          forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::canceled);
}

[[nodiscard]] bool is_clean_stream_eof(const forge::exceptions::base& error) noexcept {
   return exceptions::is(error, exceptions::code::closed) ||
          forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::closed) ||
          forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::stream_closed) ||
          forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed);
}

[[nodiscard]] std::uint64_t random_nonce() {
   const auto bytes = forge::crypto::core::random_bytes(8);
   auto out = std::uint64_t{};
   for (auto byte : bytes) {
      out = (out << 8U) | byte;
   }
   return out == 0 ? 1 : out;
}

[[nodiscard]] std::vector<std::uint8_t> wrap_length_delimited(std::span<const std::uint8_t> payload) {
   auto out = forge::multiformats::varint_encode(payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> unwrap_length_delimited(std::span<const std::uint8_t> bytes,
                                                                std::size_t max_payload_size) {
   auto decoded = forge::multiformats::decoded_varint{};
   try {
      decoded = forge::multiformats::varint_decode(bytes);
   } catch (const forge::multiformats::exceptions::invalid_format& error) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, error.what());
   }
   if (decoded.value > max_payload_size) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p protobuf message exceeds max size");
   }
   const auto total = decoded.size + static_cast<std::size_t>(decoded.value);
   if (total != bytes.size()) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p protobuf message length mismatch");
   }
   return {bytes.begin() + static_cast<std::ptrdiff_t>(decoded.size), bytes.end()};
}

[[nodiscard]] peer_exchange_codec::options codec_for(const node::options& options) noexcept {
   return peer_exchange_codec::options{
       .max_message_size = static_cast<std::uint32_t>(options.limits.max_peer_exchange_message_size),
       .max_endpoint_records = static_cast<std::uint32_t>(options.limits.max_peer_exchange_records),
   };
}

void validate_operation_timeout(std::chrono::milliseconds timeout, std::string_view name) {
   if (timeout.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::string{name} + " must be positive");
   }
}

[[nodiscard]] std::chrono::milliseconds remaining_timeout(std::chrono::steady_clock::time_point started,
                                                          std::chrono::milliseconds timeout,
                                                          std::string_view operation) {
   validate_operation_timeout(timeout, operation);
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   if (elapsed >= timeout) {
      FORGE_THROW_EXCEPTION(exceptions::timeout, std::string{operation} + " timed out");
   }
   return timeout - elapsed;
}

[[nodiscard]] std::chrono::milliseconds
attempt_timeout(std::chrono::milliseconds remaining, std::chrono::milliseconds configured, std::string_view operation) {
   validate_operation_timeout(remaining, operation);
   validate_operation_timeout(configured, operation);
   return std::min(remaining, configured);
}

[[noreturn]] void throw_operation_timeout(std::string_view operation) {
   FORGE_THROW_EXCEPTION(exceptions::timeout, std::string{operation} + " timed out");
}

resource_manager::limits resource_limits_for(const node::limits& limits) {
   return limits.resources;
}

void normalize_legacy_discovery(node::options& options) {
   const auto defaults = discovery::policy{};
   const auto& legacy = options.limits.discovery;
   auto& topology_policy = options.limits.topology;
   const auto topology_defaults = topology::policy{};

   if (legacy.query_timeout.count() <= 0 || legacy.refresh_interval.count() <= 0 || legacy.max_parallel_queries == 0 ||
       legacy.max_results == 0 ||
       std::ranges::any_of(legacy.rendezvous_namespaces, [](const auto& value) { return value.empty(); })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid legacy P2P discovery policy");
   }

   const auto map_value = [](const auto& legacy_value, const auto& legacy_default, auto& topology_value,
                             const auto& topology_default, std::string_view name) {
      if (legacy_value == legacy_default) {
         return;
      }
      if (topology_value != topology_default && topology_value != legacy_value) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               std::string{"conflicting legacy discovery and topology "} + std::string{name});
      }
      topology_value = legacy_value;
   };

   if (legacy.enabled != defaults.enabled) {
      const auto operating_mode = legacy.enabled ? topology::mode::managed : topology::mode::static_only;
      if (topology_policy.operating_mode != topology_defaults.operating_mode &&
          topology_policy.operating_mode != operating_mode) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "conflicting legacy discovery and topology mode");
      }
      topology_policy.operating_mode = operating_mode;
   }
   map_value(legacy.dht_enabled, defaults.dht_enabled, topology_policy.dht_enabled, topology_defaults.dht_enabled,
             "DHT setting");
   map_value(legacy.rendezvous_enabled, defaults.rendezvous_enabled, topology_policy.rendezvous_enabled,
             topology_defaults.rendezvous_enabled, "Rendezvous setting");
   map_value(legacy.query_timeout, defaults.query_timeout, topology_policy.query_timeout,
             topology_defaults.query_timeout, "query timeout");
   map_value(legacy.refresh_interval, defaults.refresh_interval, topology_policy.refresh_interval,
             topology_defaults.refresh_interval, "refresh interval");
   map_value(legacy.max_parallel_queries, defaults.max_parallel_queries, topology_policy.max_parallel_queries,
             topology_defaults.max_parallel_queries, "query concurrency");
   map_value(legacy.max_results, defaults.max_results, topology_policy.max_candidates, topology_defaults.max_candidates,
             "candidate limit");

   if (legacy.rendezvous_namespaces != defaults.rendezvous_namespaces) {
      if (topology_policy.rendezvous_points.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "legacy P2P discovery namespaces require configured topology Rendezvous points");
      }
      for (const auto& point : topology_policy.rendezvous_points) {
         if (point.namespaces != legacy.rendezvous_namespaces) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                  "conflicting legacy discovery and topology Rendezvous namespaces");
         }
      }
   }
}

void normalize_topology_capacity(node::options& options) noexcept {
   auto& policy = options.limits.topology;
   if (policy.operating_mode == topology::mode::static_only) {
      return;
   }
   policy.peers.target = std::min(policy.peers.target, options.limits.max_sessions);
   policy.peers.low = std::min(policy.peers.low, policy.peers.target);
}

[[nodiscard]] detail::dial_scheduler::policy dial_scheduler_policy(const node::options& options) {
   auto black_holes = options.direct_dial.black_holes;
   if (options.private_network) {
      // Private networks are direct-TCP-only, but IPv6 reachability detection remains useful.
      black_holes.udp_enabled = false;
   }
   return detail::dial_scheduler::policy{
       .resolution = options.dns_resolution,
       .ranker = options.direct_dial.ranker,
       .black_holes = std::move(black_holes),
       .max_concurrent_attempts = options.direct_dial.max_concurrent_attempts,
   };
}

void validate(const node::options& options) {
   validate(options.reachability_policy);
   detail::dns_address_expander::validate_policy(options.dns_resolution);
   detail::dial_scheduler::validate_policy(dial_scheduler_policy(options));
   try {
      forge::net::dns::validate(options.dns_resolver);
   } catch (const forge::net::dns::exceptions::invalid_options& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "invalid P2P DNS resolver options: " + std::string{error.what()});
   }
   if (options.private_network) {
      private_network::validate(*options.private_network);
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
   }
   const auto relay_duration = std::chrono::duration_cast<std::chrono::seconds>(options.limits.relay.max_duration);
   validate(options.limits.topology);
   for (const auto& point : options.limits.topology.rendezvous_points) {
      for (const auto& namespace_name : point.namespaces) {
         if (namespace_name.size() > options.limits.rendezvous.max_namespace_size) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                  "P2P topology rendezvous namespace exceeds the configured limit");
         }
      }
   }
   if (!options.allow_insecure_test_mode && (options.certificate_pem.empty() || options.private_key_pem.empty())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "production P2P node requires mTLS certificate and private key");
   }
   if (options.certificate_pem.empty() != options.private_key_pem.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P certificate and private key must be provided together");
   }
   if (options.explicit_peer_id && !valid_peer_id(*options.explicit_peer_id)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid explicit P2P peer id");
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
       options.limits.pubsub.limits.max_rpc_size == 0 || options.limits.pubsub.limits.max_message_size == 0 ||
       options.limits.pubsub.limits.max_data_size == 0 || options.limits.pubsub.limits.max_topic_size == 0 ||
       options.limits.pubsub.limits.max_subscriptions == 0 || options.limits.pubsub.limits.max_messages == 0 ||
       options.limits.pubsub.limits.max_control_entries == 0 || options.limits.pubsub.limits.max_message_ids == 0 ||
       options.limits.pubsub.limits.max_peers_per_topic == 0 || options.limits.pubsub.limits.max_topics == 0 ||
       options.limits.pubsub.limits.max_validation_queue == 0 ||
       options.limits.pubsub.limits.max_outbound_queue_bytes == 0 ||
       options.limits.pubsub.limits.heartbeat_initial_delay.count() <= 0 ||
       options.limits.pubsub.limits.heartbeat_interval.count() <= 0 ||
       options.limits.pubsub.limits.fanout_ttl.count() <= 0 ||
       options.limits.pubsub.limits.prune_backoff.count() <= 0 ||
       options.limits.pubsub.limits.unsubscribe_backoff.count() <= 0 ||
       options.limits.pubsub.limits.validation_retry_initial_delay.count() <= 0 ||
       options.limits.pubsub.limits.validation_retry_max_delay <
           options.limits.pubsub.limits.validation_retry_initial_delay ||
       options.limits.pubsub.limits.max_validation_attempts == 0 ||
       options.limits.pubsub.limits.max_validation_redeliveries == 0 ||
       options.limits.pubsub.limits.max_validation_requests == 0 || options.limits.pubsub.limits.mesh_n == 0 ||
       options.limits.pubsub.limits.mesh_n_low == 0 ||
       options.limits.pubsub.limits.mesh_n_high < options.limits.pubsub.limits.mesh_n_low ||
       options.limits.pubsub.limits.history_length == 0 || options.limits.pubsub.limits.history_gossip == 0 ||
       options.limits.pubsub.limits.gossip_lazy == 0 || options.limits.pubsub.limits.gossip_factor <= 0.0 ||
       options.limits.pubsub.limits.gossip_retransmission == 0) {
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
       options.relay_policy.candidate_backoff.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P AutoRelay policy limits must be positive");
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
      if (!options.allow_insecure_test_mode) {
         const auto persistence = options.dht_record_persistence.find(profile.protocol);
         if (persistence == options.dht_record_persistence.end() || !persistence->second) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                  "production P2P DHT profile requires durable record persistence");
         }
      }
   }
   for (const auto& [protocol, persistence] : options.dht_record_persistence) {
      if (!persistence || !dht_protocols.contains(protocol)) {
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
       identify.max_push_operations == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P Identify limits");
   }
}

node::impl::impl(forge::asio::runtime& runtime_value, node::options options_value)
    : runtime(runtime_value), options(std::move(options_value)), identity(make_libp2p_identity_material(options)),
      local(options.explicit_peer_id ? *options.explicit_peer_id
                                     : make_peer_id(decode_public_key(identity.public_key))),
      resources(resource_limits_for(options.limits)),
      connection_gate(std::make_shared<detail::connection_gate>(options.connection_gater)),
      dial_scheduler(std::make_shared<detail::dial_scheduler>(runtime_value.context().get_executor(),
                                                              dial_scheduler_policy(options), options.dns_resolver)),
      direct_registry(runtime_value, options, identity, resources, connection_gate),
      teardown(runtime_value.context().get_executor()), lifecycle(runtime_value.context().get_executor()),
      lifecycle_wakeup(std::make_shared<detail::lifecycle_wakeup>()),
      identify_service(runtime_value.context().get_executor()), store(options.peer_state),
      dht_profiles(
          detail::make_dht_profile_states(local, options.dht_profiles, options.dht_record_persistence,
                                          [this](const peer_id& peer) { return store.find_public_key(peer); })),
      peer_exchange_value(options.limits.max_peer_exchange_queue) {
   if (!options.allow_insecure_test_mode) {
      const auto identity_peer = make_peer_id(decode_public_key(identity.public_key));
      if (local != identity_peer) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity,
                               "configured P2P peer id does not match the identity private key");
      }
      if (!options.certificate_pem.empty() && make_peer_id_from_certificate_pem(options.certificate_pem) != local) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity,
                               "configured P2P certificate does not match the identity private key");
      }
   }
}

void node::impl::request_dial_scheduler_stop() noexcept {
   dial_scheduler->request_stop();
}

boost::asio::awaitable<void> node::impl::async_close_dial_scheduler() {
   co_await dial_scheduler->async_close();
}

dialing::black_hole_status node::impl::dial_black_hole_status() const {
   return dial_scheduler->black_hole_status();
}

bool node::impl::private_network_enabled() const noexcept {
   return options.private_network.has_value();
}

void node::impl::require_private_protocol_allowed(const protocol_id& protocol) const {
   if (!private_network_enabled()) {
      return;
   }
   if (options.private_network->internet_egress == private_network::internet_egress_policy::allow_internet &&
       (protocol == builtins::autonat_v1 || protocol == builtins::autonat_v2_dial_request ||
        protocol == builtins::autonat_v2_dial_back)) {
      return;
   }
   if (protocol == builtins::autonat_v1 || protocol == builtins::autonat_v2_dial_request ||
       protocol == builtins::autonat_v2_dial_back || protocol == builtins::relay_hop ||
       protocol == builtins::relay_stop || protocol == builtins::dcutr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P private-network profile does not permit reserved protocol " + protocol.value);
   }
}

void node::impl::require_private_direct_tcp(const forge::net::p2p::endpoint& endpoint,
                                            std::string_view operation) const {
   if (private_network_enabled() && !endpoint.is_direct_tcp()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P private-network " + std::string{operation} + " requires a direct TCP endpoint");
   }
}

detail::dht_profile_state& node::impl::dht_profile(const protocol_id& protocol) {
   const auto found = dht_profiles.find(protocol);
   if (found == dht_profiles.end()) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P DHT profile is not configured");
   }
   return *found->second;
}

const detail::dht_profile_state& node::impl::dht_profile(const protocol_id& protocol) const {
   const auto found = dht_profiles.find(protocol);
   if (found == dht_profiles.end()) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P DHT profile is not configured");
   }
   return *found->second;
}

bool node::impl::launch_tracked(std::function<boost::asio::awaitable<void>()> task) noexcept {
   auto operation = lifecycle.track();
   if (!operation.active()) {
      return false;
   }
   const auto executor = operation.executor();
   const auto lifecycle_stop = operation.stop_source();
   try {
      asio::co_spawn(
          executor,
          [task = std::move(task), lifecycle_stop]() mutable -> asio::awaitable<void> {
             if (lifecycle_stop && lifecycle_stop->stop_requested()) {
                co_return;
             }
             co_await task();
          },
          [operation = std::move(operation)](std::exception_ptr error) mutable {
             static_cast<void>(error);
             operation.release();
          });
      return true;
   } catch (...) {
      return false;
   }
}

bool node::impl::launch_tracked_cleanup(std::function<boost::asio::awaitable<void>()> task) noexcept {
   auto operation = lifecycle.track();
   if (!operation.active()) {
      return false;
   }
   const auto executor = operation.executor();
   try {
      asio::co_spawn(
          executor, [task = std::move(task)]() mutable -> asio::awaitable<void> { co_await task(); },
          [operation = std::move(operation)](std::exception_ptr error) mutable {
             static_cast<void>(error);
             operation.release();
          });
      return true;
   } catch (...) {
      return false;
   }
}

std::vector<forge::net::p2p::endpoint> node::impl::local_endpoints_for_control() const {
   auto lock = std::scoped_lock{mutex};
   return local_endpoints_for_control_locked();
}

std::vector<forge::net::p2p::endpoint> node::impl::local_endpoints_for_control_locked() const {
   auto configured = options.advertised_endpoints;
   configured.insert(configured.end(), confirmed_observed_addresses.begin(), confirmed_observed_addresses.end());
   return host_addresses::merge_advertised(configured, direct_registry.local_endpoints(), local);
}

[[nodiscard]] std::optional<node::protocol_handler> node::impl::handler_for(const protocol_id& protocol) const {
   auto lock = std::scoped_lock{mutex};
   const auto it = handlers.find(protocol);
   if (it == handlers.end()) {
      return std::nullopt;
   }
   return it->second;
}

[[nodiscard]] std::vector<protocol_id> node::impl::supported_protocols() const {
   auto lock = std::scoped_lock{mutex};
   return supported_protocols_locked();
}

[[nodiscard]] std::vector<protocol_id> node::impl::supported_protocols_locked() const {
   auto out = std::vector<protocol_id>{builtins::ping,
                                       builtins::identify,
                                       builtins::identify_push};
   if (!private_network_enabled() ||
       options.private_network->internet_egress == private_network::internet_egress_policy::allow_internet) {
      if (options.reachability_policy.service_v2_enabled) {
         out.push_back(builtins::autonat_v2_dial_request);
      }
      if (options.reachability_policy.client_v2_enabled) {
         out.push_back(builtins::autonat_v2_dial_back);
      }
      if (options.reachability_policy.service_v1_enabled) {
         out.push_back(builtins::autonat_v1);
      }
   }
   if (!private_network_enabled()) {
      out.push_back(builtins::relay_stop);
      out.push_back(builtins::dcutr);
   }
   if (options.capabilities.has(capabilities::relay) || options.capabilities.has(capabilities::relay_reservation)) {
      out.push_back(builtins::relay_hop);
   }
   if (options.capabilities.has(capabilities::peer_exchange)) {
      out.push_back(builtins::peer_exchange);
   }
   for (const auto& [protocol, state] : dht_profiles) {
      if (state->profile.operating_mode == dht::mode::server) {
         out.push_back(protocol);
      }
   }
   if (options.capabilities.has(capabilities::rendezvous) &&
       (options.limits.rendezvous.operating_role == rendezvous::role::server ||
        options.limits.rendezvous.operating_role == rendezvous::role::client_and_server)) {
      out.push_back(builtins::rendezvous);
   }
   if (options.capabilities.has(capabilities::pubsub)) {
      out.push_back(builtins::meshsub_v11);
      if (options.limits.pubsub.allow_v1_0_fallback) {
         out.push_back(builtins::meshsub_v10);
      }
   }
   out.reserve(out.size() + handlers.size());
   for (const auto& [protocol, _] : handlers) {
      out.push_back(protocol);
   }
   return out;
}


void node::impl::increment_opened_protocol() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.protocol_streams_opened;
}

void node::impl::increment_protocol_accepted() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.protocol_streams_accepted;
}

void node::impl::increment_protocol_rejected() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.protocol_rejections;
}

void node::impl::increment_peer_exchange() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.peer_exchange_messages;
}

void node::impl::increment_reachability_check(reachability::state state) {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.reachability_checks;
   if (state == reachability::state::publicly_reachable) {
      ++metrics_value.reachability_public;
   } else if (state == reachability::state::private_network || state == reachability::state::blocked ||
              state == reachability::state::relay_only) {
      ++metrics_value.reachability_private;
   }
}

void node::impl::increment_rendezvous_registration() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.rendezvous_registrations;
}

void node::impl::increment_rendezvous_discover() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.rendezvous_discovers;
}

boost::asio::awaitable<void> node::impl::request_peer_exchange(const peer_id& peer) {
   if (!options.limits.topology.peer_exchange_enabled) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P peer exchange is disabled by topology policy");
   }

   auto session = co_await ensure_direct_session(peer);
   co_await identify_session(session);
   auto claim_guard = [&] {
      const auto lock = std::scoped_lock{mutex};
      if (stopped || peer_exchange_admission_closed) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped before peer exchange");
      }
      // claim_peer commits scheduler/singleflight ownership. The single-claim
      // guard takes that ownership without allocation before the lock is released.
      return detail::topology_peer_exchange_claims{
          mutex, peer_exchange_value,
          peer_exchange_value.claim_peer(peer, peer_exchange_sessions_locked(), std::chrono::steady_clock::now(),
                                         options.limits.topology.max_peer_exchange_peers,
                                         runtime.context().get_executor()),
          options.limits.topology.query_timeout};
   }();
   auto& claim = claim_guard.single_claim();
   if (claim.status == detail::peer_exchange_scheduler::claim_status::unavailable) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol,
                            "P2P peer does not advertise the exact peer exchange protocol through Identify");
   }
   if (claim.status == detail::peer_exchange_scheduler::claim_status::closed) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P peer exchange scheduler is stopped");
   }
   if (claim.status == detail::peer_exchange_scheduler::claim_status::backpressure) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer exchange waiter capacity reached");
   }
   if (claim.status == detail::peer_exchange_scheduler::claim_status::backoff ||
       claim.status == detail::peer_exchange_scheduler::claim_status::not_selected) {
      co_return;
   }
   co_await await_peer_exchange_claim(claim);
   claim_guard.release();
}

boost::asio::awaitable<void>
node::impl::await_peer_exchange_claim(detail::peer_exchange_scheduler::claim& claim,
                                      std::shared_ptr<detail::worker_terminal_owner> terminal) {
   if (claim.started()) {
      co_await run_peer_exchange(claim, std::move(terminal));
      co_return;
   }

   auto release_participant = [this, &claim](void*) noexcept {
      auto lock = std::scoped_lock{mutex};
      peer_exchange_value.leave(claim);
   };
   auto participant_guard = std::unique_ptr<void, decltype(release_participant)>{this, std::move(release_participant)};
   if (terminal) {
      static_cast<void>(terminal->publish(detail::worker_terminal_owner::callback{
          [participant = std::addressof(claim.participant)]() noexcept { participant->request_stop(); },
      }));
   }
   auto result = detail::connection_singleflight_registry::outcome{};
   try {
      result = co_await claim.participant.wait();
   } catch (const boost::system::system_error& error) {
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P peer exchange canceled while waiting");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P peer exchange wait failed",
                            forge::exceptions::ctx("reason", error.code().message()));
   }
   if (!result.succeeded) {
      FORGE_THROW_CODE(result.error.value_or(exceptions::code::internal), std::move(result.message));
   }
}

std::vector<detail::peer_exchange_scheduler::session> node::impl::peer_exchange_sessions_locked() const {
   auto result = std::vector<detail::peer_exchange_scheduler::session>{};
   result.reserve(sessions.size());
   for (const auto& [id, session] : sessions) {
      if (session->closed) {
         continue;
      }
      result.push_back(detail::peer_exchange_scheduler::session{
          .peer = session->info.remote_peer,
          .session_id = id,
          .identify_state = session->info.identify_state,
          .capabilities = session->info.capabilities,
          .protocols = session->remote_protocols,
      });
   }
   return result;
}

void node::impl::launch_peer_exchange() {
   if (!options.limits.topology.peer_exchange_enabled ||
       options.limits.topology.operating_mode == topology::mode::static_only) {
      return;
   }

   auto cancellation = std::make_shared<cancellation_latch>();
   auto self = shared_from_this();
   static_cast<void>(launch_tracked(
       [self = std::move(self), cancellation = std::move(cancellation)]() mutable -> boost::asio::awaitable<void> {
          try {
             static_cast<void>(co_await self->async_collect_topology_peer_exchange(
                 std::move(cancellation), self->options.limits.topology.max_parallel_queries));
          } catch (...) {
             auto stopping = false;
             {
                const auto lock = std::scoped_lock{self->mutex};
                stopping = self->stopped || self->peer_exchange_admission_closed;
             }
             if (!stopping) {
                forge::exceptions::capture_and_log("P2P peer exchange refresh failed");
             }
          }
       }));
}

boost::asio::awaitable<void> node::impl::run_peer_exchange(detail::peer_exchange_scheduler::claim& claim,
                                                           std::shared_ptr<detail::worker_terminal_owner> terminal) {
   auto finish = [this, &claim](bool succeeded, exceptions::code error, std::string message) noexcept {
      auto lock = std::scoped_lock{mutex};
      if (succeeded) {
         peer_exchange_value.succeed(claim, std::chrono::steady_clock::now(), options.limits.topology.refresh_interval);
      } else {
         peer_exchange_value.fail(claim, error, std::move(message), std::chrono::steady_clock::now(),
                                  options.limits.topology.query_timeout);
      }
      peer_exchange_value.leave(claim);
   };

   try {
      auto operation = std::make_shared<peer_exchange_operation>();
      operation->cancellation.reserve(1);
      auto operation_stop = std::make_shared<detail::worker_stop_bridge>();
      operation->cancellation.publish([operation_stop] noexcept { operation_stop->request_stop(); });
      auto operation_id = std::uint64_t{};
      {
         const auto lock = std::scoped_lock{mutex};
         if (stopped || peer_exchange_admission_closed) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped before peer exchange query");
         }
         operation_id = next_peer_exchange_operation_id++;
         peer_exchange_operations.emplace(operation_id, operation);
      }
      const auto release_operation = [this, operation_id](peer_exchange_operation*) noexcept {
         const auto lock = std::scoped_lock{mutex};
         peer_exchange_operations.erase(operation_id);
      };
      auto operation_cleanup =
          std::unique_ptr<peer_exchange_operation, decltype(release_operation)>{operation.get(), release_operation};
      if (terminal) {
         static_cast<void>(terminal->publish(detail::worker_terminal_owner::callback{
             [operation]() noexcept { operation->cancellation.request_stop(); },
         }));
      }

      auto session = std::shared_ptr<session_state>{};
      {
         auto lock = std::scoped_lock{mutex};
         const auto found = sessions.find(claim.selected.session_id);
         if (found != sessions.end() && found->second->info.remote_peer == claim.selected.peer &&
             !found->second->closed &&
             detail::peer_exchange_scheduler::eligible(detail::peer_exchange_scheduler::session{
                 .peer = found->second->info.remote_peer,
                 .session_id = found->second->id,
                 .identify_state = found->second->info.identify_state,
                 .capabilities = found->second->info.capabilities,
                 .protocols = found->second->remote_protocols,
             })) {
            session = found->second;
         }
      }
      if (!session) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P peer exchange session is no longer Identify-eligible");
      }
      const auto cancel_operation = [operation] noexcept { operation->cancellation.cancel(); };
      auto parent_cancellation = co_await boost::asio::this_coro::cancellation_state;
      auto parent_slot = parent_cancellation.slot();
      if (!terminal && parent_slot.is_connected()) {
         parent_slot.assign([cancel_operation](boost::asio::cancellation_type) { cancel_operation(); });
      }
      const auto clear_parent_slot = [](boost::asio::cancellation_slot* slot) noexcept { slot->clear(); };
      auto parent_slot_cleanup =
          std::unique_ptr<boost::asio::cancellation_slot, decltype(clear_parent_slot)>{&parent_slot, clear_parent_slot};
      if ((!terminal && parent_cancellation.cancelled() != boost::asio::cancellation_type::none) ||
          (terminal && terminal->stop_requested())) {
         cancel_operation();
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P peer exchange canceled before stream open");
      }
      {
         const auto lock = std::scoped_lock{mutex};
         if (stopped) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped before peer exchange stream open");
         }
      }

      const auto codec_options = codec_for(options);
      auto deadline = operation_deadline{runtime.context(), options.limits.topology.query_timeout};
      deadline.arm(cancel_operation);
      try {
         co_await detail::async_run_with_owner_cancellation(
             operation_stop,
             [this, session, operation, operation_stop,
              codec_options](boost::asio::cancellation_slot slot) -> boost::asio::awaitable<void> {
                if (operation->cancellation.cancel_requested()) {
                   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P peer exchange canceled before stream open");
                }
                auto admission = detail::make_owner_stream_admission(slot, operation_stop);
                auto stream = std::make_shared<forge::net::p2p::stream>(
                    co_await open_session_stream(session, builtins::peer_exchange, false, std::move(admission)));
                auto cancellation = detail::owner_stream_cancellation{std::move(slot), stream};
                if (operation->cancellation.cancel_requested()) {
                   cancellation.request_cancel();
                   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P peer exchange canceled during stream open");
                }
                co_await peer_exchange_codec::async_write(
                    *stream,
                    peer_exchange_message{
                        .kind = peer_exchange_message::type::peer_exchange_request,
                        .peer = local,
                        .max_frame_size = codec_options.max_message_size,
                    },
                    codec_options);
                auto response = co_await peer_exchange_codec::async_read(*stream, codec_options);
                if (response.kind != peer_exchange_message::type::peer_exchange_response) {
                   FORGE_THROW_EXCEPTION(exceptions::protocol_error, "P2P peer exchange expected response");
                }
                const auto applied_response_limit =
                    peer_exchange_codec::negotiate_response_max_frame_size(response.max_frame_size, codec_options);
                if (applied_response_limit != response.max_frame_size) {
                   FORGE_THROW_EXCEPTION(exceptions::protocol_error,
                                         "P2P peer exchange response exceeds the advertised receive limit");
                }
                const auto observed_at = std::chrono::system_clock::now();
                detail::learn_authenticated_peer_exchange_response(
                    store, response, session->info.remote_peer, session->remote_endpoint, observed_at,
                    detail::saturating_topology_expiry(observed_at, options.limits.topology.refresh_interval));
                increment_peer_exchange();
                co_await stream->async_close();
             });
         if (operation->cancellation.cancel_requested()) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P peer exchange canceled before operation start");
         }
         if (!deadline.finish()) {
            throw_operation_timeout("P2P peer exchange");
         }
      } catch (...) {
         const auto completed = deadline.finish();
         operation->cancellation.cancel();
         if ((terminal && terminal->stop_requested()) ||
             (!terminal && parent_cancellation.cancelled() != boost::asio::cancellation_type::none)) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P peer exchange canceled");
         }
         if (operation->cancellation.stop_requested()) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped during peer exchange");
         }
         if (deadline.timed_out() || !completed) {
            throw_operation_timeout("P2P peer exchange");
         }
         throw;
      }
      finish(true, exceptions::code::internal, {});
   } catch (const forge::exceptions::base& error) {
      const auto code = p2p_code(error);
      // Claim settlement is noexcept: copying an exception message here could
      // otherwise turn an allocation failure into an ownerless scheduler claim.
      finish(false, code, {});
      rethrow_transport_as_p2p(error);
   } catch (...) {
      finish(false, exceptions::code::internal, {});
      throw;
   }
}

boost::asio::awaitable<void> node::impl::handle_ping(forge::net::p2p::stream stream) {
   try {
      while (true) {
         auto payload = co_await stream.async_read();
         if (payload.size() != 32) {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "libp2p ping payload must be 32 bytes");
         }
         co_await stream.async_write(payload);
      }
   } catch (const forge::exceptions::base& error) {
      if (!is_orderly_stream_close(error)) {
         throw;
      }
      co_return;
   } catch (...) {
      throw;
   }
}


[[nodiscard]] host_addresses::learning_context
discovery_context_for_session_peer(std::optional<peer_id> session_peer, std::optional<endpoint> session_remote_endpoint,
                                   std::optional<endpoint> session_direct_endpoint, const peer_id& peer) {
   if (session_peer && peer == *session_peer) {
      auto remote_endpoint =
          session_remote_endpoint ? std::move(session_remote_endpoint) : std::move(session_direct_endpoint);
      return host_addresses::learning_context{
          .source = host_addresses::source_kind::authenticated,
          .remote_endpoint = std::move(remote_endpoint),
      };
   }
   return host_addresses::learning_context{.source = host_addresses::source_kind::third_party};
}

namespace {

[[nodiscard]] std::vector<forge::multiformats::multiaddr>
endpoints_from_registration(const rendezvous::registration& registration) {
   if (registration.signed_peer_record.empty()) {
      return registration.endpoints;
   }
   try {
      const auto record = rendezvous::codec::open_peer_record(signed_envelope::decode(registration.signed_peer_record),
                                                              registration.peer);
      return record.endpoints;
   } catch (const forge::exceptions::base&) {
      return {};
   }
}

[[nodiscard]] std::optional<rendezvous::registration>
sanitize_registration_for_session(rendezvous::registration registration, const auto& session) {
   const auto original_endpoints = endpoints_from_registration(registration);
   const auto signed_record = !registration.signed_peer_record.empty();
   if (original_endpoints.empty()) {
      if (signed_record) {
         return std::nullopt;
      }
      return registration;
   }
   auto sanitized = host_addresses::sanitize_discovered_addresses(
       original_endpoints, registration.peer,
       discovery_context_for_session_peer(session ? std::optional<peer_id>{session->info.remote_peer} : std::nullopt,
                                          session ? session->remote_endpoint : std::nullopt,
                                          session ? session->direct_endpoint : std::nullopt, registration.peer));
   if (sanitized.empty()) {
      return std::nullopt;
   }
   if (signed_record) {
      if (sanitized.size() != original_endpoints.size()) {
         return std::nullopt;
      }
      for (auto index = std::size_t{0}; index < sanitized.size(); ++index) {
         if (sanitized[index].to_string() != original_endpoints[index].to_string()) {
            return std::nullopt;
         }
      }
   }
   registration.endpoints = std::move(sanitized);
   return registration;
}

} // namespace

boost::asio::awaitable<void> node::impl::handle_rendezvous(std::shared_ptr<node::impl::session_state> session,
                                                           forge::net::p2p::stream stream) {
   if (!options.capabilities.has(capabilities::rendezvous) ||
       (options.limits.rendezvous.operating_role != rendezvous::role::server &&
        options.limits.rendezvous.operating_role != rendezvous::role::client_and_server)) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "rendezvous server mode is disabled");
   }
   auto buffer = std::vector<std::uint8_t>{};
   auto request = rendezvous::codec::decode(
       co_await async_read_length_delimited(stream, buffer, options.limits.rendezvous.max_message_size),
       options.limits.rendezvous);

   if (request.type == rendezvous::message_type::register_peer && request.register_value) {
      auto response = rendezvous::register_response{.status_value = rendezvous::status::ok};
      auto ttl = request.register_value->ttl.count() == 0 ? options.limits.rendezvous.default_ttl
                                                          : request.register_value->ttl;
      if (ttl < options.limits.rendezvous.min_ttl || ttl > options.limits.rendezvous.max_ttl) {
         response.status_value = rendezvous::status::invalid_ttl;
         response.status_text = "rendezvous registration TTL outside allowed range";
      } else {
         auto endpoints = std::vector<forge::multiformats::multiaddr>{};
         auto registered_peer = session->info.remote_peer;
         if (!request.register_value->signed_peer_record.empty()) {
            try {
               const auto record = rendezvous::codec::open_peer_record(
                   signed_envelope::decode(request.register_value->signed_peer_record), session->info.remote_peer);
               registered_peer = record.peer;
               endpoints = record.endpoints;
            } catch (const forge::exceptions::base&) {
               response.status_value = rendezvous::status::invalid_signed_peer_record;
               response.status_text = "rendezvous signed peer record is invalid";
            }
         } else if (options.limits.rendezvous.require_signed_peer_record) {
            response.status_value = rendezvous::status::invalid_signed_peer_record;
            response.status_text = "rendezvous registration requires signed peer record";
         }
         if (response.status_value == rendezvous::status::ok && endpoints.empty()) {
            if (const auto record = store.find(registered_peer)) {
               for (const auto& endpoint : record->endpoints) {
                  endpoints.push_back(endpoint.address);
               }
            }
         }
         if (response.status_value == rendezvous::status::ok) {
            auto registration = rendezvous::registration{
                .namespace_name = request.register_value->namespace_name,
                .peer = registered_peer,
                .endpoints = std::move(endpoints),
                .signed_peer_record = request.register_value->signed_peer_record,
                .ttl = ttl,
                .expires_at = detail::rendezvous_expiry_after(std::chrono::system_clock::now(), ttl),
            };
            auto sanitized = sanitize_registration_for_session(std::move(registration), session);
            if (!sanitized) {
               response.status_value = rendezvous::status::not_authorized;
               response.status_text = "rendezvous registration endpoints are not routable from source";
            } else {
               try {
                  co_await store.async_register_rendezvous(std::move(*sanitized),
                                                           options.limits.rendezvous.max_registrations_per_peer);
                  response.ttl = ttl;
                  increment_rendezvous_registration();
               } catch (const forge::exceptions::base& error) {
                  if (exceptions::code_of(error) != exceptions::code::backpressure_rejected) {
                     throw;
                  }
                  response.status_value = rendezvous::status::unavailable;
                  response.status_text = "rendezvous registration capacity reached";
               }
            }
         }
      }
      co_await stream.async_write(rendezvous::codec::encode(
          rendezvous::message{
              .type = rendezvous::message_type::register_response,
              .register_response_value = std::move(response),
          },
          options.limits.rendezvous));
      co_await stream.async_close();
      co_return;
   }

   if (request.type == rendezvous::message_type::unregister_peer && request.unregister_value) {
      co_await store.async_remove_rendezvous(session->info.remote_peer, request.unregister_value->namespace_name);
      co_await stream.async_close();
      co_return;
   }

   if (request.type == rendezvous::message_type::discover && request.discover_value) {
      const auto after = rendezvous::codec::read_cookie(request.discover_value->cookie);
      const auto cookie_namespace = rendezvous::codec::read_cookie_namespace(request.discover_value->cookie);
      if (!request.discover_value->cookie.empty() && cookie_namespace != request.discover_value->namespace_name) {
         co_await stream.async_write(rendezvous::codec::encode(
             rendezvous::message{
                 .type = rendezvous::message_type::discover_response,
                 .discover_response_value =
                     rendezvous::discover_response{
                         .status_value = rendezvous::status::invalid_cookie,
                         .status_text = "rendezvous cookie belongs to a different namespace",
                     },
             },
             options.limits.rendezvous));
         co_await stream.async_close();
         co_return;
      }
      const auto limit = request.discover_value->limit == 0
                             ? options.limits.rendezvous.max_discover_limit
                             : std::min(request.discover_value->limit, options.limits.rendezvous.max_discover_limit);
      auto registrations = store.discover_rendezvous(request.discover_value->namespace_name, after, limit);
      auto sequence = after;
      for (const auto& registration : registrations) {
         sequence = std::max(sequence, registration.sequence);
      }
      increment_rendezvous_discover();
      co_await stream.async_write(rendezvous::codec::encode(
          rendezvous::message{
              .type = rendezvous::message_type::discover_response,
              .discover_response_value =
                  rendezvous::discover_response{
                      .registrations = std::move(registrations),
                      .cookie = rendezvous::codec::make_cookie(sequence, request.discover_value->namespace_name),
                      .status_value = rendezvous::status::ok,
                  },
          },
          options.limits.rendezvous));
      co_await stream.async_close();
      co_return;
   }

   co_await stream.async_write(rendezvous::codec::encode(
       rendezvous::message{
           .type = rendezvous::message_type::discover_response,
           .discover_response_value =
               rendezvous::discover_response{
                   .status_value = rendezvous::status::internal_error,
                   .status_text = "unexpected rendezvous message",
               },
       },
       options.limits.rendezvous));
   co_await stream.async_close();
}

boost::asio::awaitable<void> node::impl::handle_peer_exchange(forge::net::p2p::stream stream, std::uint64_t request_id,
                                                              std::uint64_t remote_receive_limit) {
   auto response_options = codec_for(options);
   response_options.max_message_size =
       peer_exchange_codec::negotiate_response_max_frame_size(remote_receive_limit, response_options);
   auto response = peer_exchange_message{
       .kind = peer_exchange_message::type::peer_exchange_response,
       .request_id = request_id,
       .peer = local,
       .capabilities = options.capabilities,
       .max_frame_size = response_options.max_message_size,
   };
   if (peer_exchange_codec::encoded_size(response, response_options) > response_options.max_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "peer exchange receive limit cannot hold a response header");
   }

   const auto append_endpoint = [&](peer_exchange_message::endpoint_record endpoint) {
      if (response.endpoints.size() >= response_options.max_endpoint_records) {
         return false;
      }
      response.endpoints.push_back(std::move(endpoint));
      if (peer_exchange_codec::encoded_size(response, response_options) <= response_options.max_message_size) {
         return true;
      }
      response.endpoints.pop_back();
      return false;
   };

   for (const auto& endpoint : local_endpoints_for_control()) {
      append_endpoint(peer_exchange_message::endpoint_record{
          .peer = local,
          .address = endpoint.to_multiaddr(),
          .capabilities = options.capabilities,
      });
      if (response.endpoints.size() >= response_options.max_endpoint_records) {
         break;
      }
   }
   const auto remaining = response_options.max_endpoint_records - response.endpoints.size();
   const auto candidates = store.scored_candidates(remaining);
   for (const auto& record : candidates) {
      if (record.peer == local) {
         continue;
      }
      for (const auto& endpoint : record.endpoints) {
         if (response.endpoints.size() >= response_options.max_endpoint_records) {
            break;
         }
         append_endpoint(peer_exchange_message::endpoint_record{
             .peer = record.peer,
             .address = endpoint.address,
             .capabilities = record.capabilities,
         });
         if (response.endpoints.size() >= response_options.max_endpoint_records) {
            break;
         }
      }
      if (response.endpoints.size() >= response_options.max_endpoint_records) {
         break;
      }
   }
   increment_peer_exchange();
   co_await peer_exchange_codec::async_write(stream, response, response_options);
   co_await stream.async_close();
}

} // namespace forge::net::p2p
