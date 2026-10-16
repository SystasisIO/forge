module;

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

export module forge.auth.http.bearer;

export import forge.auth.http.exceptions;
import forge.net.http.types;

export namespace forge::auth::http {

// Returned views borrow the supplied header storage. No credential verification is performed.
[[nodiscard]] std::optional<std::string_view>
extract_bearer_token(std::span<const std::string_view> authorization_headers, std::size_t max_token_bytes = 4096);

[[nodiscard]] std::optional<std::string_view> extract_bearer_token(const forge::net::http::request& request,
                                                                   std::size_t max_token_bytes = 4096);

} // namespace forge::auth::http
