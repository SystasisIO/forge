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

module forge.rocksdb.store;

import forge.exceptions;
import forge.rocksdb.exceptions;

#include "details/native.hxx"
#include "details/store_impl.hxx"
#include "details/transaction_impl.hxx"

namespace forge::rocksdb {

transaction::transaction(std::unique_ptr<impl> impl_value) : impl_{std::move(impl_value)} {}

transaction::~transaction() {
   rollback_if_active();
}

transaction::transaction(transaction&&) noexcept = default;

transaction& transaction::operator=(transaction&& other) noexcept {
   if (this != &other) {
      rollback_if_active();
      impl_ = std::move(other.impl_);
   }
   return *this;
}

void transaction::rollback_if_active() noexcept {
   if (impl_ != nullptr && !impl_->finished && impl_->transaction != nullptr) {
      static_cast<void>(impl_->transaction->Rollback());
      impl_->finished = true;
   }
}

void transaction::ensure_active(std::string_view context) const {
   if (impl_ == nullptr || impl_->finished || impl_->transaction == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, std::string{context} + ": RocksDB transaction is closed");
   }
}

std::optional<std::vector<std::byte>>
transaction::get(family column_family, std::vector<std::byte> key, read_options options) {
   ensure_active("failed to get RocksDB transaction value");
   std::string value;
   const auto status = impl_->transaction->Get(
      detail::to_native_options(options),
      impl_->store->require_handle(column_family),
      detail::to_slice(key),
      &value);
   if (status.IsNotFound()) {
      return std::nullopt;
   }
   detail::throw_if_error(status, "failed to get RocksDB transaction value");
   auto bytes = std::vector<std::byte>{};
   bytes.resize(value.size());
   std::memcpy(bytes.data(), value.data(), value.size());
   return bytes;
}

std::optional<std::vector<std::byte>>
transaction::get_for_update(family column_family, std::vector<std::byte> key, read_options options) {
   ensure_active("failed to lock and get RocksDB transaction value");
   std::string value;
   const auto status = impl_->transaction->GetForUpdate(
      detail::to_native_options(options),
      impl_->store->require_handle(column_family),
      detail::to_slice(key),
      &value);
   if (status.IsNotFound()) {
      return std::nullopt;
   }
   detail::throw_if_error(status, "failed to lock and get RocksDB transaction value");
   auto bytes = std::vector<std::byte>{};
   bytes.resize(value.size());
   std::memcpy(bytes.data(), value.data(), value.size());
   return bytes;
}

std::vector<entry> transaction::scan(family column_family,
                                     std::vector<std::byte> prefix,
                                     read_options options) {
   ensure_active("failed to scan RocksDB transaction prefix");
   auto iterator = std::unique_ptr<::rocksdb::Iterator>{
      impl_->transaction->GetIterator(
         detail::to_native_options(options), impl_->store->require_handle(column_family)),
   };

   auto values = std::vector<entry>{};
   for (iterator->Seek(detail::to_slice(prefix)); iterator->Valid(); iterator->Next()) {
      auto key = detail::bytes_from_slice(iterator->key());
      if (!detail::starts_with(key, prefix)) {
         break;
      }
      values.push_back(entry{.key = std::move(key), .value = detail::bytes_from_slice(iterator->value())});
   }
   detail::throw_if_error(iterator->status(), "failed to scan RocksDB transaction prefix");
   return values;
}

scan_result transaction::scan_page(family column_family, scan_request request) {
   ensure_active("failed to scan RocksDB transaction prefix page");
   auto iterator = std::unique_ptr<::rocksdb::Iterator>{
      impl_->transaction->GetIterator(
         detail::to_native_options(request.options),
         impl_->store->require_handle(column_family)),
   };
   return detail::read_scan_page(
      std::move(iterator), std::move(request), "failed to scan RocksDB transaction prefix page");
}

void transaction::lock(family column_family, std::vector<std::byte> key, read_options options) {
   static_cast<void>(get_for_update(std::move(column_family), std::move(key), options));
}

void transaction::put(family column_family,
                      std::vector<std::byte> key,
                      std::vector<std::byte> value) {
   ensure_active("failed to put RocksDB transaction value");
   detail::throw_if_error(
      impl_->transaction->Put(
         impl_->store->require_handle(column_family),
         detail::to_slice(key),
         detail::to_slice(value)),
      "failed to put RocksDB transaction value");
}

void transaction::erase(family column_family, std::vector<std::byte> key) {
   ensure_active("failed to delete RocksDB transaction value");
   detail::throw_if_error(
      impl_->transaction->Delete(
         impl_->store->require_handle(column_family), detail::to_slice(key)),
      "failed to delete RocksDB transaction value");
}

void transaction::create_savepoint() {
   ensure_active("failed to create RocksDB transaction savepoint");
   impl_->transaction->SetSavePoint();
}

void transaction::rollback_to_savepoint() {
   ensure_active("failed to rollback RocksDB transaction savepoint");
   detail::throw_if_error(
      impl_->transaction->RollbackToSavePoint(), "failed to rollback RocksDB transaction savepoint");
}

void transaction::release_savepoint() {
   ensure_active("failed to release RocksDB transaction savepoint");
   detail::throw_if_error(impl_->transaction->PopSavePoint(), "failed to release RocksDB transaction savepoint");
}

void transaction::commit() {
   ensure_active("failed to commit RocksDB transaction");
   const auto status = impl_->transaction->Commit();
   detail::throw_if_error(status, "failed to commit RocksDB transaction");
   impl_->finished = true;
   impl_->store->record_commit(true);
}

void transaction::rollback() {
   ensure_active("failed to rollback RocksDB transaction");
   const auto status = impl_->transaction->Rollback();
   detail::throw_if_error(status, "failed to rollback RocksDB transaction");
   impl_->finished = true;
   impl_->store->record_commit(false);
}

} // namespace forge::rocksdb
