#include <array>
#include <cstdint>
#include <utility>
#include <variant>

import forge.swarm.protocol.metadata;
import forge.swarm.protocol.metadata_reader;
import forge.swarm.protocol.metadata_assembler;
import forge.swarm.protocol.peer_message;

int main() {
   namespace core = forge::swarm::core;
   namespace wire = forge::swarm::protocol;
   const auto hashes = core::hash_file(std::array<std::uint8_t, 1>{'a'}, core::block_size);
   const auto manifest =
       core::descriptor{core::descriptor_version::v1, core::block_size, {{{"a"}, 1U, hashes.root, {}}}};
   const auto value = wire::metadata{core::calculate_swarm_id(manifest), manifest};
   auto reader = wire::metadata_reader{value};
   auto assembler = wire::metadata_assembler{value.swarm};
   while (auto chunk = reader.next())
      assembler.append(wire::decode_metadata_chunk(wire::encode_metadata_chunk(*chunk)));
   if (!assembler.complete() || std::move(assembler).finish().manifest != manifest)
      return 1;
   const auto encoded = wire::encode_message(wire::peer_message{wire::choke{true}});
   return std::get<wire::choke>(wire::decode_message(encoded).body).choked ? 0 : 2;
}
