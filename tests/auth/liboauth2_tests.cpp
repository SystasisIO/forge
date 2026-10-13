#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

#include <cjose/cjose.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.auth.liboauth2.verifier;
import forge.net.http.route_context;
import forge.net.http.server;
import forge.net.http.types;
import forge.net.tls.context;
import forge.net.tls.options;

namespace {

namespace oauth2 = forge::auth::oauth2;
using adapter = forge::auth::liboauth2::verifier;
using secret = forge::crypto::core::secret_string;
using namespace std::chrono_literals;

void require(bool condition) {
   if (!condition) {
      throw std::runtime_error{"auth fixture setup failed"};
   }
}

struct cjose_text_deleter {
   void operator()(char* value) const {
      cjose_get_dealloc()(value);
   }
};
struct key_deleter {
   void operator()(cjose_jwk_t* value) const {
      cjose_jwk_release(value);
   }
};
struct json_deleter {
   void operator()(json_t* value) const {
      json_decref(value);
   }
};
using key_ptr = std::unique_ptr<cjose_jwk_t, key_deleter>;
using json_ptr = std::unique_ptr<json_t, json_deleter>;

key_ptr key() {
   cjose_err error{};
   constexpr std::array<std::uint8_t, 3> exponent{1, 0, 1};
   auto result = key_ptr{cjose_jwk_create_RSA_random(2048, exponent.data(), exponent.size(), &error)};
   require(result && cjose_jwk_set_kid(result.get(), "same-kid", 8, &error));
   return result;
}

std::string public_key(cjose_jwk_t* value) {
   cjose_err error{};
   const auto encoded = std::unique_ptr<char, cjose_text_deleter>{cjose_jwk_to_json(value, false, &error)};
   require(encoded != nullptr);
   return encoded.get();
}

std::string encode(json_t* value) {
   const auto text = std::unique_ptr<char, decltype(&std::free)>{json_dumps(value, JSON_COMPACT), &std::free};
   require(text != nullptr);
   return text.get();
}

struct tls_identity {
   std::string certificate;
   std::string private_key;
};

tls_identity make_tls_identity() {
   const auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>{
       EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free};
   require(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
           EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1);
   EVP_PKEY* raw = nullptr;
   require(EVP_PKEY_keygen(context.get(), &raw) == 1);
   const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>{raw, &EVP_PKEY_free};
   const auto cert = std::unique_ptr<X509, decltype(&X509_free)>{X509_new(), &X509_free};
   require(cert && X509_set_version(cert.get(), 2) == 1 &&
           ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1 &&
           X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) &&
           X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) && X509_set_pubkey(cert.get(), key.get()) == 1);
   auto* subject = X509_get_subject_name(cert.get());
   require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("auth-test"),
                                      -1, -1, 0) == 1 &&
           X509_set_issuer_name(cert.get(), subject) == 1);
   X509V3_CTX extensions{};
   X509V3_set_ctx(&extensions, cert.get(), cert.get(), nullptr, nullptr, 0);
   for (const auto& [name, text] :
        std::array{std::pair{NID_basic_constraints, "critical,CA:TRUE"},
                   std::pair{NID_key_usage, "critical,keyCertSign,digitalSignature,keyEncipherment"},
                   std::pair{NID_subject_alt_name, "IP:127.0.0.1"}}) {
      const auto extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>{
          X509V3_EXT_conf_nid(nullptr, &extensions, name, text), &X509_EXTENSION_free};
      require(extension && X509_add_ext(cert.get(), extension.get(), -1) == 1);
   }
   require(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0);
   const auto cert_bio = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), &BIO_free};
   const auto key_bio = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), &BIO_free};
   require(cert_bio && key_bio && PEM_write_bio_X509(cert_bio.get(), cert.get()) == 1 &&
           PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
   const auto read = [](BIO* bio) {
      char* text = nullptr;
      const auto length = BIO_get_mem_data(bio, &text);
      require(length > 0);
      return std::string{text, static_cast<std::size_t>(length)};
   };
   return {read(cert_bio.get()), read(key_bio.get())};
}

boost::asio::awaitable<std::optional<oauth2::verified_token>>
own_result(boost::asio::awaitable<oauth2::verified_token> operation) {
   co_return std::optional<oauth2::verified_token>{co_await std::move(operation)};
}

class fixture {
 public:
   fixture()
       : runtime{forge::asio::runtime_options{.worker_threads = 2}},
         pool{forge::asio::compute::pool::options{
             .worker_threads = 2, .max_pending_tasks = 8, .max_waiting_submissions = 0}},
         first{key()}, second{key()} {
      static std::atomic<unsigned> sequence{0};
      directory = std::filesystem::temp_directory_path() /
                  ("forge-auth-tls-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                   "-" + std::to_string(sequence.fetch_add(1)));
      std::filesystem::create_directories(directory);
      ca = directory / "ca.pem";
      const auto identity = make_tls_identity();
      auto file = std::ofstream{ca, std::ios::binary};
      file << identity.certificate;
      file.close();
      require(file.good());
      auto tls = std::make_shared<forge::net::tls::context_provider>(forge::net::tls::context_options{
          .role = forge::net::tls::endpoint_role::server,
          .verification = forge::net::tls::peer_verification::none,
          .certificate_chain_pem = identity.certificate,
          .private_key_pem = identity.private_key,
      });
      server = std::make_unique<forge::net::http::server>(
          runtime, forge::net::http::server_config{.tls_context_provider = std::move(tls)},
          [this](forge::net::http::route_context& context) -> boost::asio::awaitable<forge::net::http::response> {
             const auto epoch = released.epoch();
             const auto path = std::string{context.request.target()};
             requests.fetch_add(1);
             seen.notify();
             if (hold.load()) {
                co_await released.async_wait(epoch);
             }
             auto result = forge::net::http::response{};
             result.version(context.request.version());
             result.result(static_cast<forge::net::http::status>(status.load()));
             result.set(forge::net::http::field::content_type, "application/json");
             if (path == "/redirect") {
                const auto lock = std::scoped_lock{mutex};
                result.result(static_cast<forge::net::http::status>(307));
                result.set("Location", origin + "/after-redirect");
             } else {
                const auto lock = std::scoped_lock{mutex};
                result.body() = path == "/keys" ? keys : introspected;
             }
             result.prepare_payload();
             co_return result;
          });
      forge::asio::blocking::run(runtime, server->async_start());
      const auto lock = std::scoped_lock{mutex};
      origin = "https://127.0.0.1:" + std::to_string(server->port());
      issuer = origin + "/issuer";
      keys = "{\"keys\":[" + public_key(first.get()) + "]}";
      auto claims = payload();
      require(json_object_set_new(claims.get(), "active", json_true()) == 0 &&
              json_object_set_new(claims.get(), "token_type", json_string("Bearer")) == 0);
      introspected = encode(claims.get());
   }

   ~fixture() {
      release();
      for (auto& instance : instances) {
         instance->request_stop();
      }
      for (auto& instance : instances) {
         try {
            forge::asio::blocking::run(runtime, instance->async_drain());
         } catch (...) {
         }
      }
      try {
         forge::asio::blocking::run(runtime, server->async_stop());
      } catch (...) {
      }
      try {
         forge::asio::blocking::run(runtime, pool.shutdown());
      } catch (...) {
      }
      runtime.stop();
      auto error = std::error_code{};
      std::filesystem::remove_all(directory, error);
   }

   adapter::options jwt_options(std::string endpoint = "/keys") const {
      return {
          .policy = {.issuer = issuer,
                     .audience = "resource-api",
                     .version = "v1",
                     .algorithms = {oauth2::signature_algorithm::rs256},
                     .required_scopes = {"read"}},
          .source = adapter::jwks{.endpoint = origin + endpoint,
                                  .max_keys = 4,
                                  .cache_lifetime = 60s,
                                  .refresh_interval = 1s,
                                  .refresh_wait_timeout = 100ms},
          .ca_bundle = ca,
          .http_timeout = 1s,
          .timeout = 4s,
      };
   }

   adapter::options opaque_options(std::string endpoint = "/introspect") const {
      auto result = jwt_options();
      result.policy.profile = oauth2::token_profile::opaque_introspection;
      result.policy.algorithms.clear();
      result.source = adapter::introspection{.endpoint = origin + endpoint,
                                             .client_id = "resource-server",
                                             .client_secret = secret{"fixture-server-secret-never-log"}};
      return result;
   }

   std::shared_ptr<adapter> create(adapter::options options) {
      auto result = adapter::create(std::move(options), pool.get_executor());
      instances.push_back(result);
      return result;
   }

   json_ptr payload() const {
      auto result = json_ptr{json_object()};
      const auto now =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      require(result != nullptr);
      require(json_object_set_new(result.get(), "iss", json_string(issuer.c_str())) == 0 &&
              json_object_set_new(result.get(), "sub", json_string("alice")) == 0 &&
              json_object_set_new(result.get(), "aud", json_string("resource-api")) == 0 &&
              json_object_set_new(result.get(), "scope", json_string("read write")) == 0 &&
              json_object_set_new(result.get(), "exp", json_integer(now + 3600)) == 0 &&
              json_object_set_new(result.get(), "nbf", json_integer(now - 60)) == 0 &&
              json_object_set_new(result.get(), "iat", json_integer(now - 60)) == 0 &&
              json_object_set_new(result.get(), "client_id", json_string("native-client")) == 0 &&
              json_object_set_new(result.get(), "jti", json_string("fixture-token-reference")) == 0);
      return result;
   }

   secret sign(std::function<void(json_t*)> edit = {}, const char* typ = "at+jwt", const char* alg = "RS256",
               cjose_jwk_t* signing_key = nullptr, std::string raw_payload = {}) const {
      cjose_err error{};
      const auto header = std::unique_ptr<cjose_header_t, decltype(&cjose_header_release)>{cjose_header_new(&error),
                                                                                           &cjose_header_release};
      require(header && cjose_header_set(header.get(), "alg", alg, &error) &&
              cjose_header_set(header.get(), "kid", "same-kid", &error) &&
              cjose_header_set(header.get(), "typ", typ, &error));
      auto claims = payload();
      if (edit) {
         edit(claims.get());
      }
      if (raw_payload.empty()) {
         raw_payload = encode(claims.get());
      }
      const auto jws = std::unique_ptr<cjose_jws_t, decltype(&cjose_jws_release)>{
          cjose_jws_sign(signing_key ? signing_key : first.get(), header.get(),
                         reinterpret_cast<const std::uint8_t*>(raw_payload.data()), raw_payload.size(), &error),
          &cjose_jws_release};
      require(jws != nullptr);
      const char* compact = nullptr;
      require(cjose_jws_export(jws.get(), &compact, &error) && compact);
      return secret{compact};
   }

   oauth2::verified_token verify(const std::shared_ptr<adapter>& instance, secret token,
                                 const oauth2::verification_policy& policy) {
      auto result =
          boost::asio::co_spawn(runtime.context(), own_result(instance->async_verify(std::move(token), policy)),
                                boost::asio::use_future)
              .get();
      require(result.has_value());
      return std::move(*result);
   }

   boost::asio::awaitable<void> await_request(std::size_t count) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (true) {
         const auto epoch = seen.epoch();
         if (requests.load() >= count) {
            co_return;
         }
         if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error{"auth fixture request deadline exceeded"};
         }
         co_await seen.async_wait_until(epoch, deadline);
      }
   }

   void release() {
      hold.store(false);
      released.notify();
   }

   forge::asio::runtime runtime;
   forge::asio::compute::pool pool;
   key_ptr first;
   key_ptr second;
   std::filesystem::path directory;
   std::filesystem::path ca;
   std::unique_ptr<forge::net::http::server> server;
   std::string origin;
   std::string issuer;
   std::mutex mutex;
   std::string keys;
   std::string introspected;
   std::atomic<int> status{200};
   std::atomic<bool> hold{false};
   std::atomic<std::size_t> requests{0};
   forge::asio::notification seen;
   forge::asio::notification released;
   std::vector<std::shared_ptr<adapter>> instances;
};

} // namespace

BOOST_AUTO_TEST_SUITE(liboauth2_tests)

BOOST_AUTO_TEST_CASE(strict_urls_profiles_and_finite_limits_are_not_decorative) {
   auto f = fixture{};
   const auto expect_invalid = [&](adapter::options options) {
      BOOST_CHECK_THROW((void)f.create(std::move(options)), oauth2::exceptions::invalid_request);
   };
   for (const auto* endpoint : {"http://issuer.example/keys", "https://user@issuer.example/keys",
                                "https://issuer.example/keys#fragment", "https://issuer.example/keys?q=secret"}) {
      auto options = f.jwt_options();
      std::get<adapter::jwks>(options.source).endpoint = endpoint;
      expect_invalid(std::move(options));
   }
   auto options = f.jwt_options();
   options.max_in_flight = 0;
   expect_invalid(options);
   options = f.jwt_options();
   options.timeout = 100ms;
   expect_invalid(options);
   options = f.jwt_options();
   std::get<adapter::jwks>(options.source).max_keys = 0;
   expect_invalid(options);
   options = f.jwt_options();
   std::get<adapter::jwks>(options.source).refresh_interval = 0s;
   expect_invalid(options);
   options = f.jwt_options();
   options.source = adapter::introspection{.endpoint = f.origin + "/introspect"};
   expect_invalid(options);
   BOOST_CHECK_THROW((void)adapter::create(f.jwt_options(), {}), oauth2::exceptions::invalid_request);
   BOOST_CHECK_EQUAL(f.requests.load(), 0U);
}

BOOST_AUTO_TEST_CASE(jwt_normalizes_verified_evidence_and_requires_the_exact_configured_policy) {
   auto f = fixture{};
   auto options = f.jwt_options();
   const auto instance = f.create(options);
   const auto evidence = f.verify(instance, f.sign(), options.policy);
   BOOST_CHECK_EQUAL(evidence.issuer(), f.issuer);
   BOOST_CHECK_EQUAL(evidence.subject(), "alice");
   BOOST_CHECK(evidence.valid_at(std::chrono::system_clock::now()));
   BOOST_CHECK(evidence.valid_until() <= evidence.expires_at());
   BOOST_CHECK(evidence.valid_until() - evidence.verified_at() <= options.policy.freshness);
   auto wrong = options.policy;
   wrong.version = "v2";
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign(), wrong), oauth2::exceptions::unsupported_request);
}

BOOST_AUTO_TEST_CASE(purpose_algorithm_and_signature_denials_never_fallback_to_introspection) {
   auto f = fixture{};
   const auto options = f.jwt_options();
   const auto instance = f.create(options);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign({}, "JWT"), options.policy), oauth2::exceptions::invalid_token);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign({}, "at+jwt", "RS512"), options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign({}, "at+jwt", "RS256", f.second.get()), options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
}

BOOST_AUTO_TEST_CASE(strict_claim_types_resources_and_numericdate_overflow_are_denied) {
   auto f = fixture{};
   const auto options = f.jwt_options();
   const auto instance = f.create(options);
   for (const auto* field : {"iss", "sub", "aud", "exp", "nbf", "iat", "client_id", "jti"}) {
      BOOST_CHECK_THROW((void)f.verify(instance, f.sign([field](json_t* claims) {
         require(json_object_set_new(claims, field, json_boolean(1)) == 0);
      }),
                                       options.policy),
                        oauth2::exceptions::invalid_token);
   }
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign([](json_t* claims) {
      require(json_object_set_new(claims, "exp", json_integer(std::numeric_limits<json_int_t>::max())) == 0);
   }),
                                    options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign([](json_t* claims) {
      require(json_object_set_new(claims, "exp", json_integer(1)) == 0);
   }),
                                    options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign([](json_t* claims) {
      require(json_object_set_new(claims, "aud", json_string("another-resource")) == 0);
   }),
                                    options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign([](json_t* claims) {
      require(json_object_set_new(claims, "scope", json_string("write")) == 0);
   }),
                                    options.policy),
                     oauth2::exceptions::policy_denied);
}

BOOST_AUTO_TEST_CASE(duplicate_claims_and_nul_suffixed_signed_payloads_are_rejected_without_a_second_parser) {
   auto f = fixture{};
   const auto options = f.jwt_options();
   const auto instance = f.create(options);
   auto repeated = encode(f.payload().get());
   repeated.pop_back();
   repeated += ",\"sub\":\"bob\"}";
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign({}, "at+jwt", "RS256", nullptr, repeated), options.policy),
                     oauth2::exceptions::invalid_token);
   auto nul = encode(f.payload().get());
   nul.append("\0ignored", 8);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign({}, "at+jwt", "RS256", nullptr, nul), options.policy),
                     oauth2::exceptions::invalid_token);
}

BOOST_AUTO_TEST_CASE(verification_result_cache_is_unconditionally_disabled_including_literal_jwks_url_tokens) {
   auto f = fixture{};
   const auto options = f.jwt_options();
   const auto instance = f.create(options);
   (void)f.verify(instance, f.sign(), options.policy);
   BOOST_CHECK_THROW((void)f.verify(instance, secret{f.origin + "/keys"}, options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_THROW((void)f.verify(instance, f.sign([](json_t* claims) {
      require(json_object_set_new(claims, "exp", json_integer(1)) == 0);
   }),
                                    options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
}

BOOST_AUTO_TEST_CASE(provider_domains_do_not_share_keys_even_at_same_endpoint_and_same_kid) {
   auto f = fixture{};
   auto options = f.jwt_options();
   const auto first = f.create(options);
   options.policy.version = "v2";
   const auto second = f.create(options);
   (void)f.verify(first, f.sign(), f.jwt_options().policy);
   {
      const auto lock = std::scoped_lock{f.mutex};
      f.keys = "{\"keys\":[" + public_key(f.second.get()) + "]}";
   }
   (void)f.verify(second, f.sign({}, "at+jwt", "RS256", f.second.get()), options.policy);
   BOOST_CHECK_THROW((void)f.verify(second, f.sign(), options.policy), oauth2::exceptions::invalid_token);
   BOOST_CHECK_EQUAL(f.requests.load(), 2U);
}

BOOST_AUTO_TEST_CASE(key_count_and_response_size_bounds_fail_closed_with_size_errors) {
   auto f = fixture{};
   auto options = f.jwt_options();
   std::get<adapter::jwks>(options.source).max_keys = 1;
   const auto counted = f.create(options);
   auto small = f.jwt_options();
   small.max_response_bytes = 128;
   small.max_claim_bytes = 128;
   const auto sized = f.create(small);
   {
      const auto lock = std::scoped_lock{f.mutex};
      f.keys = "{\"keys\":[" + public_key(f.first.get()) + "," + public_key(f.second.get()) + "]}";
   }
   BOOST_CHECK_THROW((void)f.verify(counted, f.sign(), options.policy), oauth2::exceptions::response_too_large);
   BOOST_CHECK_THROW((void)f.verify(sized, f.sign(), small.policy), oauth2::exceptions::response_too_large);
}

BOOST_AUTO_TEST_CASE(fast_completion_and_ticket_release_race_settles_on_multithread_executor) {
   auto f = fixture{};
   const auto options = f.jwt_options();
   const auto instance = f.create(options);
   constexpr std::size_t rounds = 2048;
   constexpr std::size_t concurrent = 4;
   const auto stress_deadline = std::chrono::steady_clock::now() + 30s;
   for (std::size_t round = 0; round < rounds; ++round) {
      BOOST_TEST_CONTEXT("fast completion round " << round) {
         BOOST_REQUIRE_MESSAGE(std::chrono::steady_clock::now() < stress_deadline,
                               "fast completion stress exceeded its total budget");
         auto running = std::array<std::future<std::optional<oauth2::verified_token>>, concurrent>{};
         for (auto& result : running) {
            // Fail inside physical compute work, without HTTP or caller-side serialization.
            result = boost::asio::co_spawn(
                f.runtime.context(), own_result(instance->async_verify(secret{"not-a-jwt"}, options.policy)),
                boost::asio::use_future);
         }
         const auto deadline = std::min(stress_deadline,
                                        std::chrono::steady_clock::now() + options.timeout + 1s);
         auto ready = true;
         for (auto& result : running) {
            ready = (result.wait_until(deadline) == std::future_status::ready) && ready;
         }
         if (!ready) {
            // Wake a stranded stop wait so a RED result does not hang fixture teardown.
            instance->request_stop();
            const auto cleanup_deadline = std::chrono::steady_clock::now() + 2s;
            for (auto& result : running) {
               BOOST_CHECK_MESSAGE(result.wait_until(cleanup_deadline) == std::future_status::ready,
                                   "stop must settle a timed-out notification race");
            }
         }
         BOOST_REQUIRE_MESSAGE(ready, "fast physical completion exceeded its bounded notification wait");
         for (auto& result : running) {
            BOOST_CHECK_THROW((void)result.get(), oauth2::exceptions::invalid_token);
         }
      }
   }
   BOOST_CHECK_EQUAL(f.requests.load(), 0U);
   auto drain = boost::asio::co_spawn(f.runtime.context(), instance->async_drain(), boost::asio::use_future);
   BOOST_REQUIRE(drain.wait_for(2s) == std::future_status::ready);
   drain.get();
   BOOST_CHECK_EQUAL(f.pool.snapshot().pending, 0U);
}

BOOST_AUTO_TEST_CASE(introspection_rechecks_resource_claims_and_never_caches_opaque_decisions) {
   auto f = fixture{};
   const auto options = f.opaque_options();
   const auto instance = f.create(options);
   const auto evidence = f.verify(instance, secret{"opaque-canary-never-log"}, options.policy);
   BOOST_CHECK(evidence.profile() == oauth2::token_profile::opaque_introspection);
   {
      const auto lock = std::scoped_lock{f.mutex};
      f.introspected = "{\"active\":false}";
   }
   BOOST_CHECK_THROW((void)f.verify(instance, secret{"opaque-canary-never-log"}, options.policy),
                     oauth2::exceptions::invalid_token);
   BOOST_CHECK_EQUAL(f.requests.load(), 2U);
}

BOOST_AUTO_TEST_CASE(https_trust_redirects_and_outages_never_become_success_or_bad_credential) {
   auto f = fixture{};
   auto options = f.opaque_options();
   const auto instance = f.create(options);
   auto untrusted = options;
   untrusted.ca_bundle.clear();
   const auto no_ca = f.create(untrusted);
   const auto redirected = f.create(f.opaque_options("/redirect"));
   BOOST_CHECK_THROW((void)f.verify(no_ca, secret{"canary"}, untrusted.policy), oauth2::exceptions::issuer_unavailable);
   BOOST_CHECK_EQUAL(f.requests.load(), 0U);
   BOOST_CHECK_THROW((void)f.verify(redirected, secret{"canary"}, options.policy),
                     oauth2::exceptions::issuer_unavailable);
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
   for (const auto status : {401, 503}) {
      f.status.store(status);
      BOOST_CHECK_THROW((void)f.verify(instance, secret{"canary"}, options.policy),
                        oauth2::exceptions::issuer_unavailable);
   }
}

BOOST_AUTO_TEST_CASE(cancel_discards_late_result_backpressure_and_drain_track_physical_work) {
   auto f = fixture{};
   auto options = f.opaque_options();
   options.max_in_flight = 1;
   const auto instance = f.create(options);
   f.hold.store(true);
   const auto cancel = std::make_shared<boost::asio::cancellation_signal>();
   const auto executor = boost::asio::make_strand(f.runtime.context());
   const auto result = std::make_shared<std::promise<std::optional<oauth2::verified_token>>>();
   auto running = result->get_future();
   boost::asio::co_spawn(
       executor, own_result(instance->async_verify(secret{"canary"}, options.policy)),
       boost::asio::bind_cancellation_slot(
           cancel->slot(), [cancel, result](std::exception_ptr error, std::optional<oauth2::verified_token> token) {
              (void)cancel; // Own the signal until the coroutine completion handler returns.
              if (error) {
                 result->set_exception(error);
              } else {
                 result->set_value(std::move(token));
              }
           }));
   const auto finish = [instance, &running](fixture* value) {
      value->release();
      instance->request_stop();
      if (running.valid()) {
         running.wait();
      }
   };
   const auto cleanup = std::unique_ptr<fixture, decltype(finish)>{&f, finish};
   forge::asio::blocking::run(f.runtime, f.await_request(1));
   BOOST_CHECK_THROW((void)f.verify(instance, secret{"other-canary"}, options.policy), oauth2::exceptions::busy);
   const auto delivery = std::make_shared<std::promise<void>>();
   auto delivered = delivery->get_future();
   boost::asio::post(executor, [cancel, delivery] {
      cancel->emit(boost::asio::cancellation_type::all);
      delivery->set_value();
   });
   BOOST_REQUIRE(delivered.wait_for(500ms) == std::future_status::ready);
   delivered.get();
   BOOST_REQUIRE(running.wait_for(500ms) == std::future_status::ready);
   BOOST_CHECK_THROW((void)running.get(), oauth2::exceptions::canceled);
   auto drain = boost::asio::co_spawn(f.runtime.context(), instance->async_drain(), boost::asio::use_future);
   BOOST_CHECK(drain.wait_for(20ms) == std::future_status::timeout);
   f.release();
   BOOST_REQUIRE(drain.wait_for(2s) == std::future_status::ready);
   drain.get();
   BOOST_CHECK_THROW((void)f.verify(instance, secret{"canary"}, options.policy), oauth2::exceptions::stopped);
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
}

BOOST_AUTO_TEST_CASE(stop_discards_running_result_without_shortening_physical_drain) {
   auto f = fixture{};
   const auto options = f.opaque_options();
   const auto instance = f.create(options);
   f.hold.store(true);
   auto running =
       boost::asio::co_spawn(f.runtime.context(), own_result(instance->async_verify(secret{"canary"}, options.policy)),
                             boost::asio::use_future);
   const auto finish = [instance, &running](fixture* value) {
      value->release();
      instance->request_stop();
      if (running.valid()) {
         running.wait();
      }
   };
   const auto cleanup = std::unique_ptr<fixture, decltype(finish)>{&f, finish};
   forge::asio::blocking::run(f.runtime, f.await_request(1));
   instance->request_stop();
   BOOST_REQUIRE(running.wait_for(500ms) == std::future_status::ready);
   BOOST_CHECK_THROW((void)running.get(), oauth2::exceptions::stopped);
   auto drain = boost::asio::co_spawn(f.runtime.context(), instance->async_drain(), boost::asio::use_future);
   BOOST_CHECK(drain.wait_for(20ms) == std::future_status::timeout);
   f.release();
   BOOST_REQUIRE(drain.wait_for(2s) == std::future_status::ready);
   drain.get();
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
}

BOOST_AUTO_TEST_CASE(shared_provider_refresh_wait_is_bounded_and_coalesces_cloned_calls) {
   auto f = fixture{};
   const auto options = f.jwt_options();
   const auto instance = f.create(options);
   f.hold.store(true);
   auto first = boost::asio::co_spawn(f.runtime.context(), own_result(instance->async_verify(f.sign(), options.policy)),
                                      boost::asio::use_future);
   forge::asio::blocking::run(f.runtime, f.await_request(1));
   auto second = boost::asio::co_spawn(
       f.runtime.context(), own_result(instance->async_verify(f.sign(), options.policy)), boost::asio::use_future);
   BOOST_REQUIRE(second.wait_for(500ms) == std::future_status::ready);
   BOOST_CHECK_THROW((void)second.get(), oauth2::exceptions::issuer_unavailable);
   BOOST_CHECK(first.wait_for(0ms) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
   f.release();
   BOOST_REQUIRE(first.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK(first.get().has_value());
   (void)f.verify(instance, f.sign(), options.policy);
   BOOST_CHECK_EQUAL(f.requests.load(), 1U);
}

BOOST_AUTO_TEST_CASE(secret_material_never_enters_own_error_messages) {
   auto f = fixture{};
   const auto options = f.opaque_options();
   const auto instance = f.create(options);
   f.status.store(503);
   auto caught = false;
   const auto bearer = std::string{"canary-bearer-do-not-log"};
   const auto server_secret = std::string{"fixture-server-secret-never-log"};
   try {
      (void)f.verify(instance, secret{bearer}, options.policy);
   } catch (const oauth2::exceptions::issuer_unavailable& error) {
      caught = true;
      const auto message = std::string{error.what()};
      BOOST_CHECK(message.find(bearer) == std::string::npos);
      BOOST_CHECK(message.find(server_secret) == std::string::npos);
   }
   BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(oversized_inputs_never_enter_the_compute_or_http_lane) {
   auto f = fixture{};
   auto options = f.opaque_options();
   options.max_token_bytes = 8;
   const auto instance = f.create(options);
   BOOST_CHECK_THROW((void)f.verify(instance, secret{"ninebytes"}, options.policy),
                     oauth2::exceptions::input_too_large);
   BOOST_CHECK_EQUAL(f.pool.snapshot().submitted, 0U);
   BOOST_CHECK_EQUAL(f.requests.load(), 0U);
}

BOOST_AUTO_TEST_CASE(queue_deadline_cancels_unstarted_work_and_drain_does_not_stop_the_shared_pool) {
   auto f = fixture{};
   auto options = f.opaque_options();
   options.timeout = 1200ms;
   const auto instance = f.create(options);
   struct barrier {
      std::latch entered{2};
      std::counting_semaphore<2> released{0};
   };
   const auto blocked = std::make_shared<barrier>();
   const auto release = [](barrier* value) { value->released.release(2); };
   const auto cleanup = std::unique_ptr<barrier, decltype(release)>{blocked.get(), release};
   const auto block = [blocked] {
      blocked->entered.count_down();
      blocked->released.acquire();
   };
   auto first = f.pool.get_executor().try_submit({.name = "external fixture work"}, block);
   auto second = f.pool.get_executor().try_submit({.name = "external fixture work"}, block);
   BOOST_REQUIRE(first && second);
   blocked->entered.wait();
   auto queued =
       boost::asio::co_spawn(f.runtime.context(), own_result(instance->async_verify(secret{"canary"}, options.policy)),
                             boost::asio::use_future);
   BOOST_REQUIRE(queued.wait_for(3s) == std::future_status::ready);
   BOOST_CHECK_THROW((void)queued.get(), oauth2::exceptions::issuer_unavailable);
   BOOST_CHECK_EQUAL(f.pool.snapshot().pending, 0U);
   BOOST_CHECK_EQUAL(f.pool.snapshot().running, 2U);
   auto drained = boost::asio::co_spawn(f.runtime.context(), instance->async_drain(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(500ms) == std::future_status::ready);
   drained.get();
   BOOST_CHECK(!f.pool.snapshot().stopped);
   BOOST_CHECK_EQUAL(f.requests.load(), 0U);
}

BOOST_AUTO_TEST_CASE(introspection_token_type_is_case_insensitive_but_active_does_not_replace_resource_policy) {
   auto f = fixture{};
   const auto options = f.opaque_options();
   const auto instance = f.create(options);
   auto claims = f.payload();
   require(json_object_set_new(claims.get(), "active", json_true()) == 0 &&
           json_object_set_new(claims.get(), "token_type", json_string("bearer")) == 0);
   {
      const auto lock = std::scoped_lock{f.mutex};
      f.introspected = encode(claims.get());
   }
   (void)f.verify(instance, secret{"opaque"}, options.policy);
   require(json_object_del(claims.get(), "exp") == 0);
   {
      const auto lock = std::scoped_lock{f.mutex};
      f.introspected = encode(claims.get());
   }
   BOOST_CHECK_THROW((void)f.verify(instance, secret{"opaque"}, options.policy), oauth2::exceptions::invalid_token);
}

BOOST_AUTO_TEST_SUITE_END()
