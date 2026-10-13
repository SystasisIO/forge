module;

#include <forge/log/macros.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mdbx.h>

module forge.db.mdbx.result_diagnostics;

import forge.log.logger;

namespace forge::db::mdbx::detail {

result_diagnostics& result_diagnostics::process() noexcept {
   static auto instance = result_diagnostics{};
   return instance;
}

void result_diagnostics::record_result(int code) noexcept {
   if (code == MDBX_SUCCESS || code == MDBX_RESULT_TRUE) { return; }
   for (auto index = std::size_t{0}; index < _codes.size(); ++index) {
      auto observed = _codes[index].load(std::memory_order_relaxed);
      if (observed == 0) {
         static_cast<void>(_codes[index].compare_exchange_strong(observed, code, std::memory_order_relaxed));
         if (observed == 0) { observed = code; }
      }
      if (observed == code) {
         _counts[index].fetch_add(1, std::memory_order_release);
         return;
      }
   }
   _overflow_code.store(code, std::memory_order_relaxed);
   _overflow.fetch_add(1, std::memory_order_release);
}

void result_diagnostics::record_commit(bool committed) noexcept {
   (committed ? _commits : _rollbacks).fetch_add(1, std::memory_order_relaxed);
}

void result_diagnostics::drain() noexcept {
   if (_draining.test_and_set(std::memory_order_acquire)) { return; }
   auto release = [](std::atomic_flag* flag) { flag->clear(std::memory_order_release); };
   const auto guard = std::unique_ptr<std::atomic_flag, decltype(release)>{&_draining, release};
   const auto commits = _commits.exchange(0, std::memory_order_relaxed);
   const auto rollbacks = _rollbacks.exchange(0, std::memory_order_relaxed);
   if (commits != 0 || rollbacks != 0) {
      dlog("forge.db.mdbx", "transaction outcomes observed",
           ("event", "transaction.outcomes")("scope", "process")
           ("committed", commits)("rolled_back", rollbacks));
   }
   for (auto index = std::size_t{0}; index < _counts.size(); ++index) {
      const auto count = _counts[index].exchange(0, std::memory_order_acquire);
      if (count == 0) { continue; }
      const auto code = _codes[index].load(std::memory_order_relaxed);
      if (code == MDBX_NOTFOUND || code == MDBX_KEYEXIST || code == MDBX_BUSY ||
          code == MDBX_TXN_OVERLAPPING || code == EAGAIN) {
         dlog("forge.db.mdbx", "native operations incomplete",
              ("event", "operation.incomplete")("scope", "process")("code", code)("count", count));
      } else {
         elog("forge.db.mdbx", "native operations failed",
              ("event", "operation.failed")("scope", "process")("code", code)("count", count));
      }
   }
   const auto overflow = _overflow.exchange(0, std::memory_order_acquire);
   if (overflow != 0) {
      elog("forge.db.mdbx", "additional native errors observed",
           ("event", "operation.error_overflow")("scope", "process")
           ("count", overflow)("latest_code", _overflow_code.load(std::memory_order_relaxed)));
   }
}

} // namespace forge::db::mdbx::detail
