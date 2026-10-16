module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

module forge.plugins.net.http.server.bearer_auth;

import forge.crypto.core.constant_time;
import forge.auth.http.bearer;
import forge.net.http.types;
import forge.plugins.net.http.server.exceptions;

namespace forge::plugins::net::http::server {
namespace {

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
          auto headers = std::array<std::string_view, 2>{};
          auto count = std::size_t{};
          for (const auto& header : request.headers) {
             if (forge::net::http::header_name_equal(header.name, "Authorization")) {
                headers[count++] = header.value;
                if (count == headers.size()) {
                   break;
                }
             }
          }
          auto token = std::optional<std::string_view>{};
          try {
             token = forge::auth::http::extract_bearer_token(std::span<const std::string_view>{headers.data(), count});
          } catch (const forge::auth::http::exceptions::malformed_evidence&) {
             co_return unauthorized();
          } catch (const forge::auth::http::exceptions::duplicate_evidence&) {
             co_return unauthorized();
          }
          if (!token) {
             co_return unauthorized();
          }

          const auto candidate = hash_bearer_token(*token);
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
