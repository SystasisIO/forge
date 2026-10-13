#include <concepts>
#include <cstdint>
#include <type_traits>

import forge.asio.affine;
import forge.db.core.driver;
import forge.db.mdbx.driver;
import forge.db.mdbx.diagnostics;
import forge.db.mdbx.exceptions;

int main() {
   forge::db::mdbx::diagnostics::claim_process();
   if (!forge::db::mdbx::diagnostics::installed()) {
      return 2;
   }
   forge::db::mdbx::diagnostics::drain();
   static_assert(std::derived_from<
                 forge::db::mdbx::driver,
                 forge::db::core::driver>);
   static_assert(std::is_move_constructible_v<forge::asio::affine::executor>);

   auto value = forge::db::mdbx::config{};
   value.path = "unused";
   value.families = {"objectdb", "blobdb.data", "blobdb.refs"};
   value.durability_mode = forge::db::mdbx::durability::durable_sync;
   value.map.upper_size = std::uint64_t{1024U * 1024U};
   return value.families.size() == 3U ? 0 : 1;
}
