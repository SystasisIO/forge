module;

#include <forge/exceptions/macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.exceptions;
import forge.net.p2p.resource_manager;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.config;
import forge.schema.exceptions;
import forge.schema.scalar;

#include "details/resource_policy.hxx"

namespace forge::plugins::net::p2p::node::detail {
namespace {

template<class Target>
void apply_budget(Target& target, const std::optional<std::uint64_t>& budget,
                  std::string_view scope, std::string_view field) {
   if (!budget.has_value()) { return; }
   try {
      target = forge::schema::checked_integral_cast<Target>(*budget);
   } catch (const forge::schema::exceptions::invalid_value&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P resource budget is outside its native type range",
                            forge::exceptions::ctx("field", "resources." +
                                (scope.empty() ? std::string{} : std::string{scope} + ".") + std::string{field}));
   }
}

void apply_scope(forge::net::p2p::resource_manager::scope_limits& target,
                 const resource_config::scope_patch& patch, std::string_view name) {
   apply_budget(target.max_memory, patch.max_memory, name, "max-memory");
   apply_budget(target.max_file_descriptors, patch.max_file_descriptors, name, "max-file-descriptors");
   apply_budget(target.max_inbound_connections, patch.max_inbound_connections, name, "max-inbound-connections");
   apply_budget(target.max_outbound_connections, patch.max_outbound_connections, name, "max-outbound-connections");
   apply_budget(target.max_connections, patch.max_connections, name, "max-connections");
   apply_budget(target.max_inbound_streams, patch.max_inbound_streams, name, "max-inbound-streams");
   apply_budget(target.max_outbound_streams, patch.max_outbound_streams, name, "max-outbound-streams");
   apply_budget(target.max_streams, patch.max_streams, name, "max-streams");
}

} // namespace

void apply_resource_config(forge::net::p2p::resource_manager::limits& limits, const resource_config& value) {
   auto candidate = limits;
   apply_scope(candidate.system, value.system, "system");
   apply_scope(candidate.transient, value.transient, "transient");
   apply_scope(candidate.peer, value.peer, "peer");
   apply_scope(candidate.protocol, value.protocol, "protocol");
   apply_scope(candidate.service, value.service, "service");
   apply_scope(candidate.protocol_peer, value.protocol_peer, "protocol-peer");
   apply_scope(candidate.service_peer, value.service_peer, "service-peer");
   apply_scope(candidate.connection, value.connection, "connection");
   apply_scope(candidate.stream, value.stream, "stream");
   apply_budget(candidate.max_dial_attempts, value.max_dial_attempts, "", "max-dial-attempts");
   apply_budget(candidate.max_dial_attempts_per_peer, value.max_dial_attempts_per_peer, "", "max-dial-attempts-per-peer");
   apply_budget(candidate.max_relay_reservations, value.max_relay_reservations, "", "max-relay-reservations");
   apply_budget(candidate.max_malformed_messages_per_peer, value.max_malformed_messages_per_peer,
                "", "max-malformed-messages-per-peer");
   limits = std::move(candidate);
}

} // namespace forge::plugins::net::p2p::node::detail
