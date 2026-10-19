module;

#include <boost/describe.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

export module forge.swarm.protocol.peer_message;

export import forge.swarm.protocol.metadata;
export import forge.swarm.core.checkpoint;
import forge.raw.enum_wire_type;

export namespace forge::swarm::protocol {

inline constexpr std::uint32_t max_wire_message_size = 64U * 1024U;
inline constexpr std::uint32_t max_bitmap_size = (core::max_piece_count + 7U) / 8U;
inline constexpr std::uint32_t max_proof_siblings = 18U;

enum class rejection : std::uint8_t { unavailable = 1, choked = 2, invalid_request = 3, canceled = 4 };

struct handshake {
   std::uint16_t version = 1;
   core::swarm_id swarm;
   std::string realm;
   std::uint32_t capabilities = 0;
   bool operator==(const handshake&) const = default;
};
struct inventory {
   std::vector<std::uint8_t> pieces;
   bool operator==(const inventory&) const = default;
};
struct have {
   core::piece_id piece;
   bool operator==(const have&) const = default;
};
struct interest {
   bool interested = false;
   bool operator==(const interest&) const = default;
};
struct choke {
   bool choked = true;
   bool operator==(const choke&) const = default;
};
struct request {
   std::uint64_t id = 0;
   core::piece_id piece;
   std::uint32_t offset = 0;
   std::uint32_t length = 0;
   bool operator==(const request&) const = default;
};
struct block {
   std::uint64_t id = 0;
   core::piece_id piece;
   std::uint32_t offset = 0;
   std::vector<std::uint8_t> data;
   bool operator==(const block&) const = default;
};
struct cancel {
   std::uint64_t id = 0;
   bool operator==(const cancel&) const = default;
};
struct reject {
   std::uint64_t id = 0;
   rejection reason = rejection::unavailable;
   bool operator==(const reject&) const = default;
};
struct hash_request {
   std::uint64_t id = 0;
   core::piece_id piece;
   bool operator==(const hash_request&) const = default;
};
struct hash_response {
   std::uint64_t id = 0;
   core::piece_id piece;
   core::sha256 hash;
   core::piece_proof proof;
   bool operator==(const hash_response&) const;
};

// V1 wire discriminants are the zero-based indices in THIS frozen order.
struct peer_message {
   std::variant<handshake, inventory, have, interest, choke, request, block, cancel, reject, hash_request,
                hash_response>
       body;
   bool operator==(const peer_message&) const = default;
};

void validate(const peer_message&);
// Manifest must already have passed Core canonical descriptor validation.
// Checks geometry, inventory bits and proof/root binding, not request ownership.
void validate(const peer_message&, const core::descriptor&);
[[nodiscard]] std::vector<std::uint8_t> encode_message(const peer_message&);
[[nodiscard]] peer_message decode_message(std::span<const std::uint8_t>, forge::raw::unpack_limits limits = {});

BOOST_DESCRIBE_ENUM(rejection, unavailable, choked, invalid_request, canceled)
BOOST_DESCRIBE_STRUCT(handshake, (), (version, swarm, realm, capabilities))
BOOST_DESCRIBE_STRUCT(inventory, (), (pieces))
BOOST_DESCRIBE_STRUCT(have, (), (piece))
BOOST_DESCRIBE_STRUCT(interest, (), (interested))
BOOST_DESCRIBE_STRUCT(choke, (), (choked))
BOOST_DESCRIBE_STRUCT(request, (), (id, piece, offset, length))
BOOST_DESCRIBE_STRUCT(block, (), (id, piece, offset, data))
BOOST_DESCRIBE_STRUCT(cancel, (), (id))
BOOST_DESCRIBE_STRUCT(reject, (), (id, reason))
BOOST_DESCRIBE_STRUCT(hash_request, (), (id, piece))
BOOST_DESCRIBE_STRUCT(hash_response, (), (id, piece, hash, proof))
BOOST_DESCRIBE_STRUCT(peer_message, (), (body))

} // namespace forge::swarm::protocol

export namespace forge::raw {

template <> struct enum_wire_type<forge::swarm::protocol::rejection> {
   using type = std::uint8_t;
   static constexpr bool shared = true;
};

} // namespace forge::raw
