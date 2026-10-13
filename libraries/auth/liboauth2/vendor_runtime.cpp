module;

#include <forge/exceptions/macros.hpp>
extern "C" {
#include <oauth2/log.h>
#include <oauth2/mem.h>
#include <oauth2/util.h>
}

#include <cstddef>
#include <mutex>

module forge.auth.liboauth2.verifier;

#include "details/vendor_runtime.hxx"

namespace forge::auth::liboauth2::detail {
namespace {

std::mutex runtime_mutex;
std::size_t runtime_users = 0;
oauth2_log_t* runtime_log = nullptr;

void discard_vendor_log(oauth2_log_sink_t*, const char*, unsigned long, const char*, oauth2_log_level_t, const char*) {}

} // namespace

vendor_runtime::vendor_runtime() {
   const auto lock = std::scoped_lock{runtime_mutex};
   if (runtime_users == 0) {
      auto* sink = oauth2_log_sink_create(OAUTH2_LOG_ERROR, discard_vendor_log, nullptr);
      if (!sink) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification runtime initialization failed");
      }
      runtime_log = oauth2_init(OAUTH2_LOG_ERROR, sink);
      if (!runtime_log) {
         oauth2_shutdown(nullptr);
         oauth2_mem_free(sink);
         FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification runtime initialization failed");
      }
   }
   ++runtime_users;
}

vendor_runtime::~vendor_runtime() {
   const auto lock = std::scoped_lock{runtime_mutex};
   if (--runtime_users == 0) {
      oauth2_shutdown(runtime_log);
      runtime_log = nullptr;
   }
}

} // namespace forge::auth::liboauth2::detail
