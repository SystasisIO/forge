#pragma once

#include <cstdint>
#include <string>
#include <vector>

import forge.net.p2p.identity;

namespace forge::test::libp2p_interop {

struct libp2p_identity {
   std::string certificate_pem;
   std::string private_key_pem;
   std::vector<std::uint8_t> public_key;
   forge::net::p2p::peer_id peer;
};

// The pinned Rust fixture enables Ed25519, not the optional rsa host-key feature.
// These identities exercise real TLS/Noise/QUIC, but do not prove RSA interop.
[[nodiscard]] libp2p_identity generate_libp2p_identity();

} // namespace forge::test::libp2p_interop
