module;

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

module test.api_chain.counter_client;

import package.chain_api_component.test_support;
import forge.plugins.db.store.api;
import forge.plugins.db.store.plugin;
import forge.db.core.record;
import forge.config.core.decode;

namespace demo {
namespace protocol = forge::chain::protocol;

namespace storage = forge::plugins::db::store;
using package_chain_api_component::require;
forge::config::core::document databases(const std::filesystem::path& root) {
   storage::config stores;
   for (const char* name : {"unrelated", "counter"}) {
      const auto path = root / name;
      std::filesystem::create_directories(path);
      std::filesystem::permissions(path, std::filesystem::perms::owner_all);
      stores.stores.push_back({.name = name,
                               .driver = "mdbx",
                               .path = path.string(),
                               .families = {"counter.intent"},
                               .object = storage::object_layer_config{.id_allocation = "transactional"},
                               .mdbx = storage::mdbx_driver_config{.durability = "durable-sync",
                                                                   .map = {.upper_size = 64U * 1024U * 1024U}}});
   }
   return forge::config::core::encode(stores, "plugins.db.store");
}

boost::asio::awaitable<protocol::transaction_id> admitted_transaction(forge::api::core::view apis) {
   const auto stores = apis.get<storage::api>(storage::api::ref());
   const auto key = forge::db::core::record_key{{std::byte{1}}};
   auto unused = co_await stores->store("unrelated");
   auto other = co_await unused.begin_read();
   require(!(co_await other.get(forge::db::core::family{"counter.intent"}, key)),
           "Named store not selected for the outbox contains a counter admission");
   auto selected = co_await stores->store("counter");
   auto snapshot = co_await selected.begin_read();
   auto saved = co_await snapshot.get(forge::db::core::family{"counter.intent"}, key);
   require(saved && saved->size() == protocol::transaction_id::byte_size, "Selected MDBX store lost atomic admission");
   co_return protocol::transaction_id{reinterpret_cast<const char*>(saved->data()), saved->size()};
}

forge::app::plugin_descriptor database_plugin() {
   return storage::plugin::descriptor();
}

} // namespace demo
