module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.diagnostics.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.plugins.net.p2p.diagnostics.api;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.diagnostics.types;

#include "details/config.hxx"

namespace forge::plugins::net::p2p::diagnostics {

config decode_config(const forge::config::core::component_view& view) {
   if (view.source().try_get("plugins.p2p.diagnostics")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "retired plugin configuration: plugins.p2p.diagnostics; use plugins.net.p2p.diagnostics");
   }
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                          forge::config::core::format_decode_diagnostics("invalid P2P diagnostics config",
                                                                 decoded.diagnostics));
   }
   return std::move(decoded.value);
}

void validate_config(const config& value) {
   static_cast<void>(value);
}

forge::net::p2p::diagnostics::options configured_options(const config& value) {
   return forge::net::p2p::diagnostics::options{
      .max_peers = static_cast<std::size_t>(value.max_peers),
      .max_sessions = static_cast<std::size_t>(value.max_sessions),
      .max_endpoints_per_peer = static_cast<std::size_t>(value.max_endpoints_per_peer),
      .max_protocols_per_peer = static_cast<std::size_t>(value.max_protocols_per_peer),
      .max_relay_reservations_per_peer = static_cast<std::size_t>(value.max_relay_reservations_per_peer),
   };
}

std::vector<forge::net::p2p::diagnostics::peer>
filter_peers(const forge::net::p2p::diagnostics::snapshot& snapshot, const filter& filter) {
   auto connected = std::set<forge::net::p2p::peer_id>{};
   if (filter.only_connected) {
      for (const auto& session : snapshot.sessions) {
         connected.insert(session.remote_peer);
      }
   }

   auto out = std::vector<forge::net::p2p::diagnostics::peer>{};
   out.reserve(snapshot.peers.size());
   for (const auto& peer : snapshot.peers) {
      if (filter.peer.has_value() && peer.peer != *filter.peer) {
         continue;
      }
      if (filter.only_connected && !connected.contains(peer.peer)) {
         continue;
      }
      if (filter.only_protected && !peer.protected_peer) {
         continue;
      }
      out.push_back(peer);
      if (filter.limit != 0 && out.size() >= filter.limit) {
         break;
      }
   }
   return out;
}

} // namespace forge::plugins::net::p2p::diagnostics
