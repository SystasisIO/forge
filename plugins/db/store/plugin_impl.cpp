module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module forge.plugins.db.store.plugin;

import forge.api.core.binding;
import forge.app.plugin_context;
import forge.asio.affine;
import forge.asio.exceptions;
import forge.config.core.component;
import forge.config.core.decode;
import forge.db.blob.store;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.core.record;
import forge.exceptions;
import forge.log.logger;
import forge.db.object.store;
import forge.db.revision.store;
import forge.plugins.db.store.exceptions;
import forge.plugins.db.store.types;

#if FORGE_PLUGINS_DB_STORE_HAS_ROCKSDB
import forge.db.rocksdb.driver;
import forge.rocksdb.types;
#endif

#if FORGE_PLUGINS_DB_STORE_HAS_MDBX
import forge.db.mdbx.driver;
#endif

#include "details/plugin_impl.hxx"
#include "details/config.hxx"

namespace forge::plugins::db::store {
plugin::impl::impl(forge::app::plugin_lifecycle value) : lifecycle{std::move(value)} {}


void plugin::impl::configure(config value) {
   detail::validate_config(value);

   auto configured = std::unordered_map<std::string, std::shared_ptr<managed_store>>{};
   for (const auto& item : value.stores) {
      auto record = std::make_shared<managed_store>();
      record->name = item.name;
      record->driver_name = item.driver;
      if (item.driver == "mdbx") {
         record->durability = item.mdbx.value_or(mdbx_driver_config{}).durability;
      }
      record->path = item.path;
      record->families = item.families;
      record->options = detail::parse_options(item);
      configured.emplace(record->name, std::move(record));
   }

   const auto generation = lifecycle.snapshot().generation;
   auto count = std::size_t{};
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      reject_started_setup();
      settings = std::move(value);
      enabled = true;
      stores.swap(configured);
      count = stores.size();
   })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store configuration was stopped");
   }
   dlog("forge.db.store", "store configuration accepted",
        ("event", "configuration.accepted")("stores", count));
}

void plugin::impl::initialize() {}

void plugin::impl::reject_started_setup() const {
   const auto state = current.load();
   if (state == phase::opening || state == phase::ready || state == phase::closed) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db stores can only be added before startup");
   }
}

void plugin::impl::reject_duplicate_name(const std::string& name) const {
   if (stores.contains(name)) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_store, "db store name is already registered",
                            forge::exceptions::ctx("store", name));
   }
}

void plugin::impl::add_store(std::string name, std::shared_ptr<forge::db::core::driver> driver, store_options options) {
   if (name.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store name must not be empty");
   }
   if (!driver) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store driver must not be null",
                            forge::exceptions::ctx("store", name));
   }
   if (options.revision && !options.object) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store revision layer requires object layer",
                            forge::exceptions::ctx("store", name));
   }
   if (!options.object && !options.blob) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store must configure object or blob layer",
                            forge::exceptions::ctx("store", name));
   }
   detail::validate_options(options, name, true);

   auto record = std::make_shared<managed_store>();
   record->name = std::move(name);
   record->driver_name = "custom";
   record->options = std::move(options);
   record->driver = std::move(driver);

   const auto object = record->options.object.has_value();
   const auto blob = record->options.blob.has_value();
   const auto revision = record->options.revision.has_value();
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      reject_started_setup();
      reject_duplicate_name(record->name);
      stores.emplace(record->name, record);
   })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store registration was stopped");
   }
   dlog("forge.db.store", "external store registered",
        ("event", "store.registered")("store", record->name)("backend", "custom")("owns_driver", false)
        ("object", object)("blob", blob)("revision", revision));
}

boost::asio::awaitable<void> plugin::impl::open() {
   auto pending = std::vector<pending_open>{};
   auto restore_phase = phase::collecting;
   const auto generation = lifecycle.snapshot().generation;
   {
      auto lock = std::scoped_lock{mutex};
      const auto state = current.load();
      if (state == phase::ready) {
         co_return;
      }
      if (state == phase::closed) {
         FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopping");
      }
      if (!enabled) {
         current.store(phase::ready);
         co_return;
      }
      if (state != phase::collecting) {
         FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store plugin is not initialized");
      }

      restore_phase = state;
      current.store(phase::opening);
      pending.reserve(stores.size());
      for (const auto& [name, record] : stores) {
         auto item = pending_open{
             .name = name,
             .options = record->options,
             .owns_driver = record->owns_driver,
             .driver = record->driver,
         };
         if (!item.driver) {
            const auto configured =
                std::ranges::find_if(settings.stores, [&](const auto& value) { return value.name == name; });
            if (configured == settings.stores.end()) {
               current.store(restore_phase);
               FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store configured store is not registered",
                                     forge::exceptions::ctx("store", name));
            }
            item.config = *configured;
         }
         pending.push_back(std::move(item));
      }
   }

   const auto* failed = static_cast<const pending_open*>(nullptr);
   try {
      for (auto& item : pending) {
         failed = &item;
         if (!item.driver) {
            item.driver = co_await make_configured_driver(*item.config);
            item.owns_driver = true;
         }
         if (!item.driver) {
            FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store has no driver",
                                  forge::exceptions::ctx("store", item.name));
         }
         if (item.options.object) {
            auto objects = co_await forge::db::object::store::open(
                item.driver, forge::db::object::store::config{.family = item.options.object->family},
                item.options.object->runtime);
            item.objects = std::make_shared<forge::db::object::store>(std::move(objects));
         }
         if (item.options.revision) {
            if (!item.objects) {
               FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store revision layer requires object layer",
                                     forge::exceptions::ctx("store", item.name));
            }
            auto revisions = co_await forge::db::revision::store::open(item.driver, *item.objects);
            item.revisions = std::make_shared<forge::db::revision::store>(std::move(revisions));
         }
         if (item.options.blob) {
            item.blobs =
                std::make_shared<forge::db::blob::store>(item.driver, forge::db::blob::store::config{
                                                                          .data_family = item.options.blob->data_family,
                                                                          .refs_family = item.options.blob->refs_family,
                                                                      });
         }
      }

      if (!lifecycle.try_admit(generation, [&] {
         auto lock = std::scoped_lock{mutex};
         const auto state = current.load();
         if (state == phase::closed) {
            FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopping");
         }
         if (state != phase::opening) {
            FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store plugin initialization state changed");
         }
         for (auto& item : pending) {
            failed = &item;
            const auto found = stores.find(item.name);
            if (found == stores.end()) {
               FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store is no longer registered",
                                     forge::exceptions::ctx("store", item.name));
            }
            auto& record = found->second;
            record->owns_driver = item.owns_driver;
            record->driver = std::move(item.driver);
            record->objects = std::move(item.objects);
            record->blobs = std::move(item.blobs);
            record->revisions = std::move(item.revisions);
            record->opened = true;
            record->started = false;
         }
         current.store(phase::ready);
      })) {
         FORGE_THROW_EXCEPTION(exceptions::stopped, "db store opening was stopped");
      }
   } catch (...) {
      const auto stopping = lifecycle.snapshot().stop_requested;
      auto lock = std::unique_lock{mutex};
      if (current.load() == phase::opening) {
         current.store(restore_phase);
      }
      lock.unlock();
      const auto failed_store = failed ? std::string_view{failed->name} : std::string_view{};
      const auto failed_backend = failed
         ? (failed->config ? std::string_view{failed->config->driver} : std::string_view{"custom"})
         : std::string_view{};
      auto canceled = false;
      try {
         throw;
      } catch (const forge::asio::exceptions::canceled&) {
         canceled = true;
      } catch (...) {
      }
      if (canceled) {
         dlog("forge.db.store", "store composition canceled", ("event", "composition.canceled")
              ("store", failed_store)("backend", failed_backend));
      } else if (!stopping) {
         elog("forge.db.store", "store composition failed", ("event", "composition.failed")
              ("store", failed_store)("backend", failed_backend));
      }
      throw;
   }
   const auto owned_count = static_cast<std::size_t>(
       std::ranges::count_if(pending, [](const auto& item) { return item.owns_driver; }));
   dlog("forge.db.store", "store composition ready",
        ("event", "composition.ready")("stores", pending.size())("owned_drivers", owned_count));
   co_return;
}

void plugin::impl::start() {
   const auto generation = lifecycle.snapshot().generation;
   auto count = std::size_t{};
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (current.load() != phase::ready) {
         FORGE_THROW_EXCEPTION(exceptions::startup_failed, "db store composition is not ready");
      }
      for (const auto& [_, record] : stores) {
         if (!record->opened) {
            FORGE_THROW_EXCEPTION(exceptions::startup_failed, "db store is not ready",
                                  forge::exceptions::ctx("store", record->name));
         }
      }
      for (auto& [_, record] : stores) { record->started = true; }
      count = stores.size();
   })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store startup was stopped");
   }
   ilog("forge.db.store", "store plugin started", ("event", "plugin.started")("stores", count));
}

// Existing handles remain available for dependent plugins' persistence drain.
void plugin::impl::request_stop() noexcept {}

boost::asio::awaitable<void> plugin::impl::close() {
   auto owned = std::vector<std::shared_ptr<forge::db::core::driver>>{};
   auto retired = std::vector<opened_store>{};
   {
      auto lock = std::scoped_lock{mutex};
      if (shutdown_completed) {
         co_return;
      }
      owned.reserve(stores.size());
      retired.reserve(stores.size());
      for (auto& [_, record] : stores) {
         retired.push_back(opened_store{
            .driver = {},
            .objects = std::move(record->objects),
            .blobs = std::move(record->blobs),
            .revisions = std::move(record->revisions),
         });
         if (record->owns_driver && record->driver) {
            owned.push_back(record->driver);
         } else {
            retired.back().driver = std::move(record->driver);
            record->owns_driver = false;
         }
         record->opened = false;
         record->started = false;
      }
      current.store(phase::closed);
   }
   // Backend destruction can emit diagnostics and must not run under the plugin mutex.
   retired.clear();

   auto first_error = std::exception_ptr{};
   for (auto& driver : owned) {
      auto released = false;
      try {
         co_await driver->async_close();
         released = true;
      } catch (const forge::db::core::exceptions::driver_busy&) {
         // Existing sessions keep the backend and any managed lane alive.
         released = true;
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
      }
      if (released) {
         const auto lock = std::scoped_lock{mutex};
         for (auto& [_, record] : stores) {
            if (record->owns_driver && record->driver == driver) {
               // The local batch pins this owner until after the mutex is released.
               record->driver.reset();
               record->owns_driver = false;
            }
         }
      }
   }

   if (first_error) {
      elog("forge.db.store", "owned driver shutdown failed", ("event", "shutdown.failed")
           ("owned_drivers", owned.size()));
      std::rethrow_exception(first_error);
   }
   auto stopped_now = false;
   {
      auto lock = std::scoped_lock{mutex};
      stopped_now = !shutdown_completed;
      shutdown_completed = true;
   }
   if (stopped_now) {
      ilog("forge.db.store", "store plugin stopped", ("event", "plugin.stopped")
           ("owned_drivers", owned.size()));
   }
}

std::shared_ptr<managed_store> plugin::impl::find_store(const std::string& name) const {
   auto lock = std::scoped_lock{mutex};
   const auto found = stores.find(name);
   if (found == stores.end()) {
      return {};
   }
   return found->second;
}

std::shared_ptr<managed_store> plugin::impl::require_store(const std::string& name) const {
   auto record = find_store(name);
   if (!record) {
      FORGE_THROW_EXCEPTION(exceptions::unknown_store, "db store is not registered",
                            forge::exceptions::ctx("store", name));
   }
   return record;
}

opened_store plugin::impl::require_setup_store(const std::string& name) const {
   auto lock = std::scoped_lock{mutex};
   const auto found = stores.find(name);
   if (found == stores.end()) {
      FORGE_THROW_EXCEPTION(exceptions::unknown_store, "db store is not registered",
                            forge::exceptions::ctx("store", name));
   }

   const auto& record = found->second;
   const auto state = current.load();
   if ((state != phase::ready) || record->driver == nullptr ||
       !record->opened) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store is not ready", forge::exceptions::ctx("store", name));
   }

   return opened_store{
       .driver = record->driver,
       .objects = record->objects,
       .blobs = record->blobs,
       .revisions = record->revisions,
   };
}

opened_store plugin::impl::require_started_store(const std::string& name) const {
   auto lock = std::scoped_lock{mutex};
   const auto found = stores.find(name);
   if (found == stores.end()) {
      FORGE_THROW_EXCEPTION(exceptions::unknown_store, "db store is not registered",
                            forge::exceptions::ctx("store", name));
   }

   const auto& record = found->second;
   const auto state = current.load();
   if ((state != phase::ready) || record->driver == nullptr || !record->opened ||
       !record->started) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store is not started", forge::exceptions::ctx("store", name));
   }

   return opened_store{
       .driver = record->driver,
       .objects = record->objects,
       .blobs = record->blobs,
       .revisions = record->revisions,
   };
}

status plugin::impl::current_status() const {
   auto out = status{};
   auto lock = std::scoped_lock{mutex};
   out.stores.reserve(stores.size());
   for (const auto& [_, record] : stores) {
      out.stores.push_back(store_status{
          .name = record->name,
          .driver = record->driver_name,
          .durability = record->durability,
          .path = record->path,
          .families = record->families,
          .object = record->options.object.has_value(),
          .blob = record->options.blob.has_value(),
          .revision = record->options.revision.has_value(),
          .started = record->started,
      });
   }
   return out;
}

} // namespace forge::plugins::db::store

namespace forge::plugins::db::store {
namespace {

#if FORGE_PLUGINS_DB_STORE_HAS_ROCKSDB
[[nodiscard]] forge::rocksdb::compression_type parse_blob_compression(const std::string& value,
                                                                      const std::string& store_name) {
   if (value == "none") {
      return forge::rocksdb::compression_type::none;
   }
   if (value == "snappy") {
      return forge::rocksdb::compression_type::snappy;
   }
   if (value == "zlib") {
      return forge::rocksdb::compression_type::zlib;
   }
   if (value == "bzip2") {
      return forge::rocksdb::compression_type::bzip2;
   }
   if (value == "lz4") {
      return forge::rocksdb::compression_type::lz4;
   }
   if (value == "lz4hc") {
      return forge::rocksdb::compression_type::lz4hc;
   }
   if (value == "xpress") {
      return forge::rocksdb::compression_type::xpress;
   }
   if (value == "zstd") {
      return forge::rocksdb::compression_type::zstd;
   }

   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "db store blob compression type is unsupported",
                         forge::exceptions::ctx("store", store_name),
                         forge::exceptions::ctx("blob-compression-type", value));
}

[[nodiscard]] forge::rocksdb::blob_options to_rocksdb_blob_options(const blob_data_options& value,
                                                                   const std::string& store_name) {
   return forge::rocksdb::blob_options{
       .enable_blob_files = value.enable_blob_files,
       .min_blob_size = value.min_blob_size,
       .blob_file_size = value.blob_file_size,
       .blob_compression_type = parse_blob_compression(value.blob_compression_type, store_name),
       .enable_blob_garbage_collection = value.enable_blob_garbage_collection,
       .blob_garbage_collection_age_cutoff = value.blob_garbage_collection_age_cutoff,
   };
}

void add_family_once(std::vector<forge::rocksdb::column_family_config>& families,
                     forge::rocksdb::column_family_config value) {
   const auto found = std::ranges::find_if(families, [&](const auto& item) { return item.name == value.name; });
   if (found == families.end()) {
      families.push_back(std::move(value));
   }
}

[[nodiscard]] std::vector<forge::rocksdb::column_family_config> configured_families(const store_config& value) {
   auto families = std::vector<forge::rocksdb::column_family_config>{};
   for (const auto& family : value.families) {
      add_family_once(families, forge::rocksdb::column_family_config{family});
   }
   if (value.object) {
      add_family_once(families, forge::rocksdb::column_family_config{value.object->family});
   }
   if (value.blob) {
      auto data_family = forge::rocksdb::column_family_config{value.blob->data_family};
      data_family.blobs = to_rocksdb_blob_options(value.blob->data_blobs, value.name);
      add_family_once(families, std::move(data_family));
      add_family_once(families, forge::rocksdb::column_family_config{value.blob->refs_family});
   }
   return families;
}
#endif

#if FORGE_PLUGINS_DB_STORE_HAS_MDBX
[[nodiscard]] std::vector<std::string> configured_family_names(const store_config& value) {
   auto families = value.families;
   const auto add = [&](const std::string& family) {
      if (std::ranges::find(families, family) == families.end()) {
         families.push_back(family);
      }
   };
   if (value.object) {
      add(value.object->family);
   }
   if (value.blob) {
      add(value.blob->data_family);
      add(value.blob->refs_family);
   }
   return families;
}

[[nodiscard]] forge::db::mdbx::durability parse_mdbx_durability(const std::string& value) {
   if (value == "durable-sync") {
      return forge::db::mdbx::durability::durable_sync;
   }
   return forge::db::mdbx::durability::safe_nosync;
}

[[nodiscard]] forge::db::mdbx::config configured_mdbx(const store_config& value) {
   const auto options = value.mdbx.value_or(mdbx_driver_config{});
   return forge::db::mdbx::config{
       .path = value.path,
       .families = configured_family_names(value),
       .durability_mode = parse_mdbx_durability(options.durability),
       .map =
           forge::db::mdbx::geometry{
               .lower_size = options.map.lower_size,
               .current_size = options.map.current_size,
               .upper_size = options.map.upper_size,
               .growth_step = options.map.growth_step,
               .shrink_threshold = options.map.shrink_threshold,
               .page_size = options.map.page_size,
           },
       .max_readers = options.max_readers,
       .create_if_missing = value.create_if_missing,
       .create_missing_families = value.create_missing_column_families,
   };
}
#endif

} // namespace

boost::asio::awaitable<std::shared_ptr<forge::db::core::driver>>
plugin::impl::make_configured_driver(const store_config& value) {
   if (value.driver == "rocksdb") {
#if FORGE_PLUGINS_DB_STORE_HAS_ROCKSDB
      co_return std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
          .path = value.path,
          .families = configured_families(value),
          .create_if_missing = value.create_if_missing,
          .create_missing_column_families = value.create_missing_column_families,
      });
#else
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "db store rocksdb driver is not available in this build",
                            forge::exceptions::ctx("store", value.name));
#endif
   }

   if (value.driver == "mdbx") {
#if FORGE_PLUGINS_DB_STORE_HAS_MDBX
      auto native = configured_mdbx(value);
      const auto options = value.mdbx.value_or(mdbx_driver_config{});
      co_return co_await forge::db::mdbx::driver::open(
          std::move(native), forge::asio::affine::lane::options{
                                 .max_pending_operations = options.lane.max_pending_operations,
                                 .max_waiting_submissions = options.lane.max_waiting_submissions,
                                 .thread_name = options.lane.thread_name.value_or("db-mdbx-" + value.name),
                             });
#else
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "db store MDBX driver is not available in this build",
                            forge::exceptions::ctx("store", value.name));
#endif
   }

   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "db store driver is unsupported",
                         forge::exceptions::ctx("store", value.name), forge::exceptions::ctx("driver", value.driver));
}

} // namespace forge::plugins::db::store
