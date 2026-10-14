module;

#include <boost/describe.hpp>
#include <cstdint>
#include <optional>
#include <string>

export module forge.api.wasm.http_command;

export import forge.api.wasm.payload;
export import forge.net.http.method;

export namespace forge::api::wasm {

// A bounded intent. Host adapters still own URL/path/TLS/redirect authorization.
struct http_command {
   net::http::method method = net::http::method::get;
   std::string target;
   std::optional<payload> body;

   bool operator==(const http_command&) const = default;
};

struct http_response {
   std::uint16_t status = 0;
   std::optional<payload> body;

   bool operator==(const http_response&) const = default;
};

BOOST_DESCRIBE_STRUCT(http_command, (), (method, target, body))
BOOST_DESCRIBE_STRUCT(http_response, (), (status, body))

} // namespace forge::api::wasm
