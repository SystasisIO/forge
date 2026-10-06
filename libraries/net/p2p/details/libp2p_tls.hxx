#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "libp2p_identity_material.hxx"

namespace forge::net::p2p {

struct libp2p_tls_material {
   std::string certificate_pem;
   std::string private_key_pem;
};

struct libp2p_tls_handshake {
   peer_id peer;
   std::optional<protocol_id> muxer;
};

[[nodiscard]] libp2p_tls_material make_libp2p_tls_material(const libp2p_identity_material& identity);
[[nodiscard]] peer_id verify_libp2p_tls_chain(const forge::net::stcp::certificate_chain& chain,
                                              const std::optional<peer_id>& expected_peer);
[[nodiscard]] libp2p_tls_handshake
verify_libp2p_tls_handshake(const forge::net::stcp::certificate_chain& chain, std::string_view selected_alpn,
                           const std::optional<peer_id>& expected_peer);
[[nodiscard]] forge::net::stcp::client_options make_libp2p_tls_client_options(const libp2p_identity_material& identity);
[[nodiscard]] forge::net::stcp::server_options make_libp2p_tls_server_options(const libp2p_identity_material& identity);

} // namespace forge::net::p2p
