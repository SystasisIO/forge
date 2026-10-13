#pragma once

namespace forge::rocksdb {

namespace detail { class native_diagnostics; }

struct store::impl {
   explicit impl(config value);
   ~impl();

   [[nodiscard]] ::rocksdb::ColumnFamilyHandle* require_handle(const family& column_family) const;
   void drain_diagnostics() const noexcept;
   void acquire_transaction() const noexcept;
   void release_transaction() const noexcept;
   void record_commit(bool committed) const noexcept;

   config settings;
   std::shared_ptr<detail::native_diagnostics> diagnostics;
   std::unique_ptr<::rocksdb::TransactionDB> db;
   std::unordered_map<std::string, ::rocksdb::ColumnFamilyHandle*> handles;
};

} // namespace forge::rocksdb
