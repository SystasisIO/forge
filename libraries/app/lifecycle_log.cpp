#include "details/lifecycle_log.hxx"

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <forge/log/macros.hpp>

#include <string>
#include <system_error>

import forge.app.exceptions;
import forge.log.logger;

namespace forge::app::detail {

void emit_lifecycle_log(const lifecycle_log& value) noexcept {
   try {
      auto level = value.level;
      auto event = value.event;
      auto code = level == lifecycle_log_level::error ? -1 : 0;
      auto category = level == lifecycle_log_level::error ? "unknown" : "none";
      auto canceled = false;
      if (value.error) {
         code = -1;
         category = "unknown";
         try {
            std::rethrow_exception(value.error);
         } catch (const forge::app::exceptions::operation_stopped& error) {
            code = error.code().value();
            category = "forge";
            canceled = true;
         } catch (const forge::exceptions::base& error) {
            code = error.code().value();
            category = "forge";
         } catch (const boost::system::system_error& error) {
            code = error.code().value();
            category = "boost";
            canceled = error.code() == boost::asio::error::operation_aborted;
         } catch (const std::system_error& error) {
            code = error.code().value();
            category = "std";
         } catch (...) {
         }
         // Cleanup failures are never downgraded to an ordinary stop, even
         // when their typed code is invalid_state or operation_aborted.
         canceled = canceled && std::string_view{value.operation} != "shutdown";
         level = canceled ? lifecycle_log_level::warning : lifecycle_log_level::error;
         if (canceled) {
            event = value.canceled_event;
         }
      }

      if (value.error || level == lifecycle_log_level::error) {
         if (level == lifecycle_log_level::warning) {
            wlog(value.logger, "${event}: ${operation}",
                 ("event", event)("operation", value.operation)("phase", value.phase)
                 ("generation", value.generation)("epoch", value.epoch)("id", value.id)
                 ("error_code", code)("error_category", category));
         } else {
            elog(value.logger, "${event}: ${operation}",
                 ("event", event)("operation", value.operation)("phase", value.phase)
                 ("generation", value.generation)("epoch", value.epoch)("id", value.id)
                 ("error_code", code)("error_category", category));
         }
         return;
      }
      if (value.has_exit_code) {
         ilog(value.logger, "${event}: ${operation}",
              ("event", event)("operation", value.operation)("phase", value.phase)
              ("generation", value.generation)("epoch", value.epoch)("exit_code", value.exit_code));
         return;
      }
      switch (level) {
      case lifecycle_log_level::debug:
         dlog(value.logger, "${event}: ${operation}",
              ("event", event)("operation", value.operation)("phase", value.phase)
              ("generation", value.generation)("epoch", value.epoch)("id", value.id));
         break;
      case lifecycle_log_level::info:
         ilog(value.logger, "${event}: ${operation}",
              ("event", event)("operation", value.operation)("phase", value.phase)
              ("generation", value.generation)("epoch", value.epoch)("id", value.id));
         break;
      case lifecycle_log_level::warning:
         wlog(value.logger, "${event}: ${operation}",
              ("event", event)("operation", value.operation)("phase", value.phase)
              ("generation", value.generation)("epoch", value.epoch)("id", value.id));
         break;
      case lifecycle_log_level::error:
         break;
      }
   } catch (...) {
      // No exception text/context reaches logs, and no observer or sink may
      // veto a published operation result or resource cleanup.
   }
}

} // namespace forge::app::detail
