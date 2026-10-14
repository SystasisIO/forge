module forge.net.http.method;

namespace forge::net::http {

bool is_idempotent(method value) noexcept {
   return value == method::get || value == method::head || value == method::put || value == method::delete_ ||
          value == method::options;
}

} // namespace forge::net::http
