#include <array>
#include <cstdint>
#include <type_traits>

import forge.swarm.db.content;
import forge.swarm.db.schema;
import forge.raw.raw;
import forge.db.object.object;

int main() {
   namespace core = forge::swarm::core;
   namespace db = forge::swarm::db;
   static_assert(forge::db::object::application_object_value<db::content>);
   const auto bytes = std::array<std::uint8_t, 1>{'a'};
   auto row = db::content{};
   row.manifest = {core::descriptor_version::v1, core::block_size,
      {{{"a"}, 1U, core::hash_file(bytes, core::block_size).root, {}}}};
   row.swarm = core::calculate_swarm_id(row.manifest);
   row.locator = "consumer/file";
   const auto encoded = forge::raw::pack(row);
   const auto decoded = forge::raw::unpack<db::content>(encoded);
   return decoded.swarm == row.swarm && decoded.locator == row.locator &&
      decoded.expires_at == INT64_MAX ? 0 : 1;
}
