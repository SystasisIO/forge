module;

#include <forge/exceptions/macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

module forge.plugins.net.p2p.node.plugin;

import forge.config.core.component;
import forge.exceptions;
import forge.net.p2p.endpoint;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;
import forge.schema.exceptions;
import forge.schema.scalar;

#include "details/configuration_policy.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

void apply_size(std::size_t& target, const std::optional<std::uint64_t>& value, std::string_view field) {
   if (!value) { return; }
   try {
      target = forge::schema::checked_integral_cast<std::size_t>(*value);
   } catch (const forge::schema::exceptions::invalid_value&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P count exceeds the native type range",
                            forge::exceptions::ctx("field", field));
   }
}

void apply_time(std::chrono::milliseconds& target, const std::optional<std::uint64_t>& value) {
   if (value) { target = to_ms(*value); }
}

} // namespace

void apply_connections(forge::net::p2p::node::limits& limits, const connection_config& value) {
   apply_size(limits.session_low_watermark, value.session_low_watermark, "connections.session-low-watermark");
   apply_time(limits.session_grace_period, value.session_grace_period_ms);
   apply_time(limits.session_prune_silence, value.session_prune_silence_ms);
   apply_time(limits.dial_backoff_base, value.dial_backoff_base_ms);
   apply_time(limits.dial_backoff_step, value.dial_backoff_step_ms);
   apply_time(limits.dial_backoff_max, value.dial_backoff_max_ms);
}

void apply_private_network(forge::net::p2p::node::options& options, const config& value) {
   options.private_network.reset();
   if (!value.private_network) { return; }
   if (value.private_network->psk_secret.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P private-network requires a nonempty PSK secret reference");
   }
   if (value.quic != quic_config{}) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P private-network does not permit explicit QUIC limits");
   }
   if (value.path_policy != path_policy::direct_only) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P private-network requires explicit direct-only path policy");
   }
   using native = forge::net::p2p::private_network::internet_egress_policy;
   auto egress = native::deny_external;
   switch (value.private_network->internet_egress) {
   case internet_egress_policy::deny_external: break;
   case internet_egress_policy::allow_internet: egress = native::allow_internet; break;
   default: FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P private-network Internet-egress policy is invalid");
   }
   // Only the declaration is stored here; startup provisions the real protector through Secrets.
   options.private_network = forge::net::p2p::private_network::options{.internet_egress = egress};
}

void apply_identify(forge::net::p2p::node::options& options, const identify_config& value) {
   apply_time(options.identify.timeout, value.timeout_ms);
   apply_size(options.identify.max_message_size, value.max_message_size, "identify.max-message-size");
   apply_size(options.identify.max_total_message_size, value.max_total_message_size, "identify.max-total-message-size");
   apply_size(options.identify.max_own_message_size, value.max_own_message_size, "identify.max-own-message-size");
   apply_size(options.identify.max_message_parts, value.max_message_parts, "identify.max-message-parts");
   apply_size(options.identify.max_protocols, value.max_protocols, "identify.max-protocols");
   apply_size(options.identify.max_listen_endpoints, value.max_listen_endpoints, "identify.max-listen-endpoints");
   apply_size(options.identify.max_protocol_size, value.max_protocol_size, "identify.max-protocol-size");
   apply_size(options.identify.max_version_size, value.max_version_size, "identify.max-version-size");
   apply_size(options.identify.max_public_key_size, value.max_public_key_size, "identify.max-public-key-size");
   apply_size(options.identify.max_signed_peer_record_size, value.max_signed_peer_record_size, "identify.max-signed-peer-record-size");
   apply_size(options.identify.max_push_operations, value.max_push_operations, "identify.max-push-operations");
   if (value.protocol_version) { options.protocol_version = *value.protocol_version; }
   if (value.agent_version) { options.agent_version = *value.agent_version; }
}

void apply_peer_state(forge::net::p2p::peer_store::options& options, const peer_state_config& value) {
   apply_size(options.max_peers, value.max_peers, "peer-state.max-peers");
   apply_size(options.max_rendezvous, value.max_rendezvous, "peer-state.max-rendezvous");
   apply_size(options.max_pending, value.max_pending, "peer-state.max-pending");
   apply_size(options.max_endpoints_per_peer, value.max_endpoints_per_peer, "peer-state.max-endpoints-per-peer");
   apply_size(options.max_protocols_per_peer, value.max_protocols_per_peer, "peer-state.max-protocols-per-peer");
   apply_size(options.max_relay_reservations_per_peer, value.max_relay_reservations_per_peer, "peer-state.max-relay-reservations-per-peer");
   apply_size(options.max_relay_endpoints_per_reservation, value.max_relay_endpoints_per_reservation,
              "peer-state.max-relay-endpoints-per-reservation");
   apply_size(options.max_peer_record_bytes, value.max_peer_record_bytes, "peer-state.max-peer-record-bytes");
   apply_size(options.hydration_page_limit, value.hydration_page_limit, "peer-state.hydration-page-limit");
   apply_size(options.prune_page_limit, value.prune_page_limit, "peer-state.prune-page-limit");
   apply_size(options.max_persistence_waiters, value.max_persistence_waiters, "peer-state.max-persistence-waiters");
}

void apply_tcp(forge::net::p2p::node::options& options, const tcp_config& value) {
   if (!value.security) { return; }
   using native = forge::net::p2p::node::stream_security;
   switch (*value.security) {
   case tcp_security::tls_and_noise:
      options.stream_security = native::tls_and_noise;
      return;
   case tcp_security::tls:
      options.stream_security = native::tls;
      return;
   case tcp_security::noise:
      options.stream_security = native::noise;
      return;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Invalid P2P TCP security policy",
                         forge::exceptions::ctx("field", "tcp.security"));
}

void apply_quic(forge::net::p2p::node::options& options, const quic_config& value) {
   auto& limits = options.transport_limits;
   apply_size(limits.max_connections, value.max_connections, "quic.max-connections");
   apply_size(limits.max_streams_per_connection, value.max_streams_per_connection,
              "quic.max-streams-per-connection");
   apply_size(limits.max_queued_bytes, value.max_queued_bytes, "quic.max-queued-bytes");
   apply_size(limits.max_inbound_queued_bytes, value.max_inbound_queued_bytes, "quic.max-inbound-queued-bytes");
   apply_size(limits.max_inbound_queued_packets, value.max_inbound_queued_packets, "quic.max-inbound-queued-packets");
}

} // namespace forge::plugins::net::p2p::node
