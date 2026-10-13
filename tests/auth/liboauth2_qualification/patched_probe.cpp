#include <array>
#include <barrier>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

extern "C" {
#include <cjose/cjose.h>
#include <oauth2/oauth2.h>
#include <oauth2/mem.h>
#include <oauth2/version.h>
}

namespace {

constexpr auto issuer = "https://forge-f0.invalid/issuer-a";
constexpr auto second_issuer = "https://forge-f0.invalid/issuer-b";
constexpr auto audience = "forge-f0-resource";
constexpr auto token_canary = "forge-f0-patched-token-canary";
constexpr auto secret_canary = "forge-f0-patched-secret-canary";
constexpr auto assertion_canary = "forge-f0-patched-assertion-canary";

struct fixture_error {};

void require(bool condition) {
   if (!condition)
      throw fixture_error{};
}

struct json_deleter {
   void operator()(json_t* value) const { json_decref(value); }
};
struct key_deleter {
   void operator()(cjose_jwk_t* value) const { cjose_jwk_release(value); }
};
struct header_deleter {
   void operator()(cjose_header_t* value) const { cjose_header_release(value); }
};
struct jws_deleter {
   void operator()(cjose_jws_t* value) const { cjose_jws_release(value); }
};
struct cjose_deleter {
   void operator()(char* value) const { cjose_get_dealloc()(value); }
};
struct oauth_deleter {
   void operator()(char* value) const { oauth2_mem_free(value); }
};
struct verify_deleter {
   void operator()(oauth2_cfg_token_verify_t* value) const {
      oauth2_cfg_token_verify_free(nullptr, value);
   }
};
struct http_deleter {
   void operator()(oauth2_http_call_ctx_t* value) const {
      oauth2_http_call_ctx_free(nullptr, value);
   }
};
using json_ptr = std::unique_ptr<json_t, json_deleter>;
using key_ptr = std::unique_ptr<cjose_jwk_t, key_deleter>;
using jws_ptr = std::unique_ptr<cjose_jws_t, jws_deleter>;
using verify_ptr = std::unique_ptr<oauth2_cfg_token_verify_t, verify_deleter>;

void discard_log(oauth2_log_sink_t*, const char*, unsigned long,
                 const char*, oauth2_log_level_t, const char*) {}

struct vendor_state {
   oauth2_log_t* log = nullptr;
   vendor_state() {
      auto* sink = oauth2_log_sink_create(OAUTH2_LOG_ERROR, discard_log, nullptr);
      require(sink != nullptr);
      log = oauth2_init(OAUTH2_LOG_ERROR, sink);
      if (log == nullptr) {
         oauth2_mem_free(sink);
         oauth2_shutdown(nullptr);
         throw fixture_error{};
      }
   }
   ~vendor_state() { oauth2_shutdown(log); }
};

void deadline(int) { std::_Exit(3); }

bool loopback_url(std::string_view value, bool path) {
   constexpr std::string_view prefix = "http://127.0.0.1:";
   if (!value.starts_with(prefix))
      return false;
   value.remove_prefix(prefix.size());
   const auto slash = value.find('/');
   if (path != (slash != std::string_view::npos))
      return false;
   const auto port_text = value.substr(0, slash);
   unsigned port = 0;
   const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
   return parsed.ec == std::errc{} && parsed.ptr == port_text.data() + port_text.size() &&
      port > 0 && port <= 65535 && (!path || value.substr(slash).find_first_not_of(
         "/abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string_view::npos);
}

constexpr std::array modes = {
   "jwt-positive", "jwt-wrong-issuer", "jwt-wrong-audience", "jwt-wrong-alg",
   "jwt-expired", "jwt-future-nbf", "jwt-missing-exp", "jwt-missing-iss",
   "jwt-missing-aud", "jwt-missing-sub", "jwt-bad-signature", "jwt-malformed",
   "jwt-malformed-payload", "jwt-id-token", "jwt-missing-typ", "jwt-other-key-same-kid",
   "jwt-type-iss", "jwt-type-aud", "jwt-type-sub", "jwt-type-exp", "jwt-type-nbf",
   "jwt-duplicate-iss", "jwt-duplicate-aud", "jwt-duplicate-sub", "jwt-duplicate-exp", "jwt-duplicate-nbf",
   "jwt-audience-array", "jwt-audience-array-mixed", "jwt-hs-rsa-key-confusion",
   "jwt-ec-rsa-key-confusion", "jwt-rsa-oct-key-confusion", "jwt-alg-none",
   "jwks-positive", "jwks-rotation", "jwks-cached-unknown-kid", "jwks-503-first",
   "jwks-503-refresh", "jwks-malformed", "jwks-oversized", "jwks-transport-reset",
   "jwks-timeout", "introspection-positive", "introspection-inactive",
   "jwks-two-issuers-same-kid", "jwks-shared-uri-policy-isolation",
   "jwks-expiry-cache-off", "jwks-uri-cache-off", "jwks-url-as-bearer",
   "jwks-key-limit-positive", "jwks-key-limit-first", "jwks-key-limit-refresh",
   "jwks-kid-throttle", "jwks-refresh-expiry-throttled",
   "jwks-clone-coalescing", "jwks-clone-wait-timeout",
   "jwks-long-unknown-kid-refresh", "jwks-long-lock-wait-expiry",
   "introspection-cache-off", "introspection-expiry-cache-off",
   "introspection-type-iss", "introspection-type-aud", "introspection-type-sub",
   "introspection-type-exp", "introspection-type-nbf", "introspection-type-active",
   "introspection-duplicate-iss", "introspection-duplicate-aud", "introspection-duplicate-sub",
   "introspection-duplicate-exp", "introspection-duplicate-nbf", "introspection-duplicate-active",
   "introspection-missing-iss", "introspection-missing-aud", "introspection-missing-sub",
   "introspection-missing-exp", "introspection-wrong-issuer", "introspection-wrong-audience",
   "introspection-expired", "introspection-503", "introspection-401",
   "introspection-timeout", "introspection-transport-reset", "introspection-status-null",
   "redirect-307", "redirect-308", "http-503-status-null"
};

key_ptr new_key(const char* kid) {
   cjose_err error{};
   constexpr std::array<std::uint8_t, 3> exponent{1, 0, 1};
   key_ptr key(cjose_jwk_create_RSA_random(2048, exponent.data(), exponent.size(), &error));
   require(key != nullptr && cjose_jwk_set_kid(key.get(), kid, std::strlen(kid), &error));
   return key;
}

std::string public_key(cjose_jwk_t* key) {
   cjose_err error{};
   std::unique_ptr<char, cjose_deleter> text(cjose_jwk_to_json(key, false, &error));
   require(text != nullptr);
   json_ptr value(json_loads(text.get(), JSON_REJECT_DUPLICATES, nullptr));
   require(value != nullptr && json_is_object(value.get()));
   for (const auto* field : {"d", "p", "q", "dp", "dq", "qi", "oth", "k"})
      require(json_object_get(value.get(), field) == nullptr);
   return text.get();
}

json_ptr claims(std::string_view variant = {}, const char* expected_issuer = issuer) {
   json_ptr result(json_object());
   require(result != nullptr);
   const auto now = static_cast<json_int_t>(std::time(nullptr));
   require(now > 0);
   auto string = [&](const char* name, const char* value) {
      require(json_object_set_new(result.get(), name, json_string(value)) == 0);
   };
   string("iss", variant == "jwt-wrong-issuer" ? second_issuer : expected_issuer);
   string("aud", variant == "jwt-wrong-audience" ? "forge-f0-other-resource" : audience);
   string("sub", "forge-f0-subject");
   string("fixture_canary", assertion_canary);
   require(json_object_set_new(result.get(), "exp", json_integer(
      now + (variant == "jwt-expired" ? -120 : variant == "short-expiry" ? 2 : 3600))) == 0);
   require(json_object_set_new(result.get(), "nbf", json_integer(
      now + (variant == "jwt-future-nbf" ? 3600 : -120))) == 0);
   for (const auto* field : {"exp", "iss", "aud", "sub"}) {
      if (variant == std::string("jwt-missing-") + field)
         require(json_object_del(result.get(), field) == 0);
   }
   for (const auto* field : {"iss", "aud", "sub", "exp", "nbf"}) {
      if (variant == std::string("jwt-type-") + field)
         require(json_object_set_new(result.get(), field,
            std::string_view(field) == "exp" || std::string_view(field) == "nbf" ?
               json_string("not-a-numeric-date") : json_integer(42)) == 0);
   }
   if (variant == "jwt-audience-array" || variant == "jwt-audience-array-mixed") {
      json_ptr values(json_array());
      require(values != nullptr && json_array_append_new(values.get(), json_string(audience)) == 0);
      require(json_array_append_new(values.get(), variant == "jwt-audience-array-mixed" ?
         json_integer(42) : json_string("forge-f0-second-resource")) == 0);
      require(json_object_set(result.get(), "aud", values.get()) == 0);
   }
   return result;
}

std::string serialize(json_t* value, size_t flags = JSON_COMPACT) {
   std::unique_ptr<char, decltype(&std::free)> encoded(json_dumps(value, flags), &std::free);
   require(encoded != nullptr);
   return encoded.get();
}

std::string base64url(std::string_view value) {
   cjose_err error{};
   char* encoded = nullptr;
   size_t length = 0;
   require(cjose_base64url_encode(reinterpret_cast<const std::uint8_t*>(value.data()),
      value.size(), &encoded, &length, &error));
   std::unique_ptr<char, cjose_deleter> owned(encoded);
   require(owned != nullptr);
   return {owned.get(), length};
}

std::string sign(cjose_jwk_t* key, std::string_view variant = {}, const char* kid = "key-a",
                 const char* expected_issuer = issuer) {
   cjose_err error{};
   std::unique_ptr<cjose_header_t, header_deleter> header(cjose_header_new(&error));
   require(header != nullptr);
   const auto* alg = variant == "jwt-wrong-alg" ? "RS512" :
      variant == "jwt-hs-rsa-key-confusion" ? "HS256" :
      variant == "jwt-ec-rsa-key-confusion" ? "ES256" : "RS256";
   require(cjose_header_set(header.get(), "alg", alg, &error));
   require(cjose_header_set(header.get(), "kid", kid, &error));
   if (variant != "jwt-missing-typ")
      require(cjose_header_set(header.get(), "typ", variant == "jwt-id-token" ? "JWT" : "at+jwt", &error));
   auto value = claims(variant, expected_issuer);
   // The malformed payload is still genuinely signed; parsing is not verification.
   auto payload = variant == "jwt-malformed-payload" ? std::string("{") : serialize(value.get());
   if (variant.starts_with("jwt-duplicate-")) {
      const auto field = std::string(variant.substr(std::string_view("jwt-duplicate-").size()));
      auto* repeated = json_object_get(value.get(), field.c_str());
      require(repeated != nullptr);
      payload.pop_back();
      payload += ",\"" + field + "\":" + serialize(repeated, JSON_COMPACT | JSON_ENCODE_ANY) + "}";
   }
   if (variant == "jwt-alg-none") {
      require(cjose_header_set(header.get(), "alg", "none", &error));
      return base64url(serialize(header.get())) + "." + base64url(payload) + ".";
   }
   jws_ptr signed_value(cjose_jws_sign(key, header.get(),
      reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(), &error));
   require(signed_value != nullptr);
   const char* compact = nullptr;
   require(cjose_jws_export(signed_value.get(), &compact, &error) && compact != nullptr);
   return compact;
}

void signature_control(const std::string& token, const std::string& right, const std::string& wrong) {
   cjose_err error{};
   key_ptr good(cjose_jwk_import(right.data(), right.size(), &error));
   key_ptr bad(cjose_jwk_import(wrong.data(), wrong.size(), &error));
   jws_ptr jws(cjose_jws_import(token.data(), token.size(), &error));
   require(good != nullptr && bad != nullptr && jws != nullptr);
   require(cjose_jws_verify(jws.get(), good.get(), &error));
   require(!cjose_jws_verify(jws.get(), bad.get(), &error));
}

auto http_context(int timeout = 1) {
   std::unique_ptr<oauth2_http_call_ctx_t, http_deleter> ctx(oauth2_http_call_ctx_init(nullptr));
   require(ctx != nullptr);
   require(oauth2_http_call_ctx_timeout_set(nullptr, ctx.get(), timeout));
   require(oauth2_http_call_ctx_retries_set(nullptr, ctx.get(), 0));
   require(oauth2_http_call_ctx_follow_redirects_set(nullptr, ctx.get(), false));
   require(oauth2_http_call_ctx_max_response_bytes_set(nullptr, ctx.get(), 4096));
   return ctx;
}

void install_public_keys(const std::string& origin, const std::string& first, const std::string& second) {
   const std::string document = "{\"keys\":[" + first + "," + second + "]}";
   json_ptr body(json_loads(document.c_str(), JSON_REJECT_DUPLICATES, nullptr));
   require(body != nullptr);
   auto ctx = http_context();
   char* response = nullptr;
   oauth2_http_status_code_t status = 0;
   const bool ok = oauth2_http_post_json(nullptr, (origin + "/fixture-keys").c_str(),
      body.get(), ctx.get(), &response, &status);
   std::unique_ptr<char, oauth_deleter> owned(response);
   require(ok && status == 204);
}

verify_ptr configuration(const char* type, const std::string& value, const char* expected_issuer = issuer,
                          const std::string& extra = {}, int timeout = 1) {
   std::string options = "verify.cache=none&iss=" + std::string(std::string_view(expected_issuer) == second_issuer ?
      "https%3A%2F%2Fforge-f0.invalid%2Fissuer-b" : "https%3A%2F%2Fforge-f0.invalid%2Fissuer-a") + "&"
      "aud=forge-f0-resource&verify.iss=required&verify.aud=required&verify.exp=required&"
      "verify.nbf=optional";
   if (std::string_view(type) != "jwk") {
      const std::string prefix = std::string(type) + ".";
      options += "&" + prefix + "http_timeout=" + std::to_string(timeout) + "&" + prefix + "http_retries=0&" + prefix +
         "http_follow_redirects=false&" + prefix + "http_max_redirects=2&" + prefix +
         "http_max_response_bytes=4096";
      if (std::string_view(type) == "introspect")
         options += "&introspect.auth=client_secret_post&client_id=forge-f0-client&client_secret=" +
            std::string(secret_canary);
   }
   options += extra;
   oauth2_cfg_token_verify_t* raw = nullptr;
   auto* error = oauth2_cfg_token_verify_add_options(nullptr, &raw, type, value.c_str(), options.c_str());
   verify_ptr cfg(raw);
   std::unique_ptr<char, oauth_deleter> owned(error);
   require(error == nullptr && cfg != nullptr);
   verify_ptr clone(oauth2_cfg_token_verify_clone(nullptr, cfg.get()));
   require(clone != nullptr);
   return clone;
}

const char* kind_name(oauth2_token_verify_result_kind_t kind) {
   switch (kind) {
      case OAUTH2_TOKEN_VERIFY_SUCCESS: return "success";
      case OAUTH2_TOKEN_VERIFY_CREDENTIAL_REJECTED: return "credential-rejected";
      case OAUTH2_TOKEN_VERIFY_TRANSPORT_ERROR: return "transport-error";
      case OAUTH2_TOKEN_VERIFY_TIMEOUT: return "timeout";
      case OAUTH2_TOKEN_VERIFY_HTTP_ERROR: return "http-error";
      case OAUTH2_TOKEN_VERIFY_MALFORMED_RESPONSE: return "malformed-response";
      case OAUTH2_TOKEN_VERIFY_RESPONSE_TOO_LARGE: return "response-too-large";
      case OAUTH2_TOKEN_VERIFY_REDIRECT_LIMIT: return "redirect-limit";
      case OAUTH2_TOKEN_VERIFY_KEY_LIMIT: return "key-limit";
      case OAUTH2_TOKEN_VERIFY_REFRESH_TIMEOUT: return "refresh-timeout";
      case OAUTH2_TOKEN_VERIFY_REFRESH_THROTTLED: return "refresh-throttled";
      case OAUTH2_TOKEN_VERIFY_INTERNAL_ERROR: return "internal-error";
   }
   return "unknown";
}

const char* http_name(oauth2_http_call_error_t error) {
   switch (error) {
      case OAUTH2_HTTP_CALL_OK: return "ok";
      case OAUTH2_HTTP_CALL_TRANSPORT_ERROR: return "transport-error";
      case OAUTH2_HTTP_CALL_TIMEOUT: return "timeout";
      case OAUTH2_HTTP_CALL_RESPONSE_TOO_LARGE: return "response-too-large";
      case OAUTH2_HTTP_CALL_REDIRECT_LIMIT: return "redirect-limit";
      case OAUTH2_HTTP_CALL_INTERNAL_ERROR: return "internal-error";
   }
   return "unknown";
}

bool string_equal(json_t* value, const char* expected) {
   return json_is_string(value) && json_string_length(value) == std::strlen(expected) &&
      std::memcmp(json_string_value(value), expected, json_string_length(value)) == 0;
}

bool audience_matches(json_t* value) {
   if (json_is_string(value))
      return string_equal(value, audience);
   if (!json_is_array(value) || json_array_size(value) == 0)
      return false;
   bool matches = false;
   for (size_t index = 0; index < json_array_size(value); ++index) {
      auto* entry = json_array_get(value, index);
      if (!json_is_string(entry) || json_string_length(entry) == 0)
         return false;
      matches |= string_equal(entry, audience);
   }
   return matches;
}

bool claims_policy(json_t* payload, const char* expected_issuer = issuer) {
   if (!json_is_object(payload) || !string_equal(json_object_get(payload, "iss"), expected_issuer) ||
       !audience_matches(json_object_get(payload, "aud")))
      return false;
   auto* subject = json_object_get(payload, "sub");
   auto* expiry = json_object_get(payload, "exp");
   auto* nbf = json_object_get(payload, "nbf");
   const auto now = static_cast<json_int_t>(std::time(nullptr));
   return json_is_string(subject) && json_string_length(subject) > 0 &&
      json_is_integer(expiry) && json_integer_value(expiry) > now &&
      (!nbf || (json_is_integer(nbf) && json_integer_value(nbf) <= now));
}

const char* jwt_policy(const std::string& verified_bytes, const char* expected_issuer,
                       const char* allowed_alg) {
   // Import only the SAME immutable bytes accepted by the backend. No payload/header
   // reconstruction and no token-selected lookup: CJOSE owns the protected header.
   cjose_err error{};
   jws_ptr jws(cjose_jws_import(verified_bytes.data(), verified_bytes.size(), &error));
   if (!jws)
      return "format";
   auto* header = cjose_jws_get_protected(jws.get());
   require(header != nullptr);
   const char* alg = cjose_header_get(header, "alg", &error);
   const char* typ = cjose_header_get(header, "typ", &error);
   if (!alg || std::strcmp(alg, allowed_alg) != 0)
      return "algorithm";
   if (!typ || std::strcmp(typ, "at+jwt") != 0)
      return "purpose";
   std::uint8_t* raw = nullptr;
   size_t length = 0;
   require(cjose_jws_get_plaintext(jws.get(), &raw, &length, &error));
   json_ptr payload(json_loadb(reinterpret_cast<const char*>(raw), length, JSON_REJECT_DUPLICATES, nullptr));
   return claims_policy(payload.get(), expected_issuer) ? "none" : "claims";
}

struct observation {
   const char* api = "ex";
   bool vendor_accepted = false;
   bool accepted = false;
   bool status_null = false;
   bool payload_present = false;
   const char* kind = "internal-error";
   const char* http_error = "ok";
   const char* policy = "not-applied";
   oauth2_http_status_code_t status = 0;
   oauth2_http_status_code_t endpoint_status = 0;
   long long elapsed_ms = 0;
   json_int_t expires_at = 0;
};

observation verify(verify_ptr& cfg, const std::string& bytes, bool jwt,
                   bool status_null = false, bool legacy = false,
                   const char* expected_issuer = issuer, const char* allowed_alg = "RS256") {
   observation result;
   result.status_null = status_null;
   result.api = legacy ? "legacy" : "ex";
   json_t* raw = nullptr;
   oauth2_token_verify_result_t evidence{};
   evidence.kind = OAUTH2_TOKEN_VERIFY_INTERNAL_ERROR;
   const auto start = std::chrono::steady_clock::now();
   if (legacy) {
      result.vendor_accepted = oauth2_token_verify(nullptr, nullptr, cfg.get(), bytes.c_str(),
         &raw, status_null ? nullptr : &result.status);
      result.kind = "legacy";
   } else {
      result.vendor_accepted = oauth2_token_verify_ex(nullptr, nullptr, cfg.get(), bytes.c_str(),
         &raw, status_null ? nullptr : &result.status, &evidence);
      result.kind = kind_name(evidence.kind);
      result.http_error = http_name(evidence.http_error);
      result.endpoint_status = evidence.status_code;
   }
   result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
   json_ptr payload(raw);
   result.payload_present = payload != nullptr;
   if (result.vendor_accepted) {
      require(payload != nullptr);
      result.policy = jwt ? jwt_policy(bytes, expected_issuer, allowed_alg) :
         (claims_policy(payload.get(), expected_issuer) ? "none" : "claims");
      auto* expiry = json_object_get(payload.get(), "exp");
      if (json_is_integer(expiry))
         result.expires_at = json_integer_value(expiry);
   }
   result.accepted = result.vendor_accepted && std::strcmp(result.policy, "none") == 0;
   return result;
}

void wait_past_expiry(json_int_t expiry) {
   const auto now = static_cast<json_int_t>(std::time(nullptr));
   require(expiry >= now && expiry - now <= 3);
   std::this_thread::sleep_until(std::chrono::system_clock::from_time_t(static_cast<std::time_t>(expiry + 1)));
}

void fixture_control(const std::string& control, const char* phase) {
   json_ptr body(json_object());
   require(body != nullptr && json_object_set_new(body.get(), "phase", json_string(phase)) == 0);
   auto ctx = http_context(std::string_view(phase) == "long-release" ? 4 : 1);
   char* response = nullptr;
   oauth2_http_status_code_t status = 0;
   const bool ok = oauth2_http_post_json(nullptr, (control + "/fixture-control").c_str(),
      body.get(), ctx.get(), &response, &status);
   std::unique_ptr<char, oauth_deleter> owned(response);
   require(ok && status == 204);
}

std::vector<observation> run(std::string_view mode, const std::string& origin,
                            const std::string& endpoint, const std::string& control,
                            bool& crypto_control) {
   require(std::strcmp(oauth2_version(), "2.4.0") == 0 &&
      std::strcmp(OAUTH2_PACKAGE_VERSION, "2.4.0") == 0 &&
      std::strcmp(CJOSE_VERSION, "0.6.2.8") == 0 && std::strcmp(cjose_version(), "0.6.2.8") == 0);
   vendor_state state;
   const bool same_kid = mode == "jwks-two-issuers-same-kid";
   auto first = new_key(same_kid ? "same-kid" : "key-a");
   auto second = new_key(same_kid ? "same-kid" : "key-b");
   const auto first_public = public_key(first.get());
   const auto second_public = public_key(second.get());
   const auto good = sign(first.get(), {}, same_kid ? "same-kid" : "key-a");
   signature_control(good, first_public, second_public);
   crypto_control = true;
   std::vector<observation> results;
   if (mode.starts_with("jwt-")) {
      auto cfg = configuration("jwk", first_public);
      results.push_back(verify(cfg, good, true));
      if (mode != "jwt-positive") {
         const bool other_key = mode == "jwt-other-key-same-kid";
         cjose_err error{};
         key_ptr special;
         std::string special_json;
         if (mode == "jwt-hs-rsa-key-confusion" || mode == "jwt-rsa-oct-key-confusion") {
            special.reset(cjose_jwk_create_oct_spec(reinterpret_cast<const std::uint8_t*>(first_public.data()),
               first_public.size(), &error));
            require(special != nullptr && cjose_jwk_set_kid(special.get(), "key-a", 5, &error));
            std::unique_ptr<char, cjose_deleter> text(cjose_jwk_to_json(special.get(), true, &error));
            require(text != nullptr);
            special_json = text.get();
         } else if (mode == "jwt-ec-rsa-key-confusion") {
            special.reset(cjose_jwk_create_EC_random(CJOSE_JWK_EC_P_256, &error));
            require(special != nullptr);
            special_json = public_key(special.get());
         }
         if (mode == "jwt-rsa-oct-key-confusion") {
            auto oct_cfg = configuration("jwk", special_json);
            const auto hs = sign(special.get(), "jwt-hs-rsa-key-confusion");
            signature_control(hs, special_json, first_public);
            results.push_back(verify(oct_cfg, hs, true, false, false, issuer, "HS256"));
            results.push_back(verify(oct_cfg, good, true));
            results.push_back(verify(cfg, good, true));
            return results;
         }
         auto candidate = sign(special ? special.get() : other_key ? second.get() : first.get(), mode);
         if (mode != "jwt-alg-none")
            signature_control(candidate, special ? special_json : other_key ? second_public : first_public,
               special || other_key ? first_public : second_public);
         if (mode == "jwt-malformed")
            candidate = "not-a-compact-jwt";
         if (mode == "jwt-bad-signature") {
            const auto signature = candidate.rfind('.') + 1;
            require(signature > 1 && signature < candidate.size());
            candidate[signature] = candidate[signature] == 'A' ? 'B' : 'A';
         }
         results.push_back(verify(cfg, candidate, true));
         results.push_back(verify(cfg, good, true));
      }
      return results;
   }
   if (mode == "http-503-status-null") {
      auto ctx = http_context();
      char* response = nullptr;
      oauth2_http_call_result_t evidence{};
      observation result;
      result.api = "http";
      result.status_null = true;
      result.vendor_accepted = oauth2_http_get_ex(nullptr, endpoint.c_str(), nullptr, ctx.get(),
         &response, nullptr, &evidence);
      std::unique_ptr<char, oauth_deleter> body(response);
      result.payload_present = body != nullptr;
      result.accepted = result.vendor_accepted;
      result.kind = "http-result";
      result.http_error = http_name(evidence.error);
      result.endpoint_status = evidence.status_code;
      results.push_back(result);
      return results;
   }
   if (mode.starts_with("jwks-")) {
      install_public_keys(origin, first_public, second_public);
      if (mode == "jwks-two-issuers-same-kid" || mode == "jwks-shared-uri-policy-isolation") {
         const auto path_a = same_kid ? endpoint + "/issuer-a" : endpoint;
         const auto path_b = same_kid ? endpoint + "/issuer-b" : endpoint;
         auto cfg_a = configuration("jwks_uri", path_a);
         auto cfg_b = configuration("jwks_uri", path_b, second_issuer);
         const auto token_b = sign(same_kid ? second.get() : first.get(), {},
            same_kid ? "same-kid" : "key-a", second_issuer);
         signature_control(token_b, same_kid ? second_public : first_public, same_kid ? first_public : second_public);
         results.push_back(verify(cfg_a, good, true));
         results.push_back(verify(cfg_b, token_b, true, false, false, second_issuer));
         results.push_back(verify(cfg_b, good, true, false, false, second_issuer));
         results.push_back(verify(cfg_a, token_b, true));
         if (same_kid) {
            const auto forged_b = sign(first.get(), {}, "same-kid", second_issuer);
            const auto forged_a = sign(second.get(), {}, "same-kid", issuer);
            signature_control(forged_b, first_public, second_public);
            signature_control(forged_a, second_public, first_public);
            results.push_back(verify(cfg_b, forged_b, true, false, false, second_issuer));
            results.push_back(verify(cfg_a, forged_a, true));
         }
         results.push_back(verify(cfg_a, good, true));
         results.push_back(verify(cfg_b, token_b, true, false, false, second_issuer));
         return results;
      }
      std::string extra;
      const bool long_refresh = mode == "jwks-long-unknown-kid-refresh" || mode == "jwks-long-lock-wait-expiry";
      if (mode.starts_with("jwks-key-limit-"))
         extra = "&jwks_uri.max_keys=1";
      if (mode == "jwks-uri-cache-off")
         extra = "&jwks_uri.cache=none";
      if (mode == "jwks-kid-throttle" || mode == "jwks-refresh-expiry-throttled" ||
          mode == "jwks-clone-coalescing" || mode == "jwks-clone-wait-timeout")
         extra = "&jwks_uri.max_keys=2&jwks_uri.refresh_interval=60&jwks_uri.refresh_wait_timeout=" +
            std::string(mode == "jwks-clone-wait-timeout" ? "100" : "1500");
      if (mode == "jwks-refresh-expiry-throttled")
         extra += "&jwks_uri.expiry=1";
      if (long_refresh)
         extra = "&jwks_uri.max_keys=2&jwks_uri.refresh_interval=1&jwks_uri.refresh_wait_timeout=" +
            std::string(mode == "jwks-long-lock-wait-expiry" ? "100" : "4000");
      auto cfg = configuration("jwks_uri", endpoint, issuer, extra, long_refresh ? 4 : 1);
      if (mode == "jwks-url-as-bearer") {
         results.push_back(verify(cfg, good, true));
         results.push_back(verify(cfg, endpoint, true));
         results.push_back(verify(cfg, good, true));
         return results;
      }
      if (long_refresh) {
         verify_ptr clone(oauth2_cfg_token_verify_clone(nullptr, cfg.get()));
         require(clone != nullptr);
         results.push_back(verify(cfg, good, true));
         const auto missing = sign(second.get(), {}, "unknown-long-refresh");
         signature_control(missing, second_public, first_public);
         // Let the configured one-second cooldown expire before triggering the
         // refresh. The separate control server then holds the ACTUAL HTTP reply
         // for two seconds, outlasting that interval without an extra flight.
         std::this_thread::sleep_for(std::chrono::milliseconds(1100));
         observation first_result;
         observation second_result;
         std::exception_ptr first_failure;
         std::exception_ptr second_failure;
         if (mode == "jwks-long-unknown-kid-refresh") {
            std::barrier start(3);
            std::jthread first_call([&] {
               start.arrive_and_wait();
               try { first_result = verify(cfg, missing, true); } catch (...) { first_failure = std::current_exception(); }
            });
            std::jthread second_call([&] {
               start.arrive_and_wait();
               try { second_result = verify(clone, missing, true); } catch (...) { second_failure = std::current_exception(); }
            });
            start.arrive_and_wait();
            fixture_control(control, "await");
            fixture_control(control, "long-release");
            first_call.join();
            second_call.join();
         } else {
            std::jthread first_call([&] {
               try { first_result = verify(cfg, missing, true); } catch (...) { first_failure = std::current_exception(); }
            });
            fixture_control(control, "await");
            second_result = verify(clone, good, true);
            fixture_control(control, "long-release");
            first_call.join();
         }
         require(!first_failure && !second_failure);
         results.push_back(first_result);
         results.push_back(second_result);
         results.push_back(verify(clone, good, true));
         return results;
      }
      if (mode == "jwks-clone-coalescing" || mode == "jwks-clone-wait-timeout") {
         verify_ptr clone(oauth2_cfg_token_verify_clone(nullptr, cfg.get()));
         require(clone != nullptr);
         observation first_result;
         observation second_result;
         std::exception_ptr first_failure;
         std::exception_ptr second_failure;
         if (mode == "jwks-clone-coalescing") {
            std::barrier start(3);
            std::jthread first_call([&] {
               start.arrive_and_wait();
               try { first_result = verify(cfg, good, true); } catch (...) { first_failure = std::current_exception(); }
            });
            std::jthread second_call([&] {
               start.arrive_and_wait();
               try { second_result = verify(clone, good, true); } catch (...) { second_failure = std::current_exception(); }
            });
            start.arrive_and_wait();
            fixture_control(control, "await");
            fixture_control(control, "release");
            first_call.join();
            second_call.join();
         } else {
            std::jthread first_call([&] {
               try { first_result = verify(cfg, good, true); } catch (...) { first_failure = std::current_exception(); }
            });
            fixture_control(control, "await");
            second_result = verify(clone, good, true);
            fixture_control(control, "release");
            first_call.join();
         }
         require(!first_failure && !second_failure);
         results.push_back(first_result);
         results.push_back(second_result);
         results.push_back(verify(clone, good, true));
         return results;
      }
      if (mode == "jwks-expiry-cache-off") {
         const auto short_token = sign(first.get(), "short-expiry");
         signature_control(short_token, first_public, second_public);
         results.push_back(verify(cfg, short_token, true));
         results.push_back(verify(cfg, short_token, true));
         wait_past_expiry(results[0].expires_at);
         results.push_back(verify(cfg, short_token, true));
         results.push_back(verify(cfg, good, true));
         return results;
      }
      results.push_back(verify(cfg, good, true));
      if (mode == "jwks-kid-throttle") {
         for (unsigned index = 0; index < 5; ++index) {
            const auto kid = "unknown-" + std::to_string(index);
            const auto missing = sign(second.get(), {}, kid.c_str());
            signature_control(missing, second_public, first_public);
            results.push_back(verify(cfg, missing, true));
         }
         results.push_back(verify(cfg, good, true));
      } else if (mode == "jwks-refresh-expiry-throttled") {
         results.push_back(verify(cfg, good, true));
         std::this_thread::sleep_for(std::chrono::seconds(2));
         results.push_back(verify(cfg, good, true));
      } else if (mode == "jwks-uri-cache-off") {
         results.push_back(verify(cfg, good, true));
      } else if (mode == "jwks-key-limit-refresh") {
         results.push_back(verify(cfg, good, true));
         const auto rotated = sign(second.get(), {}, "key-b");
         signature_control(rotated, second_public, first_public);
         results.push_back(verify(cfg, rotated, true));
      }
      if (mode == "jwks-rotation" || mode == "jwks-cached-unknown-kid" || mode == "jwks-503-refresh") {
         results.push_back(verify(cfg, good, true));
         const auto rotated = sign(second.get(), {}, mode == "jwks-cached-unknown-kid" ? "absent" : "key-b");
         signature_control(rotated, second_public, first_public);
         results.push_back(verify(cfg, rotated, true));
         results.push_back(verify(cfg, mode == "jwks-cached-unknown-kid" ? good : rotated, true));
      }
      return results;
   }
   auto cfg = configuration("introspect", endpoint);
   if (mode == "introspection-status-null") {
      results.push_back(verify(cfg, token_canary, false, true));
      results.push_back(verify(cfg, token_canary, false, true, true));
   } else if (mode == "introspection-cache-off" || mode == "introspection-expiry-cache-off") {
      results.push_back(verify(cfg, token_canary, false));
      if (mode == "introspection-expiry-cache-off")
         wait_past_expiry(results[0].expires_at);
      results.push_back(verify(cfg, token_canary, false));
      if (mode == "introspection-cache-off")
         results.push_back(verify(cfg, token_canary, false));
   } else {
      results.push_back(verify(cfg, token_canary, false));
   }
   return results;
}

void print(std::string_view mode, const std::vector<observation>& results,
           bool completed, bool crypto_control) {
   std::printf("{\"case\":\"%.*s\",\"completed\":%s,\"crypto_control\":%s,"
      "\"result_cache_disabled\":%s,\"steps\":[",
      static_cast<int>(mode.size()), mode.data(), completed ? "true" : "false",
      crypto_control ? "true" : "false", "true");
   bool first = true;
   for (const auto& result : results) {
      std::printf("%s{\"api\":\"%s\",\"vendor_accepted\":%s,\"accepted\":%s,"
         "\"status_null\":%s,\"payload_present\":%s,\"kind\":\"%s\",\"http_error\":\"%s\","
         "\"policy\":\"%s\",\"status\":%u,\"endpoint_status\":%u,\"elapsed_ms\":%lld}",
         first ? "" : ",", result.api, result.vendor_accepted ? "true" : "false",
         result.accepted ? "true" : "false", result.status_null ? "true" : "false",
         result.payload_present ? "true" : "false", result.kind, result.http_error, result.policy,
         static_cast<unsigned>(result.status), static_cast<unsigned>(result.endpoint_status), result.elapsed_ms);
      first = false;
   }
   std::printf("]}\n");
}

} // namespace

int main(int argc, char** argv) {
   bool valid = argc == 5 && loopback_url(argv[2], false) && loopback_url(argv[3], true) && loopback_url(argv[4], false);
   if (valid) {
      valid = false;
      for (const auto* mode : modes)
         valid |= std::string_view(argv[1]) == mode;
   }
   const std::string_view mode = valid ? argv[1] : "usage";
   bool completed = false;
   bool crypto_control = false;
   std::vector<observation> results;
   if (valid) {
      if (std::signal(SIGALRM, deadline) == SIG_ERR)
         return 2;
      ::alarm(10);
      try {
         results = run(mode, argv[2], argv[3], argv[4], crypto_control);
         completed = true;
      } catch (...) {
         // Exception text, vendor errors, JWT bytes and assertions are never output.
      }
      ::alarm(0);
   }
   print(mode, results, completed, crypto_control);
   return completed ? 0 : 2;
}
