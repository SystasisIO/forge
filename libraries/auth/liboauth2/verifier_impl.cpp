module;

#include <forge/exceptions/macros.hpp>
extern "C" {
#include <oauth2/cfg.h>
#include <oauth2/mem.h>
#include <oauth2/oauth2.h>
#include <oauth2/util.h>
}
#include <cjose/cjose.h>
#include <jansson.h>
#include <openssl/crypto.h>

#include <boost/asio/awaitable.hpp>
#include <boost/url.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

module forge.auth.liboauth2.verifier;

import forge.asio.notification;

#include "details/verifier_impl.hxx"

namespace forge::auth::liboauth2 {
namespace {

bool valid_text(std::string_view value) {
   return !value.empty() && value.size() <= 4096 &&
          std::ranges::none_of(value, [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}

void validate_url(const std::string& value) {
   const auto parsed = boost::urls::parse_uri(value);
   if (!valid_text(value) || !parsed || parsed->scheme() != "https" || parsed->host().empty() ||
       parsed->has_userinfo() || parsed->has_query() || parsed->has_fragment() ||
       (parsed->has_port() && parsed->port_number() == 0)) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "verification requires a trusted HTTPS URL");
   }
}

const char* algorithm_name(oauth2::signature_algorithm algorithm) {
   switch (algorithm) {
   case oauth2::signature_algorithm::rs256:
      return "RS256";
   case oauth2::signature_algorithm::rs384:
      return "RS384";
   case oauth2::signature_algorithm::rs512:
      return "RS512";
   case oauth2::signature_algorithm::ps256:
      return "PS256";
   case oauth2::signature_algorithm::ps384:
      return "PS384";
   case oauth2::signature_algorithm::ps512:
      return "PS512";
   case oauth2::signature_algorithm::es256:
      return "ES256";
   case oauth2::signature_algorithm::es384:
      return "ES384";
   case oauth2::signature_algorithm::es512:
      return "ES512";
   }
   return "";
}

} // namespace

void verifier::impl::validate(const options& value) {
   oauth2::validate_policy(value.policy);
   validate_url(value.policy.issuer);
   if (!valid_text(value.policy.audience) || !valid_text(value.policy.version) ||
       value.policy.required_scopes.size() > value.max_scopes || value.policy.algorithms.size() > 9 ||
       value.max_in_flight == 0 || value.max_in_flight > 4096 || value.max_token_bytes == 0 ||
       value.max_token_bytes > 1024 * 1024 || value.max_response_bytes == 0 ||
       value.max_response_bytes > 16 * 1024 * 1024 || value.max_claim_bytes == 0 ||
       value.max_claim_bytes > value.max_response_bytes || value.max_audiences == 0 || value.max_audiences > 1024 ||
       value.max_scopes == 0 || value.max_scopes > 1024 || value.http_timeout <= std::chrono::seconds::zero() ||
       value.http_timeout > std::chrono::seconds{60} || value.timeout > std::chrono::minutes{5}) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "verification limits are invalid");
   }
   auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(value.http_timeout);
   if (const auto* source = std::get_if<jwks>(&value.source)) {
      validate_url(source->endpoint);
      if (value.policy.profile != oauth2::token_profile::jwt_access_token || source->max_keys == 0 ||
          source->max_keys > 1024 || source->cache_lifetime <= std::chrono::seconds::zero() ||
          source->cache_lifetime > std::chrono::hours{24} || source->refresh_interval <= std::chrono::seconds::zero() ||
          source->refresh_interval > source->cache_lifetime ||
          source->refresh_wait_timeout <= std::chrono::milliseconds::zero() ||
          source->refresh_wait_timeout > std::chrono::seconds{30}) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "JWKS source limits are invalid");
      }
      budget += 2 * source->refresh_wait_timeout;
   } else {
      const auto& opaque = std::get<introspection>(value.source);
      validate_url(opaque.endpoint);
      if (value.policy.profile != oauth2::token_profile::opaque_introspection || !valid_text(opaque.client_id) ||
          opaque.client_secret.empty() || opaque.client_secret.size() > 4096 ||
          opaque.client_secret.view().find('\0') != std::string_view::npos) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "introspection source is invalid");
      }
   }
   if (value.timeout <= budget) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "verification deadline cannot cover vendor bounds");
   }
   if (!value.ca_bundle.empty()) {
      const auto path = value.ca_bundle.string();
      auto error = std::error_code{};
      if (path.size() > 4096 || path.find('\0') != std::string::npos || !value.ca_bundle.is_absolute() ||
          !std::filesystem::is_regular_file(value.ca_bundle, error) || error) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "verification CA bundle is invalid");
      }
   }
}

void verifier::impl::config_deleter::operator()(oauth2_cfg_token_verify_t* value) const noexcept {
   oauth2_cfg_token_verify_free(nullptr, value);
}

void verifier::impl::text_deleter::operator()(char* value) const noexcept {
   if (value) {
      OPENSSL_cleanse(value, std::strlen(value));
      oauth2_mem_free(value);
   }
}

verifier::impl::secret_form::secret_form() {
   value.reserve(128 * 1024);
}

verifier::impl::secret_form::~secret_form() {
   OPENSSL_cleanse(value.data(), value.size());
}

void verifier::impl::secret_form::append(const char* name, std::string_view content) {
   const auto input = forge::crypto::core::secret_string{std::string{content}};
   const auto encoded = std::unique_ptr<char, text_deleter>{oauth2_url_encode(nullptr, input.data())};
   if (!encoded) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification configuration allocation failed");
   }
   if (!value.empty()) {
      value.push_back('&');
   }
   value.append(name).append("=").append(encoded.get());
}

verifier::impl::impl(options value, forge::asio::compute::executor executor)
    : configuration{std::move(value)}, blocking{std::move(executor)},
      vendor_budget{std::chrono::duration_cast<std::chrono::milliseconds>(configuration.http_timeout)} {
   auto form = secret_form{};
   form.append("verify.cache", "none");
   form.append("type", "bearer");
   form.append("iss", configuration.policy.issuer);
   form.append("aud", configuration.policy.audience);
   form.append("verify.iss", "required");
   form.append("verify.aud", "required");
   // Host policy checks strict integer NumericDates before chrono conversion.
   form.append("verify.exp", "skip");
   form.append("verify.nbf", "skip");
   form.append("verify.iat", "skip");
   const auto jwt = std::holds_alternative<jwks>(configuration.source);
   const auto prefix = std::string{jwt ? "jwks_uri." : "introspect."};
   const auto set = [&](const char* name, std::string_view content) { form.append((prefix + name).c_str(), content); };
   set("http_timeout", std::to_string(configuration.http_timeout.count()));
   set("http_retries", "0");
   set("http_follow_redirects", "false");
   set("http_max_redirects", "0");
   set("http_max_response_bytes", std::to_string(configuration.max_response_bytes));
   set("ssl_verify", "true");
   if (!configuration.ca_bundle.empty()) {
      set("ca_info", configuration.ca_bundle.string());
   }
   std::string endpoint;
   if (const auto* source = std::get_if<jwks>(&configuration.source)) {
      endpoint = source->endpoint;
      set("max_keys", std::to_string(source->max_keys));
      set("expiry", std::to_string(source->cache_lifetime.count()));
      set("refresh_interval", std::to_string(source->refresh_interval.count()));
      set("refresh_wait_timeout", std::to_string(source->refresh_wait_timeout.count()));
      vendor_budget += 2 * source->refresh_wait_timeout;
   } else {
      const auto& opaque = std::get<introspection>(configuration.source);
      endpoint = opaque.endpoint;
      set("auth", "client_secret_basic");
      form.append("client_id", opaque.client_id);
      form.append("client_secret", opaque.client_secret.view());
   }
   const auto encoded = forge::crypto::core::secret_string{form.value};
   oauth2_cfg_token_verify_t* raw = nullptr;
   const auto error = std::unique_ptr<char, text_deleter>{oauth2_cfg_token_verify_add_options(
       nullptr, &raw, jwt ? "jwks_uri" : "introspect", endpoint.c_str(), encoded.data())};
   prototype.reset(raw);
   if (error || !prototype) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "verification backend configuration failed");
   }
}

verifier::impl::ticket::ticket(std::shared_ptr<impl> value) : owner{std::move(value)} {}
verifier::impl::ticket::~ticket() {
   owner->release();
}

std::shared_ptr<verifier::impl::ticket> verifier::impl::admit() {
   const auto lock = std::scoped_lock{mutex};
   if (stopping) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::stopped, "verification is stopped");
   }
   if (in_flight >= configuration.max_in_flight) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::busy, "verification admission is full");
   }
   auto result = std::make_shared<ticket>(shared_from_this());
   ++in_flight;
   return result;
}

void verifier::impl::release() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      --in_flight;
   }
   changed.notify();
}

void verifier::impl::request_stop() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      stopping = true;
   }
   stop.request_stop();
   changed.notify();
}

bool verifier::impl::stopped() const noexcept {
   const auto lock = std::scoped_lock{mutex};
   return stopping;
}

bool verifier::impl::drained() const noexcept {
   const auto lock = std::scoped_lock{mutex};
   return in_flight == 0;
}

boost::asio::awaitable<void> verifier::impl::async_wait_stopped() {
   while (true) {
      const auto epoch = changed.epoch();
      if (stopped()) {
         co_return;
      }
      co_await changed.async_wait(epoch);
   }
}

void verifier::impl::completion::run(impl& owner, forge::crypto::core::secret_string bearer,
                                     forge::asio::compute::context& context,
                                     std::chrono::steady_clock::time_point deadline) {
   auto result = std::optional<oauth2::verified_token::claims>{};
   auto failure = std::exception_ptr{};
   try {
      result.emplace(owner.verify(std::move(bearer), context, deadline));
   } catch (...) {
      failure = std::current_exception();
   }
   {
      const auto lock = std::scoped_lock{mutex};
      claims = std::move(result);
      error = std::move(failure);
      finished = true;
   }
   changed.notify();
}

boost::asio::awaitable<bool> verifier::impl::completion::async_wait(std::chrono::steady_clock::time_point deadline) {
   while (true) {
      const auto epoch = changed.epoch();
      {
         const auto lock = std::scoped_lock{mutex};
         if (finished) {
            co_return true;
         }
      }
      // A deadline wake can return the unchanged epoch without an exception.
      if (std::chrono::steady_clock::now() >= deadline) {
         co_return false;
      }
      try {
         co_await changed.async_wait_until(epoch, deadline);
      } catch (const boost::system::system_error&) {
         // Returning a terminal value lets the structured race cancel its peer.
         co_return false;
      }
   }
}

void verifier::impl::validate_header(const forge::crypto::core::secret_string& bearer) const {
   cjose_err error{};
   const auto jws = std::unique_ptr<cjose_jws_t, decltype(&cjose_jws_release)>{
       cjose_jws_import(bearer.data(), bearer.size(), &error), &cjose_jws_release};
   if (!jws) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential rejected");
   }
   auto* header = cjose_jws_get_protected(jws.get());
   const auto* algorithm = header ? cjose_header_get(header, "alg", &error) : nullptr;
   const auto* purpose = header ? cjose_header_get(header, "typ", &error) : nullptr;
   if (!algorithm || !purpose || std::strlen(algorithm) != json_string_length(json_object_get(header, "alg")) ||
       std::strlen(purpose) != json_string_length(json_object_get(header, "typ")) ||
       (std::string_view{purpose} != "at+jwt" && std::string_view{purpose} != "application/at+jwt") ||
       !std::ranges::any_of(configuration.policy.algorithms,
                            [&](auto value) { return std::string_view{algorithm} == algorithm_name(value); })) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential purpose or algorithm rejected");
   }
   // No token-selected key source, critical extension or embedded key is supported.
   for (const auto* name : {"jku", "x5u", "jwk", "x5c", "crit"}) {
      if (json_object_get(header, name)) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential header extension rejected");
      }
   }
   std::uint8_t* plaintext = nullptr;
   std::size_t length = 0;
   if (!cjose_jws_get_plaintext(jws.get(), &plaintext, &length, &error) || std::memchr(plaintext, '\0', length)) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential payload encoding rejected");
   }
}

oauth2::verified_token::claims verifier::impl::verify(forge::crypto::core::secret_string bearer,
                                                      forge::asio::compute::context& context,
                                                      std::chrono::steady_clock::time_point deadline) {
   context.throw_if_stop_requested();
   if (deadline - std::chrono::steady_clock::now() < vendor_budget) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification queue deadline exceeded");
   }
   auto cfg = config_ptr{};
   {
      const auto lock = std::scoped_lock{mutex};
      cfg.reset(oauth2_cfg_token_verify_clone(nullptr, prototype.get()));
   }
   if (!cfg) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification configuration allocation failed");
   }
   json_t* raw = nullptr;
   auto result = oauth2_token_verify_result_t{};
   result.kind = OAUTH2_TOKEN_VERIFY_INTERNAL_ERROR;
   const auto accepted = oauth2_token_verify_ex(nullptr, nullptr, cfg.get(), bearer.data(), &raw, nullptr, &result);
   const auto payload = std::unique_ptr<json_t, decltype(&json_decref)>{raw, &json_decref};
   context.throw_if_stop_requested();
   if (std::chrono::steady_clock::now() >= deadline) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification deadline exceeded");
   }
   if (!accepted) {
      switch (result.kind) {
      case OAUTH2_TOKEN_VERIFY_CREDENTIAL_REJECTED:
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential rejected");
      case OAUTH2_TOKEN_VERIFY_RESPONSE_TOO_LARGE:
      case OAUTH2_TOKEN_VERIFY_KEY_LIMIT:
         FORGE_THROW_EXCEPTION(oauth2::exceptions::response_too_large, "issuer response exceeds limit");
      default:
         FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "issuer verification unavailable");
      }
   }
   if (result.kind != OAUTH2_TOKEN_VERIFY_SUCCESS || !payload) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "issuer verification response is inconsistent");
   }
   if (configuration.policy.profile == oauth2::token_profile::jwt_access_token) {
      validate_header(bearer);
   }
   return normalize(payload.get(), std::chrono::system_clock::now());
}

} // namespace forge::auth::liboauth2
