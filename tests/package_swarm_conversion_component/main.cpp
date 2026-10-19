#include <array>
#include <utility>

import forge.swarm.conversion.conversion;

int main() {
   namespace core = forge::swarm::core;
   const auto hashes = core::hash_file(std::array<std::uint8_t, 1>{'a'}, core::block_size);
   auto record = forge::swarm::db::content{};
   record.manifest = {core::descriptor_version::v1, core::block_size, {{{"a"}, 1U, hashes.root, {}}}};
   record.swarm = core::calculate_swarm_id(record.manifest);
   record.locator = "local-only";
   auto public_value = forge::core::conversion<forge::swarm::protocol::metadata>{record}.finish();
   const auto restored = forge::core::conversion<core::descriptor>{std::move(public_value)}.finish();
   return restored == record.manifest ? 0 : 1;
}
