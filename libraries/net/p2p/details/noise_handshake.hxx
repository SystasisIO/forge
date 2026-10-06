#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <boost/asio/awaitable.hpp>

namespace forge::net::p2p {

struct libp2p_identity_material;
class cancellation_latch;

} // namespace forge::net::p2p

namespace forge::net::p2p::detail {

class secure_io;

struct x25519_key {
   forge::crypto::asymmetric::x25519::private_key key;
   std::array<std::uint8_t, 32> public_key{};
};

struct noise_handshake_payload {
   std::vector<std::uint8_t> identity_key;
   std::vector<std::uint8_t> identity_signature;
   std::vector<std::string> stream_muxers;
};

struct verified_noise_payload {
   peer_id peer;
   std::optional<protocol_id> muxer;
};

[[nodiscard]] std::vector<std::uint8_t> encode_noise_payload(const noise_handshake_payload& value);
[[nodiscard]] noise_handshake_payload decode_noise_payload(std::span<const std::uint8_t> bytes);
[[nodiscard]] verified_noise_payload verify_noise_payload(const noise_handshake_payload& payload,
                                                          std::span<const std::uint8_t> static_key,
                                                          const std::optional<peer_id>& expected_peer);

struct noise_result {
   peer_id peer;
   std::shared_ptr<secure_io> secure;
   std::optional<protocol_id> muxer;
};

boost::asio::awaitable<noise_result> noise_initiator(forge::net::p2p::stream stream,
                                                     const libp2p_identity_material& identity,
                                                     std::optional<peer_id> expected_peer,
                                                     const std::shared_ptr<cancellation_latch>& cancel_current = {});

boost::asio::awaitable<noise_result> noise_responder(forge::net::p2p::stream stream,
                                                     const libp2p_identity_material& identity,
                                                     std::optional<peer_id> expected_peer,
                                                     const std::shared_ptr<cancellation_latch>& cancel_current = {});

} // namespace forge::net::p2p::detail
