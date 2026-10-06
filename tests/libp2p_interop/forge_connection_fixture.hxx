#pragma once

// Included after the public node, identity, endpoint and diagnostics modules.
namespace forge::test::libp2p_interop {

void require_fresh_identify(const forge::net::p2p::node&, const forge::net::p2p::peer_id&);

[[nodiscard]] forge::net::p2p::diagnostics::session
capture_identified_connection(const forge::net::p2p::node&, const forge::net::p2p::peer_id&,
                              const forge::net::p2p::endpoint&,
                              forge::net::p2p::diagnostics::session_direction =
                                  forge::net::p2p::diagnostics::session_direction::outbound);

void require_same_connection(const forge::net::p2p::node&,
                             const forge::net::p2p::diagnostics::session&);

// JSON receipt from an actual authenticated stream and its retained session.
// Shared by private-profile and path fixtures; it never infers the muxer from TCP.
[[nodiscard]] std::string endpoint_connection_receipt(
    const forge::net::p2p::node&, const forge::net::p2p::peer_id&, const forge::net::p2p::stream&,
    forge::net::p2p::diagnostics::session_direction = forge::net::p2p::diagnostics::session_direction::outbound);

} // namespace forge::test::libp2p_interop
