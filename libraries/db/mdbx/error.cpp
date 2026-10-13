module;

#include <forge/exceptions/macros.hpp>
#include <mdbx.h>

#include <array>
#include <cerrno>
#include <string>
#include <string_view>

module forge.db.mdbx.driver;

import :error;
import forge.db.mdbx.result_diagnostics;
import forge.db.mdbx.exceptions;

namespace forge::db::mdbx::detail {

bool mdbx_success(int code) noexcept {
   return code == MDBX_SUCCESS || code == MDBX_RESULT_TRUE;
}

bool mdbx_not_found(int code) noexcept {
   return code == MDBX_NOTFOUND;
}

void require_mdbx_success(int code, std::string_view operation) {
   if (mdbx_success(code)) {
      return;
   }

   result_diagnostics::process().record_result(code);

   const auto message = std::string{operation} + " failed";
   auto native_buffer = std::array<char, 1024>{};
   const auto native_message = std::string{
      mdbx_strerror_r(code, native_buffer.data(), native_buffer.size())};
   switch (code) {
      case MDBX_MAP_FULL:
         FORGE_THROW_EXCEPTION(exceptions::map_full, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      case MDBX_READERS_FULL:
         FORGE_THROW_EXCEPTION(exceptions::readers_full, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      case MDBX_BUSY:
      case MDBX_TXN_OVERLAPPING:
      case EAGAIN:
         FORGE_THROW_EXCEPTION(exceptions::environment_busy, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      case MDBX_INCOMPATIBLE:
         FORGE_THROW_EXCEPTION(exceptions::incompatible_environment, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      case MDBX_TXN_FULL:
         FORGE_THROW_EXCEPTION(exceptions::transaction_full, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      case MDBX_PAGE_NOTFOUND:
      case MDBX_CORRUPTED:
      case MDBX_VERSION_MISMATCH:
      case MDBX_PANIC:
         FORGE_THROW_EXCEPTION(exceptions::corruption, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      case ENOSPC:
      case MDBX_UNABLE_EXTEND_MAPSIZE:
      case MDBX_EIO:
         FORGE_THROW_EXCEPTION(exceptions::io_error, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
      default:
         FORGE_THROW_EXCEPTION(exceptions::native_error, message,
                               forge::exceptions::ctx("native-code", code),
                               forge::exceptions::ctx("native-message", native_message));
   }
}

} // namespace forge::db::mdbx::detail
