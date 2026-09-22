module;

#include <array>
#include <cstddef>
#include <cstdint>

export module forge.net.pnet.network_fingerprint;

export namespace forge::net::pnet {

struct network_fingerprint {
   inline static constexpr auto byte_size = std::size_t{16};

   std::array<std::uint8_t, byte_size> bytes{};

   friend constexpr bool operator==(const network_fingerprint&, const network_fingerprint&) = default;
};

} // namespace forge::net::pnet
