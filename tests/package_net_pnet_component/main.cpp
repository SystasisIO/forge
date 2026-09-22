#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <utility>

import forge.net.pnet.network_fingerprint;
import forge.net.pnet.protector;

template <typename type>
concept exposes_raw_key_span = requires(const type& value) {
   { value.span() } -> std::convertible_to<std::span<const std::uint8_t>>;
};

template <typename type>
concept exposes_raw_key_bytes = requires(const type& value) {
   { value.bytes() } -> std::convertible_to<std::span<const std::uint8_t>>;
};

static_assert(!exposes_raw_key_span<forge::net::pnet::pre_shared_key>);
static_assert(!exposes_raw_key_bytes<forge::net::pnet::pre_shared_key>);

int main() {
   auto key = forge::net::pnet::pre_shared_key{std::array<std::uint8_t, 32>{}};
   const auto fingerprint = key.fingerprint();
   const auto network = key.network_fingerprint();
   auto protector = forge::net::pnet::protector{std::move(key)};
   return fingerprint.bytes.size() == 32U && network.bytes.size() == 16U &&
                  protector.fingerprint() == fingerprint && protector.network_fingerprint() == network
              ? 0 : 1;
}
