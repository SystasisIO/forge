module;

#include <forge/log/macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <mutex>
#include <mdbx.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

module forge.db.mdbx.diagnostics;

import forge.log.logger;

#include "details/native_diagnostics.hxx"

namespace forge::db::mdbx::detail {

native_diagnostics& native_diagnostics::process() noexcept {
   static auto instance = native_diagnostics{};
   return instance;
}

void native_diagnostics::capture(MDBX_log_level_t level, const char*, int line,
                                 const char*, va_list) noexcept {
   auto& instance = process();
   const auto index = std::min(static_cast<unsigned>(level),
                               static_cast<unsigned>(instance._messages.size() - 1));
   instance._latest_line[index].store(line, std::memory_order_relaxed);
   instance._messages[index].fetch_add(1, std::memory_order_release);
   // No formatting, allocation, attribution, sinks or reentry into libmdbx here.
   if (level == MDBX_LOG_FATAL) {
      emergency_notice();
   }
}

void native_diagnostics::emergency_notice() noexcept {
   // Native fatal paths can abort before any safe drain boundary is reached.
   static constexpr char notice[] = "forge.db.mdbx native.fatal: diagnostic text redacted\n";
   const auto saved_errno = errno;
#if defined(_WIN32)
   static_cast<void>(::_write(2, notice, static_cast<unsigned>(sizeof(notice) - 1)));
#else
   static_cast<void>(::write(STDERR_FILENO, notice, sizeof(notice) - 1));
#endif
   errno = saved_errno;
}

void native_diagnostics::install() {
   std::call_once(_installation, [this] {
      static_cast<void>(mdbx_setup_debug(MDBX_LOG_DONTCHANGE, MDBX_DBG_DONTCHANGE, capture));
      _installed.store(true, std::memory_order_release);
   });
}

bool native_diagnostics::installed() const noexcept {
   return _installed.load(std::memory_order_acquire);
}

void native_diagnostics::drain() noexcept {
   if (_draining.test_and_set(std::memory_order_acquire)) {
      return;
   }
   auto release = [](std::atomic_flag* flag) { flag->clear(std::memory_order_release); };
   const auto guard = std::unique_ptr<std::atomic_flag, decltype(release)>{&_draining, release};
   for (auto level = std::size_t{0}; level < _messages.size(); ++level) {
      const auto count = _messages[level].exchange(0, std::memory_order_acquire);
      if (count == 0) {
         continue;
      }
      const auto line = _latest_line[level].load(std::memory_order_relaxed);
      if (level <= MDBX_LOG_ERROR) {
         elog("forge.db.mdbx", "native diagnostics observed",
              ("event", "native.diagnostics")("scope", "process")
              ("native_level", level)("count", count)("latest_line", line));
      } else if (level == MDBX_LOG_WARN) {
         wlog("forge.db.mdbx", "native diagnostics observed",
              ("event", "native.diagnostics")("scope", "process")
              ("native_level", level)("count", count)("latest_line", line));
      } else {
         dlog("forge.db.mdbx", "native diagnostics observed",
              ("event", "native.diagnostics")("scope", "process")
              ("native_level", level)("count", count)("latest_line", line));
      }
   }
}

} // namespace forge::db::mdbx::detail
