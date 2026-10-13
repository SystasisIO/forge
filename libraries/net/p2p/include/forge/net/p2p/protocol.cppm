module;

#include <compare>
#include <cstdint>
#include <string>

export module forge.net.p2p.protocol;

export namespace forge::net::p2p {

struct protocol_id {
   std::string value;

   [[nodiscard]] friend bool operator==(const protocol_id&, const protocol_id&) noexcept = default;
   [[nodiscard]] friend auto operator<=>(const protocol_id&, const protocol_id&) noexcept = default;
};

struct capability_set {
   std::uint64_t bits = 0;

   [[nodiscard]] constexpr bool has(std::uint64_t flag) const noexcept {
      return (bits & flag) == flag;
   }

   constexpr void add(std::uint64_t flag) noexcept {
      bits |= flag;
   }
};

namespace capabilities {
inline constexpr std::uint64_t direct_quic = 1ULL << 0;
inline constexpr std::uint64_t relay = 1ULL << 1;
inline constexpr std::uint64_t peer_exchange = 1ULL << 2;
// Bit 3 was the pre-profile DHT capability. Keep the gap so the remaining
// persisted diagnostic bits retain their numeric values.
inline constexpr std::uint64_t autonat = 1ULL << 4;
inline constexpr std::uint64_t hole_punching = 1ULL << 5;
inline constexpr std::uint64_t relay_reservation = 1ULL << 6;
inline constexpr std::uint64_t rendezvous = 1ULL << 7;
inline constexpr std::uint64_t pubsub = 1ULL << 8;
} // namespace capabilities

namespace builtins {
inline const protocol_id echo{.value = "/forge/p2p/echo/1"};
inline const protocol_id ping{.value = "/ipfs/ping/1.0.0"};
inline const protocol_id identify{.value = "/ipfs/id/1.0.0"};
inline const protocol_id identify_push{.value = "/ipfs/id/push/1.0.0"};
inline const protocol_id peer_exchange{.value = "/forge/p2p/peer-exchange/1"};
inline const protocol_id autonat_v1{.value = "/libp2p/autonat/1.0.0"};
inline const protocol_id autonat_v2_dial_request{.value = "/libp2p/autonat/2/dial-request"};
inline const protocol_id autonat_v2_dial_back{.value = "/libp2p/autonat/2/dial-back"};
inline const protocol_id relay_hop{.value = "/libp2p/circuit/relay/0.2.0/hop"};
inline const protocol_id relay_stop{.value = "/libp2p/circuit/relay/0.2.0/stop"};
inline const protocol_id dcutr{.value = "/libp2p/dcutr"};
inline const protocol_id kad_dht{.value = "/ipfs/kad/1.0.0"};
inline const protocol_id rendezvous{.value = "/rendezvous/1.0.0"};
inline const protocol_id meshsub_v11{.value = "/meshsub/1.1.0"};
inline const protocol_id meshsub_v10{.value = "/meshsub/1.0.0"};
inline const protocol_id meshsub_v12{.value = "/meshsub/1.2.0"};
inline const protocol_id meshsub_v13{.value = "/meshsub/1.3.0"};
} // namespace builtins

} // namespace forge::net::p2p
