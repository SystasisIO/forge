module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

module forge.package.db.registration.catalog;

import forge.package.db.registration.model;
import forge.db.object.exceptions;
import forge.db.object.transaction;
import forge.plugins.db.store.api;

namespace package_registration {

void register_legacy(const forge::plugins::db::store::object_handle& objects) {
   objects.register_object<legacy_index>();
}

boost::asio::awaitable<void> create_and_read(forge::plugins::db::store::store_handle store) {
   auto active = co_await store.objects().begin_transaction();
   const auto created = co_await active.create<item>([](auto& value) {
      value.name = "native-registration";
      value.bytes = 4097;
   });
   const auto legacy = co_await active.create<legacy_item>([](auto& value) {
      value.name = "template-registration";
   });
   co_await active.commit();

   const auto retained = co_await store.objects().get(created.id);
   FORGE_ASSERT(retained.id == created.id && retained.name == created.name && retained.bytes == 4097,
                "Registration metadata did not permit a real typed write/read");
   const auto indexed = co_await store.objects().index<item_index, by_name>().find(std::string{"native-registration"});
   FORGE_ASSERT(indexed && indexed->id == created.id && indexed->bytes == 4097,
                "Metadata registration changed the native secondary index path");
   const auto previous = co_await store.objects().get(legacy.id);
   FORGE_ASSERT(previous.name == "template-registration", "Template registration changed its existing behavior");

   auto denied = false;
   try {
      static_cast<void>(co_await store.objects().get<conflicting_index>(
          {.space = 1, .type = 77, .instance = created.id.instance}));
   } catch (const forge::db::object::exceptions::unregistered_object&) {
      denied = true;
   }
   FORGE_ASSERT(denied, "Native registration accepted another model with the same physical object ID");
}

} // namespace package_registration
