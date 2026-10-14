#pragma once

#include <exception>

namespace forge::plugins::net::http::server::detail {

struct tls_reload_log {
   enum class outcome { applied, rejected, failed };
   outcome result;
   std::exception_ptr error;
};

void emit_tls_reload_log(const tls_reload_log& value) noexcept;

} // namespace forge::plugins::net::http::server::detail
