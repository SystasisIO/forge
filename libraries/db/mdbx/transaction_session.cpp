module;

#include <forge/exceptions/macros.hpp>
#include <mdbx.h>

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module forge.db.mdbx.driver;

import forge.db.mdbx.result_diagnostics;

import :error;
import forge.asio.affine;
import forge.asio.gate;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.core.record;

#include "details/driver_impl.hxx"
#include "details/environment.hxx"
#include "details/scan.hxx"
#include "details/transaction_session.hxx"

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

transaction_session::transaction_session(std::shared_ptr<driver_impl> owner,
                                         forge::asio::gate::ticket ticket,
                                         MDBX_txn* transaction)
    : owner_{std::move(owner)}, ticket_{std::move(ticket)},
      transactions_{transaction} {}

transaction_session::~transaction_session() {
   if (!transactions_.empty()) {
      owner_->abort_sync(std::move(transactions_));
   }
   finish();
}

forge::db::core::capabilities transaction_session::capabilities() const noexcept {
   return forge::db::core::capabilities{
      .snapshot_reads = false,
      .writes = true,
      .savepoints = true,
      .record_locks = true,
   };
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
transaction_session::get(forge::db::core::family column_family,
                         forge::db::core::record_key key) {
   require_active();
   owner_->environment_handle()->validate_key(key);
   const auto dbi = owner_->environment_handle()->resolve(column_family);
   co_return co_await owner_->executor().execute(
      {.name = "mdbx-get"},
      [transaction = active(), dbi, key = std::move(key)] {
         auto native_key = native_value(key.bytes());
         auto value = MDBX_val{};
         const auto code = mdbx_get(transaction, dbi, &native_key, &value);
         if (mdbx_not_found(code)) {
            return std::optional<std::vector<std::byte>>{};
         }
         require_mdbx_success(code, "mdbx_get");
         return std::optional<std::vector<std::byte>>{copy_value(value)};
      });
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
transaction_session::get_for_update(forge::db::core::family column_family,
                                    forge::db::core::record_key key) {
   co_return co_await get(std::move(column_family), std::move(key));
}

boost::asio::awaitable<void>
transaction_session::put(forge::db::core::family column_family,
                         forge::db::core::record_key key,
                         std::vector<std::byte> value) {
   require_active();
   owner_->environment_handle()->validate_key(key);
   owner_->environment_handle()->validate_value(value);
   const auto dbi = owner_->environment_handle()->resolve(column_family);
   co_await owner_->executor().execute(
      {.name = "mdbx-put"},
      [transaction = active(), dbi, key = std::move(key), value = std::move(value)] {
         auto native_key = native_value(key.bytes());
         auto native_data = native_value(value);
         require_mdbx_success(
            mdbx_put(transaction, dbi, &native_key, &native_data, MDBX_UPSERT),
            "mdbx_put");
      });
}

boost::asio::awaitable<void>
transaction_session::erase(forge::db::core::family column_family,
                           forge::db::core::record_key key) {
   require_active();
   owner_->environment_handle()->validate_key(key);
   const auto dbi = owner_->environment_handle()->resolve(column_family);
   co_await owner_->executor().execute(
      {.name = "mdbx-erase"},
      [transaction = active(), dbi, key = std::move(key)] {
         auto native_key = native_value(key.bytes());
         const auto code = mdbx_del(transaction, dbi, &native_key, nullptr);
         if (!mdbx_not_found(code)) {
            require_mdbx_success(code, "mdbx_del");
         }
      });
}

boost::asio::awaitable<forge::db::core::record_page>
transaction_session::scan_page(forge::db::core::family column_family,
                               forge::db::core::record_range range,
                               forge::db::core::page_request request) {
   require_active();
   owner_->environment_handle()->validate_range(range, request);
   const auto dbi = owner_->environment_handle()->resolve(column_family);
   co_return co_await owner_->executor().execute(
      {.name = "mdbx-scan"},
      [transaction = active(), dbi, range = std::move(range), request = std::move(request)] {
         return scan_records(transaction, dbi, range, request);
      });
}

boost::asio::awaitable<void> transaction_session::create_savepoint() {
   require_active();
   auto* child = co_await owner_->executor().execute(
      {.name = "mdbx-savepoint-create"},
      [parent = active()] {
         auto* child = static_cast<MDBX_txn*>(nullptr);
         require_mdbx_success(
            mdbx_txn_begin(mdbx_txn_env(parent), parent,
                           MDBX_TXN_READWRITE, &child),
            "mdbx_txn_begin savepoint");
         return child;
      });
   transactions_.push_back(child);
}

boost::asio::awaitable<void> transaction_session::rollback_to_savepoint() {
   require_active();
   if (transactions_.size() < 2) {
      FORGE_THROW_EXCEPTION(forge::db::core::exceptions::invalid_savepoint,
                            "MDBX savepoint stack is empty");
   }
   const auto code = co_await owner_->executor().execute(
      {.name = "mdbx-savepoint-rollback"},
      [child = transactions_.back()] { return mdbx_txn_abort(child); });
   if (code != MDBX_THREAD_MISMATCH) {
      transactions_.pop_back();
   }
   require_mdbx_success(code, "mdbx_txn_abort savepoint");
}

boost::asio::awaitable<void> transaction_session::release_savepoint() {
   require_active();
   if (transactions_.size() < 2) {
      FORGE_THROW_EXCEPTION(forge::db::core::exceptions::invalid_savepoint,
                            "MDBX savepoint stack is empty");
   }
   const auto code = co_await owner_->executor().execute(
      {.name = "mdbx-savepoint-release"},
      [child = transactions_.back()] { return mdbx_txn_commit(child); });
   if (code != MDBX_THREAD_MISMATCH) {
      transactions_.pop_back();
   }
   require_mdbx_success(code, "mdbx_txn_commit savepoint");
}

boost::asio::awaitable<void> transaction_session::commit() {
   require_active();
   try {
      co_await owner_->executor().execute(
         {.name = "mdbx-commit"},
         [this] {
            while (transactions_.size() > 1) {
               auto* child = transactions_.back();
               const auto code = mdbx_txn_commit(child);
               if (code != MDBX_THREAD_MISMATCH) {
                  transactions_.pop_back();
               }
               require_mdbx_success(code, "mdbx_txn_commit savepoint");
            }

            auto* root = transactions_.back();
            const auto code = mdbx_txn_commit(root);
            if (code != MDBX_THREAD_MISMATCH) {
               transactions_.clear();
            }
            require_mdbx_success(code, "mdbx_txn_commit");
         });
      finish();
      result_diagnostics::process().record_commit(true);
   } catch (...) {
      if (transactions_.empty()) {
         finish();
      }
      throw;
   }
}

boost::asio::awaitable<void> transaction_session::rollback() {
   if (transactions_.empty()) {
      co_return;
   }
   try {
      co_await owner_->executor().execute(
         {.name = "mdbx-rollback"},
         [this] {
            auto first_error = int{MDBX_SUCCESS};
            while (!transactions_.empty()) {
               auto* transaction = transactions_.back();
               const auto code = mdbx_txn_abort(transaction);
               if (code != MDBX_THREAD_MISMATCH) {
                  transactions_.pop_back();
               } else if (first_error == MDBX_SUCCESS) {
                  first_error = code;
                  break;
               } else {
                  break;
               }
               if (!mdbx_success(code) && first_error == MDBX_SUCCESS) {
                  first_error = code;
               }
            }
            require_mdbx_success(first_error, "mdbx_txn_abort");
         });
      finish();
      result_diagnostics::process().record_commit(false);
   } catch (...) {
      if (transactions_.empty()) {
         finish();
      }
      throw;
   }
}

MDBX_txn* transaction_session::active() const {
   require_active();
   return transactions_.back();
}

void transaction_session::require_active() const {
   if (transactions_.empty()) {
      FORGE_THROW_EXCEPTION(forge::db::core::exceptions::transaction_closed,
                            "MDBX transaction is closed");
   }
}

void transaction_session::finish() noexcept {
   ticket_.release();
}

} // namespace forge::db::mdbx::detail
