module;

#include <forge/log/macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <rocksdb/env.h>
#include <rocksdb/listener.h>

module forge.rocksdb.store;

import forge.log.logger;

#include "details/native_diagnostics.hxx"

namespace forge::rocksdb::detail {

native_diagnostics::native_diagnostics() : ::rocksdb::Logger{::rocksdb::INFO_LEVEL} {}

native_diagnostics& native_diagnostics::process() noexcept {
   static auto instance = native_diagnostics{};
   return instance;
}

void native_diagnostics::record_result(const ::rocksdb::Status& status) noexcept {
   if (status.ok()) { return; }
   const auto code = std::min(static_cast<unsigned>(status.code()),
                              static_cast<unsigned>(_results.size() - 1));
   _result_details[code].store(static_cast<unsigned>(status.subcode()), std::memory_order_relaxed);
   _results[code].fetch_add(1, std::memory_order_release);
}

void native_diagnostics::record_commit(bool committed) noexcept {
   (committed ? _commits : _rollbacks).fetch_add(1, std::memory_order_relaxed);
}

void native_diagnostics::acquire_transaction() noexcept {
   _transactions.fetch_add(1, std::memory_order_relaxed);
}

void native_diagnostics::release_transaction() noexcept {
   _transactions.fetch_sub(1, std::memory_order_release);
}

bool native_diagnostics::idle() const noexcept {
   return _transactions.load(std::memory_order_acquire) == 0;
}

void native_diagnostics::Logv(const char*, va_list) noexcept {
   _messages[::rocksdb::INFO_LEVEL].fetch_add(1, std::memory_order_relaxed);
}

void native_diagnostics::Logv(::rocksdb::InfoLogLevel level, const char*, va_list) noexcept {
   const auto index = std::min(static_cast<unsigned>(level),
                               static_cast<unsigned>(_messages.size() - 1));
   _messages[index].fetch_add(1, std::memory_order_relaxed);
}

void native_diagnostics::OnBackgroundError(::rocksdb::BackgroundErrorReason reason,
                                          ::rocksdb::Status* status) noexcept {
   if (status == nullptr || status->ok()) {
      return;
   }
   const auto code = std::min(static_cast<unsigned>(status->code()),
                              static_cast<unsigned>(_errors.size() - 1));
   const auto severity = static_cast<unsigned>(status->severity());
   _latest_details[code].store((static_cast<unsigned>(reason) << 8U) |
                                 static_cast<unsigned>(status->subcode()),
                              std::memory_order_relaxed);
   auto previous = _max_severity[code].load(std::memory_order_relaxed);
   while (previous < severity &&
          !_max_severity[code].compare_exchange_weak(previous, severity, std::memory_order_relaxed)) {
   }
   _errors[code].fetch_add(1, std::memory_order_release);
   // Never change *status: corruption and background failures remain engine failures.
}

void native_diagnostics::drain() noexcept {
   if (_draining.test_and_set(std::memory_order_acquire)) {
      return;
   }
   auto release = [](std::atomic_flag* flag) { flag->clear(std::memory_order_release); };
   const auto guard = std::unique_ptr<std::atomic_flag, decltype(release)>{&_draining, release};
   const auto commits = _commits.exchange(0, std::memory_order_relaxed);
   const auto rollbacks = _rollbacks.exchange(0, std::memory_order_relaxed);
   if (commits != 0 || rollbacks != 0) {
      dlog("forge.db.rocksdb", "transaction outcomes observed",
           ("event", "transaction.outcomes")("committed", commits)("rolled_back", rollbacks));
   }
   for (auto code = std::size_t{0}; code < _results.size(); ++code) {
      const auto count = _results[code].exchange(0, std::memory_order_acquire);
      if (count == 0) { continue; }
      const auto subcode = _result_details[code].load(std::memory_order_relaxed);
      if (code == ::rocksdb::Status::kNotFound || code == ::rocksdb::Status::kBusy ||
          code == ::rocksdb::Status::kTimedOut || code == ::rocksdb::Status::kAborted ||
          code == ::rocksdb::Status::kShutdownInProgress || code == ::rocksdb::Status::kTryAgain) {
         dlog("forge.db.rocksdb", "native operations incomplete",
              ("event", "operation.incomplete")("scope", "process")
              ("code", code)("count", count)("latest_subcode", subcode));
      } else {
         elog("forge.db.rocksdb", "native operations failed",
              ("event", "operation.failed")("scope", "process")
              ("code", code)("count", count)("latest_subcode", subcode));
      }
   }
   for (auto level = std::size_t{0}; level < _messages.size(); ++level) {
      const auto count = _messages[level].exchange(0, std::memory_order_relaxed);
      if (count == 0) {
         continue;
      }
      if (level == ::rocksdb::ERROR_LEVEL || level == ::rocksdb::FATAL_LEVEL) {
         elog("forge.db.rocksdb", "native diagnostics observed",
              ("event", "native.diagnostics")("native_level", level)("count", count));
      } else if (level == ::rocksdb::WARN_LEVEL) {
         wlog("forge.db.rocksdb", "native diagnostics observed",
              ("event", "native.diagnostics")("native_level", level)("count", count));
      } else {
         dlog("forge.db.rocksdb", "native diagnostics observed",
              ("event", "native.diagnostics")("native_level", level)("count", count));
      }
   }
   for (auto code = std::size_t{0}; code < _errors.size(); ++code) {
      const auto count = _errors[code].exchange(0, std::memory_order_acquire);
      if (count == 0) {
         continue;
      }
      const auto details = _latest_details[code].load(std::memory_order_relaxed);
      const auto severity = _max_severity[code].load(std::memory_order_relaxed);
      if (severity < ::rocksdb::Status::kHardError &&
          (code == ::rocksdb::Status::kNotFound || code == ::rocksdb::Status::kBusy ||
           code == ::rocksdb::Status::kTimedOut || code == ::rocksdb::Status::kAborted ||
           code == ::rocksdb::Status::kShutdownInProgress || code == ::rocksdb::Status::kTryAgain)) {
         dlog("forge.db.rocksdb", "background operation incomplete",
              ("event", "background_operation.incomplete")("code", code)("count", count)
              ("latest_subcode", details & 0xffU)("latest_reason", details >> 8U)
              ("max_severity", severity));
         continue;
      }
      elog("forge.db.rocksdb", "background errors observed",
           ("event", "background_error.observed")("code", code)("count", count)
           ("latest_subcode", details & 0xffU)("latest_reason", details >> 8U)
           ("max_severity", severity));
   }
}

} // namespace forge::rocksdb::detail
