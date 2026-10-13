#pragma once

namespace forge::rocksdb {

struct transaction::impl {
   impl(std::shared_ptr<store::impl> store_value, std::unique_ptr<::rocksdb::Transaction> transaction_value);
   ~impl();

   std::shared_ptr<store::impl> store;
   std::unique_ptr<::rocksdb::Transaction> transaction;
   bool finished = false;
};

} // namespace forge::rocksdb
