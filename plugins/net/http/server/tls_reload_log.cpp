#include "details/tls_reload_log.hxx"

#include <boost/system/system_error.hpp>
#include <forge/log/macros.hpp>
#include <system_error>

import forge.exceptions;
import forge.log.logger;

namespace forge::plugins::net::http::server::detail {

void emit_tls_reload_log(const tls_reload_log& value) noexcept {
   try {
      if (value.result == tls_reload_log::outcome::applied) {
         ilog("forge.plugins.net.http.server", "HTTP TLS context replaced",
              ("event", "http.tls.reload.applied"));
         return;
      }
      auto code = -1;
      auto category = "unknown";
      if (value.error) {
         try {
            std::rethrow_exception(value.error);
         } catch (const forge::exceptions::base& error) {
            code = error.code().value();
            category = "forge";
         } catch (const boost::system::system_error& error) {
            code = error.code().value();
            category = "boost";
         } catch (const std::system_error& error) {
            code = error.code().value();
            category = "std";
         } catch (...) {
         }
      }
      if (value.result == tls_reload_log::outcome::rejected) {
         dlog("forge.plugins.net.http.server", "HTTP TLS reload rejected",
              ("event", "http.tls.reload.rejected")("error_code", code)("error_category", category));
      } else {
         elog("forge.plugins.net.http.server", "HTTP TLS replacement failed before publication",
              ("event", "http.tls.reload.failed")("error_code", code)("error_category", category));
      }
   } catch (...) {
      // Observability cannot veto a context swap or replace its original error.
   }
}

} // namespace forge::plugins::net::http::server::detail
