module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

module forge.plugins.net.http.server.bearer_auth;

import forge.crypto.core.constant_time;
import forge.net.http.types;
import forge.plugins.net.http.server.exceptions;

namespace forge::plugins::net::http::server {
namespace {

constexpr auto bearer_scheme = std::string_view{"Bearer"};

bool ascii_iequal(std::string_view left, std::string_view right) noexcept {
   return left.size() == right.size() && std::ranges::equal(left, right, [](char lhs, char rhs) {
             return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
          });
}

std::string_view bearer_token(const middleware_request& request) noexcept {
   const auto authorization = request.header("Authorization");
   if (!authorization || authorization->size() <= bearer_scheme.size() ||
       !ascii_iequal(authorization->substr(0, bearer_scheme.size()), bearer_scheme)) {
      return {};
   }

   auto offset = bearer_scheme.size();
   if ((*authorization)[offset] != ' ') {
      return {};
   }
   while (offset < authorization->size() && (*authorization)[offset] == ' ') {
      ++offset;
   }
   const auto token = authorization->substr(offset);
   if (token.empty() || std::ranges::any_of(token, [](char value) {
          const auto byte = static_cast<unsigned char>(value);
          return byte <= 0x20U || byte == 0x7fU;
       })) {
      return {};
   }
   return token;
}

middleware_response unauthorized() {
   auto response = middleware_response::text(forge::net::http::status::unauthorized, "unauthorized");
   response.set_header("WWW-Authenticate", "Bearer");
   return response;
}

} // namespace

forge::crypto::digest::sha256 hash_bearer_token(std::string_view token) {
   return forge::crypto::digest::sha256::hash(std::string{token});
}

middleware_descriptor bearer_auth(bearer_auth_options options) {
   if (options.id.empty() || options.path_prefix.empty() || options.token_hashes.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "Bearer authentication requires an id, path prefix and token hashes");
   }
   if (std::ranges::any_of(options.token_hashes, [](const auto& hash) { return hash.empty(); })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Bearer authentication token hash must not be empty");
   }

   auto hashes = std::move(options.token_hashes);
   return middleware_descriptor{
       .id = std::move(options.id),
       .phase = middleware_phase::security,
       .order = options.order,
       .path_prefix = std::move(options.path_prefix),
       .handler = [hashes = std::move(hashes)](const middleware_request& request,
                                               middleware_next next) -> boost::asio::awaitable<middleware_response> {
          const auto token = bearer_token(request);
          if (token.empty()) {
             co_return unauthorized();
          }

          const auto candidate = hash_bearer_token(token);
          auto accepted = false;
          for (const auto& expected : hashes) {
             accepted = forge::crypto::core::constant_time_equal(candidate.to_uint8_span(), expected.to_uint8_span()) ||
                        accepted;
          }
          if (!accepted) {
             co_return unauthorized();
          }
          co_return co_await next();
       },
   };
}

} // namespace forge::plugins::net::http::server
