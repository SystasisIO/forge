#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace forge::net::p2p::detail::certified_peer_record {

inline constexpr auto payload_type = std::array<std::uint8_t, 2>{0x03, 0x01};

[[nodiscard]] signed_envelope seal(const rendezvous::peer_record& record, const public_key& key,
                                   const forge::crypto::asymmetric::private_key& private_key);
// Authenticate the envelope and bind the record peer to its signer, including the existing legacy profile.
[[nodiscard]] rendezvous::peer_record open(const signed_envelope& envelope, std::optional<peer_id> expected_signer);

} // namespace forge::net::p2p::detail::certified_peer_record
