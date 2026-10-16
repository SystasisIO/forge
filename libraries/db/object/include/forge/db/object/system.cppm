module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

export module forge.db.object.system;

import forge.db.ids.object_id;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.index;
import forge.db.object.store;
import forge.db.object.transaction;

export namespace forge::db::object::system {

class access final {
 public:
   [[nodiscard]] static std::shared_ptr<forge::db::core::driver> driver(const store& value);
   [[nodiscard]] static forge::db::core::family family(const store& value);
   [[nodiscard]] static bool joined(const store& value,
                                    const forge::db::core::transaction& active);
   [[nodiscard]] static forge::db::core::record_key record_key(forge::db::ids::object_id id);
   static boost::asio::awaitable<void> require_pristine(store& value, transaction& active);
   // Fixed header+Revision-state0 shape only. Revision must also validate the
   // returned state through its owning codec and exact initial-state invariant.
   static boost::asio::awaitable<std::vector<std::byte>> inspect_pristine_revision(store& value, transaction& active);

   template <system_object_model Object>
   static void register_object(store& value) {
      value.template register_system_object<Object>();
   }

 private:
   static boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   inspect_pristine(store& value, transaction& active, bool with_revision);
};

} // namespace forge::db::object::system
