module;

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <mdbx.h>

module forge.db.mdbx.diagnostics;

import forge.db.mdbx.result_diagnostics;

#include "details/native_diagnostics.hxx"

namespace forge::db::mdbx {

void diagnostics::claim_process() {
   static_cast<void>(detail::result_diagnostics::process());
   detail::native_diagnostics::process().install();
}

bool diagnostics::installed() noexcept {
   return detail::native_diagnostics::process().installed();
}

void diagnostics::drain() noexcept {
   detail::native_diagnostics::process().drain();
   detail::result_diagnostics::process().drain();
}

} // namespace forge::db::mdbx
