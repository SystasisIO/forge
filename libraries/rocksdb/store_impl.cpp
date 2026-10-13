module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/transaction_db.h>

module forge.rocksdb.store;

import forge.exceptions;
import forge.log.logger;
import forge.rocksdb.exceptions;

#include "details/native.hxx"
#include "details/native_diagnostics.hxx"
#include "details/store_impl.hxx"

namespace forge::rocksdb {

store::impl::impl(config value)
    : settings{std::move(value)}, diagnostics{std::make_shared<detail::native_diagnostics>()} {
   static_cast<void>(detail::native_diagnostics::process());
   if (settings.path.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "RocksDB path must not be empty");
   }

   auto family_options = std::unordered_map<std::string, column_family_config>{};
   for (auto family : settings.column_families) {
      if (family.name.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "RocksDB column family name must not be empty");
      }
      family_options.emplace(family.name, std::move(family));
   }
   family_options.try_emplace("default", column_family_config{"default"});

   auto names = std::vector<std::string>{};
   names.reserve(family_options.size());
   for (const auto& [name, options] : family_options) {
      static_cast<void>(options);
      if (name != "default") {
         names.push_back(name);
      }
   }
   std::ranges::sort(names);
   names.erase(std::unique(names.begin(), names.end()), names.end());
   names.insert(names.begin(), "default");

   const auto path = std::filesystem::path{settings.path};
   if (const auto parent = path.parent_path(); !parent.empty()) {
      try {
         std::filesystem::create_directories(parent);
      } catch (const std::filesystem::filesystem_error& error) {
         FORGE_THROW_EXCEPTION(exceptions::io_error,
                               "failed to create RocksDB parent directory",
                               forge::exceptions::ctx("path", path.string()),
                               forge::exceptions::ctx("parent", parent.string()),
                               forge::exceptions::ctx("reason", error.what()));
      } catch (const std::exception& error) {
         FORGE_THROW_EXCEPTION(exceptions::internal_error,
                               "failed to create RocksDB parent directory",
                               forge::exceptions::ctx("path", path.string()),
                               forge::exceptions::ctx("parent", parent.string()),
                               forge::exceptions::ctx("reason", error.what()));
      }
   }

   auto db_options = ::rocksdb::DBOptions{};
   db_options.create_if_missing = settings.create_if_missing;
   db_options.create_missing_column_families = settings.create_missing_column_families;
   db_options.info_log = diagnostics;
   db_options.listeners.push_back(diagnostics);

   std::vector<std::string> existing_names;
   const auto list_status = ::rocksdb::DB::ListColumnFamilies(db_options, path.string(), &existing_names);
   diagnostics->drain();
   if (list_status.ok()) {
      for (const auto& name : existing_names) {
         if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
            family_options.try_emplace(name, column_family_config{name});
         }
      }
   } else if (!settings.create_if_missing && !list_status.IsIOError()) {
      detail::throw_if_error(list_status, "failed to list RocksDB column families");
   }

   std::vector<::rocksdb::ColumnFamilyDescriptor> descriptors;
   descriptors.reserve(names.size());
   for (const auto& name : names) {
      descriptors.emplace_back(name, detail::to_native_options(family_options.at(name)));
   }

   std::vector<::rocksdb::ColumnFamilyHandle*> opened_handles;
   ::rocksdb::TransactionDB* opened_db = nullptr;
   const auto open_status = ::rocksdb::TransactionDB::Open(
      db_options,
      ::rocksdb::TransactionDBOptions{},
      path.string(),
      descriptors,
      &opened_handles,
      &opened_db);
   diagnostics->drain();
   detail::throw_if_error(open_status, "failed to open RocksDB TransactionDB store");

   db.reset(opened_db);
   for (std::size_t index = 0; index < names.size(); ++index) {
      handles.emplace(names[index], opened_handles[index]);
   }
   ilog("forge.db.rocksdb", "store opened",
        ("event", "store.opened")("families", handles.size()));
}

store::impl::~impl() {
   for (auto& [name, handle] : handles) {
      static_cast<void>(name);
      if (handle != nullptr && db != nullptr) {
         static_cast<void>(db->DestroyColumnFamilyHandle(handle));
      }
   }
   db.reset();
   drain_diagnostics();
   ilog("forge.db.rocksdb", "store closed", ("event", "store.closed"));
}

::rocksdb::ColumnFamilyHandle* store::impl::require_handle(const family& column_family) const {
   const auto iterator = handles.find(column_family.name);
   if (iterator == handles.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument,
                            "RocksDB column family is not open",
                            forge::exceptions::ctx("family", column_family.name));
   }
   return iterator->second;
}

void store::impl::drain_diagnostics() const noexcept {
   if (diagnostics->idle()) {
      diagnostics->drain();
      detail::drain_process_diagnostics();
   }
}

void store::impl::acquire_transaction() const noexcept {
   diagnostics->acquire_transaction();
}

void store::impl::release_transaction() const noexcept {
   diagnostics->release_transaction();
}

void store::impl::record_commit(bool committed) const noexcept {
   diagnostics->record_commit(committed);
}

} // namespace forge::rocksdb
