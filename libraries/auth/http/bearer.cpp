module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

module forge.auth.http.bearer;

import forge.net.http.types;

namespace forge::auth::http {

std::optional<std::string_view> extract_bearer_token(std::span<const std::string_view> authorization_headers,
                                                     std::size_t max_token_bytes) {
   if (max_token_bytes == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Bearer token size limit must be positive");
   }
   if (authorization_headers.empty()) {
      return std::nullopt;
   }
   if (authorization_headers.size() != 1) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_evidence, "Authorization header is duplicated");
   }

   constexpr auto scheme = std::string_view{"bearer"};
   const auto authorization = authorization_headers.front();
   if (authorization.size() <= scheme.size() || authorization[scheme.size()] != ' ' ||
       !std::ranges::equal(authorization.substr(0, scheme.size()), scheme, [](char left, char right) {
          const auto lower = left >= 'A' && left <= 'Z' ? static_cast<char>(left + ('a' - 'A')) : left;
          return lower == right;
       })) {
      FORGE_THROW_EXCEPTION(exceptions::malformed_evidence, "Bearer authorization is malformed");
   }
   auto offset = scheme.size();
   while (offset < authorization.size() && authorization[offset] == ' ') {
      ++offset;
   }
   const auto token = authorization.substr(offset);
   if (token.empty() || token.size() > max_token_bytes || std::ranges::any_of(token, [](char value) {
          const auto byte = static_cast<unsigned char>(value);
          return byte <= 0x20U || byte == 0x7fU;
       })) {
      FORGE_THROW_EXCEPTION(exceptions::malformed_evidence, "Bearer token is malformed");
   }
   return token;
}

std::optional<std::string_view> extract_bearer_token(const forge::net::http::request& request,
                                                     std::size_t max_token_bytes) {
   const auto count = std::min(request.header_count("Authorization"), std::size_t{2});
   const auto headers = std::array{request.header("Authorization").value_or(std::string_view{}), std::string_view{}};
   return extract_bearer_token(std::span<const std::string_view>{headers.data(), count}, max_token_bytes);
}

} // namespace forge::auth::http
