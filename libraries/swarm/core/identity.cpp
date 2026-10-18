module;

#include <cstdint>
#include <span>

module forge.swarm.core.identity;

import forge.raw.raw;

namespace forge::swarm::core {

swarm_id calculate_swarm_id(const descriptor& value) {
   static_cast<void>(validate_descriptor(value));
   auto encoder = sha256::encoder{};
   forge::raw::pack(encoder, value);
   return {.value = encoder.result()};
}

swarm_id calculate_swarm_id(std::span<const std::uint8_t> bytes) {
   static_cast<void>(decode_descriptor(bytes));
   return {.value = sha256::hash(bytes)};
}

} // namespace forge::swarm::core
