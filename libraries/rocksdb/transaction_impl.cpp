module;

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <rocksdb/utilities/transaction.h>

module forge.rocksdb.store;

#include "details/store_impl.hxx"
#include "details/transaction_impl.hxx"

namespace forge::rocksdb {

transaction::impl::impl(std::shared_ptr<store::impl> store_value,
                        std::unique_ptr<::rocksdb::Transaction> transaction_value)
   : store{std::move(store_value)}, transaction{std::move(transaction_value)} {
   store->acquire_transaction();
}

transaction::impl::~impl() {
   transaction.reset();
   store->release_transaction();
}

} // namespace forge::rocksdb
