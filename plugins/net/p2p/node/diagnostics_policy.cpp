module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.diagnostics_exceptions;
import forge.plugins.net.p2p.node.diagnostics_types;

#include "details/diagnostics_policy.hxx"

namespace forge::plugins::net::p2p::node::detail {

forge::net::p2p::diagnostics::options configured_options(const diagnostics_config& value) {
   return forge::net::p2p::diagnostics::options{
      .max_peers = static_cast<std::size_t>(value.max_peers),
      .max_sessions = static_cast<std::size_t>(value.max_sessions),
      .max_endpoints_per_peer = static_cast<std::size_t>(value.max_endpoints_per_peer),
      .max_protocols_per_peer = static_cast<std::size_t>(value.max_protocols_per_peer),
      .max_relay_reservations_per_peer = static_cast<std::size_t>(value.max_relay_reservations_per_peer),
   };
}

std::vector<forge::net::p2p::diagnostics::peer>
filter_peers(const forge::net::p2p::diagnostics::snapshot& snapshot, const peer_filter& peer_filter) {
   auto connected = std::set<forge::net::p2p::peer_id>{};
   if (peer_filter.only_connected) {
      for (const auto& session : snapshot.sessions) {
         connected.insert(session.remote_peer);
      }
   }

   auto out = std::vector<forge::net::p2p::diagnostics::peer>{};
   out.reserve(snapshot.peers.size());
   for (const auto& peer : snapshot.peers) {
      if (peer_filter.peer.has_value() && peer.peer != *peer_filter.peer) {
         continue;
      }
      if (peer_filter.only_connected && !connected.contains(peer.peer)) {
         continue;
      }
      if (peer_filter.only_protected && !peer.protected_peer) {
         continue;
      }
      out.push_back(peer);
      if (peer_filter.limit != 0 && out.size() >= peer_filter.limit) {
         break;
      }
   }
   return out;
}

} // namespace forge::plugins::net::p2p::node::detail
