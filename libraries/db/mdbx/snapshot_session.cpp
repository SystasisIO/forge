module;

#include <forge/exceptions/macros.hpp>
#include <mdbx.h>

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module forge.db.mdbx.driver;

import :error;
import forge.asio.affine;
import forge.asio.gate;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.core.record;

#include "details/environment.hxx"
#include "details/driver_impl.hxx"
#include "details/scan.hxx"
#include "details/snapshot_session.hxx"

namespace forge::db::mdbx::detail {
namespace {

MDBX_val native_value(const std::vector<std::byte>& bytes) noexcept {
   return MDBX_val{.iov_base = const_cast<std::byte*>(bytes.data()),
                   .iov_len = bytes.size()};
}

std::vector<std::byte> copy_value(const MDBX_val& value) {
   const auto* begin = static_cast<const std::byte*>(value.iov_base);
   return std::vector<std::byte>{begin, begin + value.iov_len};
}

} // namespace

snapshot_session::snapshot_session(std::shared_ptr<driver_impl> owner,
                                   MDBX_txn* anchor)
    : owner_{std::move(owner)}, anchor_{anchor} {}

snapshot_session::~snapshot_session() {
   auto clones = std::vector<MDBX_txn*>{};
   {
      auto guard = std::scoped_lock{clones_mutex_};
      clones.swap(clones_);
   }
   for (auto* clone : clones) {
      static_cast<void>(mdbx_txn_abort(clone));
   }
   if (anchor_ != nullptr) {
      static_cast<void>(mdbx_txn_abort(anchor_));
      anchor_ = nullptr;
   }
}

forge::db::core::capabilities snapshot_session::capabilities() const noexcept {
   return forge::db::core::capabilities{
      .snapshot_reads = true,
      .writes = false,
      .savepoints = false,
      .record_locks = false,
   };
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
snapshot_session::get(forge::db::core::family column_family,
                      forge::db::core::record_key key) {
   owner_->environment_handle()->validate_key(key);
   const auto dbi = owner_->environment_handle()->resolve(column_family);
   auto transaction = acquire_clone();
   auto native_key = native_value(key.bytes());
   auto value = MDBX_val{};
   const auto code = mdbx_get(transaction.native(), dbi, &native_key, &value);
   if (mdbx_not_found(code)) {
      co_return std::nullopt;
   }
   require_mdbx_success(code, "mdbx_get snapshot");
   co_return copy_value(value);
}

boost::asio::awaitable<void>
snapshot_session::put(forge::db::core::family,
                      forge::db::core::record_key,
                      std::vector<std::byte>) {
   FORGE_THROW_EXCEPTION(forge::db::core::exceptions::unsupported_operation,
                         "MDBX snapshot is read-only");
}

boost::asio::awaitable<void>
snapshot_session::erase(forge::db::core::family,
                        forge::db::core::record_key) {
   FORGE_THROW_EXCEPTION(forge::db::core::exceptions::unsupported_operation,
                         "MDBX snapshot is read-only");
}

boost::asio::awaitable<forge::db::core::record_page>
snapshot_session::scan_page(forge::db::core::family column_family,
                            forge::db::core::record_range range,
                            forge::db::core::page_request request) {
   owner_->environment_handle()->validate_range(range, request);
   const auto dbi = owner_->environment_handle()->resolve(column_family);
   auto transaction = acquire_clone();
   co_return scan_records(transaction.native(), dbi, range, request);
}

boost::asio::awaitable<void> snapshot_session::commit() {
   FORGE_THROW_EXCEPTION(forge::db::core::exceptions::unsupported_operation,
                         "MDBX snapshot cannot commit");
}

boost::asio::awaitable<void> snapshot_session::rollback() {
   co_return;
}

snapshot_session::clone::clone(snapshot_session& owner, MDBX_txn* transaction)
    : owner_{owner}, transaction_{transaction} {}

snapshot_session::clone::~clone() {
   owner_.recycle(transaction_);
}

MDBX_txn* snapshot_session::clone::native() const noexcept {
   return transaction_;
}

snapshot_session::clone snapshot_session::acquire_clone() {
   auto* transaction = static_cast<MDBX_txn*>(nullptr);
   auto guard = std::unique_lock{clones_mutex_};
   if (!clones_.empty()) {
      transaction = clones_.back();
      clones_.pop_back();
   }

   const auto code = mdbx_txn_clone(anchor_, &transaction, nullptr);
   if (!mdbx_success(code)) {
      if (transaction != nullptr) {
         static_cast<void>(mdbx_txn_abort(transaction));
      }
      guard.unlock();
      require_mdbx_success(code, "mdbx_txn_clone");
   }
   return clone{*this, transaction};
}

void snapshot_session::recycle(MDBX_txn* transaction) noexcept {
   if (transaction == nullptr) {
      return;
   }
   if (!mdbx_success(mdbx_txn_reset(transaction))) {
      static_cast<void>(mdbx_txn_abort(transaction));
      return;
   }
   auto guard = std::scoped_lock{clones_mutex_};
   clones_.push_back(transaction);
}

} // namespace forge::db::mdbx::detail
