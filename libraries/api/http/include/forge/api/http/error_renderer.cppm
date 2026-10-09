module;

#include <functional>
#include <optional>

export module forge.api.http.error_renderer;

import forge.api.core.types;
import forge.exceptions;
import forge.net.http.types;

export namespace forge::api::http {

// Borrowed values remain valid only during the synchronous renderer call.
struct error_context {
   const forge::net::http::request& request;
   forge::net::http::status status;
   const forge::api::core::error_payload& payload;
   const forge::exceptions::base* cause = nullptr;
};

// Return nullopt to retain the standard Forge error representation. A cause is
// available only for a local exception; already projected remote errors have none.
using error_renderer =
   std::function<std::optional<forge::net::http::response>(const error_context&)>;

} // namespace forge::api::http
