#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

import forge.package.db.registration.catalog;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.api.core.registry;
import forge.config.core.decode;
import forge.config.core.value;
import forge.db.core.exceptions;
import forge.db.object.exceptions;
import forge.plugins.db.store.plugin;
import forge.plugins.db.store.api;

int package_api_contract_checks();

namespace {

template <typename Exception, typename Work> void expect_error(Work work) {
   auto denied = false;
   try {
      work();
   } catch (const Exception&) {
      denied = true;
   }
   FORGE_ASSERT(denied, "Native registration returned a different admission result");
}

boost::asio::awaitable<void> require_closed_guards(forge::plugins::db::store::store_handle store) {
   const auto objects = store.objects();
   auto retained = co_await store.begin_read();
   auto busy = false;
   try {
      static_cast<void>(co_await store.close_for_activation());
   } catch (const forge::db::core::exceptions::driver_busy&) {
      busy = true;
   }
   FORGE_ASSERT(busy, "Real MDBX snapshot did not hold strict close incomplete");
   expect_error<forge::plugins::db::store::exceptions::stopped>([&] {
      objects.register_object(package_registration::metadata());
   });
   expect_error<forge::plugins::db::store::exceptions::stopped>([&] {
      package_registration::register_legacy(objects);
   });
   retained = {};
   auto closed = co_await store.close_for_activation();
   FORGE_ASSERT(!closed.state().published, "Registration consumer unexpectedly published its temporary store");
   expect_error<forge::plugins::db::store::exceptions::stopped>([&] {
      objects.register_object(package_registration::metadata());
   });
   expect_error<forge::plugins::db::store::exceptions::stopped>([&] {
      package_registration::register_legacy(objects);
   });
}

} // namespace

int main() {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto root = std::filesystem::temp_directory_path() / ("forge-native-registration-" + std::to_string(nonce));
   auto app = std::unique_ptr<forge::app::application_shell>{};
   try {
      FORGE_ASSERT(package_api_contract_checks() == 0, "Existing installed DB plugin API contracts regressed");
      FORGE_ASSERT(std::filesystem::create_directory(root), "Temporary registration namespace already exists");
      std::filesystem::permissions(root, std::filesystem::perms::owner_all);
      const auto stage = root / "state";
      FORGE_ASSERT(std::filesystem::create_directory(stage), "Cannot create owned MDBX test root");
      std::filesystem::permissions(stage, std::filesystem::perms::owner_all);
      auto store = forge::plugins::db::store::store_handle{};
      auto registered = false;
      auto builder = forge::app::application_builder{};
      builder.name("installed-native-registration")
          .runtime({.worker_threads = 2})
          .compute({.worker_threads = 2})
          .plugin(forge::plugins::db::store::plugin::descriptor())
          .after_initialize([&](const forge::app::application_context& context) -> boost::asio::awaitable<void> {
             auto api = context.api_view().get<forge::plugins::db::store::api>(forge::plugins::db::store::api::ref());
             store = co_await api->store("native");
             const auto metadata = package_registration::metadata();
             expect_error<forge::plugins::db::store::exceptions::stopped>([&] {
                forge::plugins::db::store::object_handle{}.register_object(metadata);
             });
             auto objects = store.objects();
             objects.register_object(metadata);
             expect_error<forge::db::object::exceptions::invalid_descriptor>([&] {
                objects.register_object(metadata);
             });
             expect_error<forge::db::object::exceptions::invalid_descriptor>([&] {
                objects.register_object(package_registration::conflicting_metadata());
             });
             package_registration::register_legacy(objects);
             expect_error<forge::db::object::exceptions::invalid_descriptor>([&] {
                objects.register_object(package_registration::legacy_metadata());
             });
             registered = true;
             co_return;
          });
      app = std::move(builder).build();
      auto settings = forge::plugins::db::store::config{};
      settings.stores.push_back({.name = "native", .driver = "mdbx", .path = stage.string(),
          .object = forge::plugins::db::store::object_layer_config{},
          .mdbx = forge::plugins::db::store::mdbx_driver_config{
              .durability = "durable-sync", .map = {.upper_size = std::uint64_t{64U * 1024U * 1024U}}}});
      app->configure(forge::config::core::encode(settings, "plugins.db.store"));
      forge::asio::blocking::run(app->runtime(), app->startup());
      FORGE_ASSERT(registered, "Metadata registration did not run through the actual setup phase");
      forge::asio::blocking::run(app->runtime(), package_registration::create_and_read(store));
      forge::asio::blocking::run(app->runtime(), require_closed_guards(store));
      forge::asio::blocking::run(app->runtime(), app->shutdown());
      app.reset();
      std::filesystem::remove_all(root);
      std::cout << "PASS installed multi-TU native registration / typed read-write-index / duplicate-model / strict-close\n";
      return 0;
   } catch (const std::exception& error) {
      if (app) {
         try {
            forge::asio::blocking::run(app->runtime(), app->shutdown());
         } catch (...) {
            std::cerr << "HARNESS_ERROR registration consumer shutdown incomplete; owned root " << root << '\n';
            std::terminate();
         }
      }
      std::cerr << "FAIL native registration consumer: " << error.what() << "; owned root preserved " << root << '\n';
      return 1;
   }
}
