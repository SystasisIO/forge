module;

#include <forge/exceptions/macros.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rocksdb/iterator.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>

module forge.rocksdb.store;

import forge.exceptions;
import forge.rocksdb.exceptions;

#include "details/native.hxx"
#include "details/snapshot_impl.hxx"
#include "details/store_impl.hxx"
#include "details/transaction_impl.hxx"

namespace forge::rocksdb {

store::store(config value) try : impl_{std::make_shared<impl>(std::move(value))} {
} catch (...) {
   detail::drain_process_diagnostics();
   throw;
}
store::~store() = default;
store::store(store&&) noexcept = default;
store& store::operator=(store&&) noexcept = default;

std::optional<std::vector<std::byte>>
store::get(family column_family, std::vector<std::byte> key, read_options options) {
   std::string value;
   const auto status = impl_->db->Get(
      detail::to_native_options(options),
      impl_->require_handle(column_family),
      detail::to_slice(key),
      &value);
   if (status.IsNotFound()) {
      return std::nullopt;
   }
   detail::throw_if_error(status, "failed to get RocksDB value");
   auto bytes = std::vector<std::byte>{};
   bytes.resize(value.size());
   std::memcpy(bytes.data(), value.data(), value.size());
   return bytes;
}

void store::put(family column_family,
                std::vector<std::byte> key,
                std::vector<std::byte> value,
                write_options options) {
   auto transaction = begin(options);
   transaction.put(std::move(column_family), std::move(key), std::move(value));
   transaction.commit();
}

void store::erase(family column_family, std::vector<std::byte> key, write_options options) {
   auto transaction = begin(options);
   transaction.erase(std::move(column_family), std::move(key));
   transaction.commit();
}

void store::write(std::vector<operation> operations, write_options options) {
   auto transaction = begin(options);
   for (const auto& operation : operations) {
      switch (operation.kind) {
         case operation_kind::put:
            transaction.put(operation.column_family, operation.key, operation.value);
            break;
         case operation_kind::erase:
            transaction.erase(operation.column_family, operation.key);
            break;
      }
   }
   transaction.commit();
}

std::vector<entry> store::scan(family column_family,
                               std::vector<std::byte> prefix,
                               read_options options) {
   auto iterator = std::unique_ptr<::rocksdb::Iterator>{
      impl_->db->NewIterator(
         detail::to_native_options(options), impl_->require_handle(column_family)),
   };

   auto values = std::vector<entry>{};
   for (iterator->Seek(detail::to_slice(prefix)); iterator->Valid(); iterator->Next()) {
      auto key = detail::bytes_from_slice(iterator->key());
      if (!detail::starts_with(key, prefix)) {
         break;
      }
      values.push_back(entry{.key = std::move(key), .value = detail::bytes_from_slice(iterator->value())});
   }
   detail::throw_if_error(iterator->status(), "failed to scan RocksDB prefix");
   return values;
}

scan_result store::scan_page(family column_family, scan_request request) {
   auto iterator = std::unique_ptr<::rocksdb::Iterator>{
      impl_->db->NewIterator(
         detail::to_native_options(request.options), impl_->require_handle(column_family)),
   };
   return detail::read_scan_page(
      std::move(iterator), std::move(request), "failed to scan RocksDB prefix page");
}

transaction store::begin(write_options options) {
   auto native = std::unique_ptr<::rocksdb::Transaction>{
      impl_->db->BeginTransaction(
         detail::to_native_options(options), ::rocksdb::TransactionOptions{}),
   };
   if (native == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::internal_error, "failed to begin RocksDB transaction");
   }
   return transaction{std::make_unique<transaction::impl>(impl_, std::move(native))};
}

snapshot store::begin_snapshot() {
   const auto native = impl_->db->GetSnapshot();
   if (native == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::internal_error, "failed to begin RocksDB snapshot");
   }
   return snapshot{std::make_unique<snapshot::impl>(impl_, native)};
}

void store::flush_wal(bool sync) {
   const auto status = impl_->db->FlushWAL(sync);
   impl_->drain_diagnostics();
   detail::throw_if_error(status, "failed to flush RocksDB WAL");
}

} // namespace forge::rocksdb
