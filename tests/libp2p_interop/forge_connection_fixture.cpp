#include <stdexcept>
#include <string>

import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.scoring;

#include "forge_connection_fixture.hxx"

namespace forge::test::libp2p_interop {

void require_fresh_identify(const forge::net::p2p::node& value, const forge::net::p2p::peer_id& peer) {
   const auto snapshot = value.diagnostics();
   const auto record = value.peers().find(peer);
   if (snapshot.metrics.sessions_opened != 0 || snapshot.metrics.handshakes_completed != 0 ||
       !snapshot.sessions.empty() || (record && (!record->protocols.empty() || !record->signed_peer_record.empty()))) {
      throw std::runtime_error{"connection evidence requires a fresh node and no cached Identify"};
   }
}

forge::net::p2p::diagnostics::session
capture_identified_connection(const forge::net::p2p::node& value, const forge::net::p2p::peer_id& peer,
                              const forge::net::p2p::endpoint& remote,
                              forge::net::p2p::diagnostics::session_direction direction) {
   const auto snapshot = value.diagnostics();
   if (snapshot.metrics.sessions_opened != 1 || snapshot.metrics.sessions_closed != 0 ||
       snapshot.metrics.sessions_pruned != 0 || snapshot.sessions.size() != 1) {
      throw std::runtime_error{"connection evidence requires exactly one established session and no reconnect"};
   }
   const auto& session = snapshot.sessions.front();
   if (session.closed || session.remote_peer != peer || session.path != forge::net::p2p::path::kind::direct ||
       session.direction != direction ||
       !session.remote_endpoint ||
       session.identify_state != forge::net::p2p::identify::state::identified || !session.identify_error.empty()) {
      throw std::runtime_error{"connection evidence lacks successful authenticated direct Identify"};
   }
   auto observed_address = *session.remote_endpoint;
   auto expected_address = remote;
   if ((observed_address.peer && *observed_address.peer != peer) ||
       (expected_address.peer && *expected_address.peer != peer)) {
      throw std::runtime_error{"connection address contradicts the authenticated peer"};
   }
   observed_address.peer.reset();
   expected_address.peer.reset();
   if (observed_address.to_string() != expected_address.to_string()) {
      throw std::runtime_error{"connection evidence does not match the dialed endpoint"};
   }
   const auto record = value.peers().find(peer);
   if (!record || record->protocols.empty() || record->signed_peer_record.empty()) {
      throw std::runtime_error{"connection evidence lacks a verified signed Identify record"};
   }
   return session;
}

void require_same_connection(const forge::net::p2p::node& value,
                             const forge::net::p2p::diagnostics::session& expected) {
   if (!expected.remote_endpoint ||
       capture_identified_connection(value, expected.remote_peer, *expected.remote_endpoint, expected.direction).id != expected.id) {
      throw std::runtime_error{"application exchange did not retain its authenticated connection"};
   }
}

} // namespace forge::test::libp2p_interop
