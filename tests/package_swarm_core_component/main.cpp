#include <array>
#include <cstdint>

import forge.swarm.core.descriptor;
import forge.swarm.core.identity;
import forge.swarm.core.merkle;

int main() {
   namespace core = forge::swarm::core;
   const auto data = std::array<std::uint8_t, 1>{'a'};
   const auto hashes = core::hash_file(data, core::block_size);
   const auto value = core::descriptor{core::descriptor_version::v1, core::block_size,
      {{{"a"}, 1U, hashes.root, {}}}};
   const auto wire = core::encode_descriptor(value);
   const auto restored = core::decode_descriptor(wire);
   return restored == value && core::calculate_swarm_id(wire).value.str() ==
      "fce83635f30c1c132dc75136561c023f62db154a65bfc68e655668edd64069ab" ? 0 : 1;
}
