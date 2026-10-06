module;

#include <forge/exceptions/macros.hpp>

#include <memory>
#include <span>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

module forge.net.p2p.node;

import forge.crypto.asymmetric;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.stream;
import forge.net.tcp.connection;
import forge.net.yamux.session;

#include "details/relay_transport.hxx"
#include "details/libp2p_identity_material.hxx"
#include "details/stream_upgrade.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] upgrade_callbacks verify_relay_peer_before_callbacks(const peer_id& expected_peer,
                                                                   upgrade_callbacks callbacks) {
   return upgrade_callbacks{
       .secured =
           [expected_peer, callback = std::move(callbacks.secured)](const peer_id& authenticated_peer) {
              if (authenticated_peer != expected_peer) {
                 FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                                       "P2P relay Noise peer does not match the relay control message");
              }
              if (callback) {
                 callback(authenticated_peer);
              }
           },
       .established =
           [expected_peer, callback = std::move(callbacks.established)](const peer_id& authenticated_peer) {
              if (authenticated_peer != expected_peer) {
                 FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                                       "P2P relay Noise peer does not match the relay control message");
              }
              if (callback) {
                 callback(authenticated_peer);
              }
           },
       .upgraded =
           [expected_peer, callback = std::move(callbacks.upgraded)](const peer_id& authenticated_peer) {
              if (authenticated_peer != expected_peer) {
                 FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                                       "P2P relay Noise peer does not match the relay control message");
              }
              if (callback) {
                 callback(authenticated_peer);
              }
           },
   };
}

} // namespace

boost::asio::awaitable<upgraded_session> upgrade_relay_outbound_session(forge::net::p2p::stream stream,
                                                                        const node::options& options,
                                                                        const libp2p_identity_material& identity,
                                                                        const peer_id& expected_peer,
                                                                        upgrade_callbacks callbacks) {
   auto upgraded =
       co_await upgrade_outbound_stream(std::move(stream), options, identity, std::make_optional(expected_peer),
                                        verify_relay_peer_before_callbacks(expected_peer, std::move(callbacks)));
   if (upgraded.peer != expected_peer || upgraded.authentication != peer_authentication::noise) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "P2P relay Noise peer does not match the relay control message");
   }
   co_return upgraded;
}

boost::asio::awaitable<upgraded_session> upgrade_relay_inbound_session(forge::net::p2p::stream stream,
                                                                       const node::options& options,
                                                                       const libp2p_identity_material& identity,
                                                                       const peer_id& expected_peer,
                                                                       upgrade_callbacks callbacks) {
   auto upgraded =
       co_await upgrade_inbound_stream(std::move(stream), options, identity, std::make_optional(expected_peer),
                                       verify_relay_peer_before_callbacks(expected_peer, std::move(callbacks)));
   if (upgraded.peer != expected_peer || upgraded.authentication != peer_authentication::noise) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "P2P relay Noise peer does not match the relay control message");
   }
   co_return upgraded;
}

} // namespace forge::net::p2p
