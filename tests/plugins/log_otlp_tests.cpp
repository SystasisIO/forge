#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/log/macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

import forge.api.core.registry;
import forge.app.application_shell;
import forge.app.diagnostics;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.crypto.core.types;
import forge.net.http.route_context;
import forge.net.http.server;
import forge.net.http.types;
import forge.log.logger;
import forge.log.logger_config;
import forge.log.record;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.types;
import forge.plugins.log.otlp.api;
import forge.plugins.log.otlp.exceptions;
import forge.plugins.log.otlp.plugin;
import forge.plugins.log.otlp.types;
import forge.variant.value;

namespace {

using namespace std::chrono_literals;
namespace log_otlp = forge::plugins::log::otlp;
namespace crypto_secrets = forge::plugins::crypto::secrets;

struct collected_request {
   std::string target;
   std::string content_type;
   std::string authorization;
   std::string body;
};

class fake_collector {
 public:
   explicit fake_collector(forge::asio::runtime& runtime)
       : server_(runtime, {.bind_address = "127.0.0.1", .port = 0},
                 [this](forge::net::http::route_context& context) { return handle(context); }) {
      server_.start();
      for (auto attempt = 0; attempt != 100; ++attempt) {
         if (server_.port() != 0) {
            return;
         }
         std::this_thread::sleep_for(10ms);
      }
      BOOST_FAIL("fake OTLP logs collector did not bind a port");
   }

   ~fake_collector() {
      server_.stop();
      std::this_thread::sleep_for(20ms);
   }

   [[nodiscard]] std::string endpoint() const {
      return "http://127.0.0.1:" + std::to_string(server_.port());
   }

   [[nodiscard]] std::vector<collected_request> requests() const {
      const auto lock = std::scoped_lock{mutex_};
      return requests_;
   }

   [[nodiscard]] bool wait_for_requests(std::size_t count, std::chrono::milliseconds timeout = 2s) const {
      auto lock = std::unique_lock{mutex_};
      return ready_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
   }

 private:
   boost::asio::awaitable<forge::net::http::response> handle(forge::net::http::route_context& context) {
      auto request = collected_request{
          .target = std::string{context.request.target()},
          .body = context.request.body(),
      };
      if (const auto header = context.request.find(forge::net::http::field::content_type);
          header != context.request.end()) {
         request.content_type = std::string{header->value()};
      }
      if (const auto header = context.request.find("Authorization"); header != context.request.end()) {
         request.authorization = std::string{header->value()};
      }
      {
         const auto lock = std::scoped_lock{mutex_};
         requests_.push_back(std::move(request));
      }
      ready_.notify_all();

      auto response = forge::net::http::response{forge::net::http::status::ok, context.request.version()};
      response.set(forge::net::http::field::content_type, "application/json");
      response.body() = "{}";
      response.prepare_payload();
      co_return response;
   }

   forge::net::http::server server_;
   mutable std::mutex mutex_;
   mutable std::condition_variable ready_;
   std::vector<collected_request> requests_;
};

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      records.push_back(record);
   }

   std::vector<forge::log_record> records;
};

[[nodiscard]] forge::config::core::value logger_route(std::string name, std::string level = "info", bool enabled = true,
                                                      bool export_logs = true) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("name", forge::config::core::value{std::move(name)});
   object.emplace("level", forge::config::core::value{std::move(level)});
   object.emplace("enabled", forge::config::core::value{enabled});
   object.emplace("export", forge::config::core::value{export_logs});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value secret_header(std::string name, std::string secret_id, std::string purpose) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("name", forge::config::core::value{std::move(name)});
   object.emplace("secret-id", forge::config::core::value{std::move(secret_id)});
   object.emplace("purpose", forge::config::core::value{std::move(purpose)});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value literal_header(std::string name, std::string value) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("name", forge::config::core::value{std::move(name)});
   object.emplace("value", forge::config::core::value{std::move(value)});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::document plugin_config(const std::string& endpoint,
                                                          forge::config::core::value::array_type loggers) {
   auto document = forge::config::core::document{};
   document.set("plugins.log.otlp.endpoint", endpoint);
   document.set("plugins.log.otlp.logs-path", std::string{"/v1/logs"});
   document.set("plugins.log.otlp.protocol", std::string{"http-json"});
   document.set("plugins.log.otlp.loggers", forge::config::core::value{std::move(loggers)});
   document.set("plugins.log.otlp.batch.flush-interval-ms", std::uint64_t{60000});
   document.set("plugins.log.otlp.retry.max-attempts", std::uint64_t{0});
   return document;
}

struct plugin_harness {
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler;
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events;
   forge::app::diagnostics_store diagnostics;
   log_otlp::plugin plugin;

   plugin_harness() : runtime{}, scheduler{runtime} {}

   void configure(const forge::config::core::document& document) {
      forge::asio::blocking::run(runtime,
                                 plugin.configure(forge::config::core::component_view{document, "plugins.log.otlp"}));
   }

   void install_secrets(std::shared_ptr<crypto_secrets::api> api) {
      auto provider = forge::api::core::installer{apis};
      provider.install<crypto_secrets::api>(std::move(api));
   }

   void provide_and_start() {
      auto provider = forge::api::core::installer{apis};
      forge::asio::blocking::run(runtime, plugin.provide(provider));
      auto context = forge::app::plugin_context{scheduler, apis, signals, events, &diagnostics};
      forge::asio::blocking::run(runtime, plugin.initialize(context));
      forge::asio::blocking::run(runtime, plugin.startup());
   }

   void shutdown() {
      forge::asio::blocking::run(runtime, plugin.shutdown());
   }
};

class shell_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(log_otlp::descriptor());
   }
};

class test_secrets_api final : public crypto_secrets::api {
 public:
   explicit test_secrets_api(bool deny = false, std::string value = "Bearer sensitive")
       : deny_{deny}, value_{std::move(value)} {}

   boost::asio::awaitable<crypto_secrets::snapshot> status(crypto_secrets::query) override {
      co_return crypto_secrets::snapshot{.configured_secrets = 1};
   }

   boost::asio::awaitable<crypto_secrets::get_result> get_bytes(crypto_secrets::get_request request) override {
      if (deny_ || request.secret_id != "otlp/cloud-token" || request.purpose != "otlp.logs.authorization") {
         throw crypto_secrets::exceptions::purpose_denied{"secret request denied"};
      }
      auto bytes = forge::crypto::core::bytes{value_.begin(), value_.end()};
      co_return crypto_secrets::get_result{.secret_id = std::move(request.secret_id), .bytes = std::move(bytes)};
   }

   boost::asio::awaitable<crypto_secrets::derive_result> derive_hkdf_sha256(crypto_secrets::derive_request) override {
      throw std::logic_error{"not implemented"};
      co_return crypto_secrets::derive_result{};
   }

   boost::asio::awaitable<crypto_secrets::aead_encrypt_result>
   encrypt_aes_gcm(crypto_secrets::aead_encrypt_request) override {
      throw std::logic_error{"not implemented"};
      co_return crypto_secrets::aead_encrypt_result{};
   }

   boost::asio::awaitable<crypto_secrets::aead_decrypt_result>
   decrypt_aes_gcm(crypto_secrets::aead_decrypt_request) override {
      throw std::logic_error{"not implemented"};
      co_return crypto_secrets::aead_decrypt_result{};
   }

 private:
   bool deny_ = false;
   std::string value_;
};

void expect_contains(std::string_view haystack, std::string_view needle) {
   BOOST_TEST_CONTEXT("needle: " << needle) {
      BOOST_TEST(haystack.find(needle) != std::string_view::npos);
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(log_otlp_plugin_test_suite)

BOOST_AUTO_TEST_CASE(log_otlp_descriptor_api_and_config_are_nested) {
   auto plugin = log_otlp::plugin{};
   BOOST_TEST(plugin.id().value == "forge.plugins.log.otlp");
   BOOST_TEST(log_otlp::api::ref().id.value == "forge.plugins.log.otlp");

   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.log.otlp");
   const auto has = [&](std::string_view field) {
      return std::ranges::any_of(descriptor->fields, [&](const auto& value) { return value.name == field; });
   };
   BOOST_TEST(has("endpoint"));
   BOOST_TEST(has("export-enabled"));
   BOOST_TEST(!has("enabled"));
   BOOST_TEST(has("loggers"));
   BOOST_TEST(has("queue"));
   BOOST_TEST(has("batch"));
   BOOST_TEST(has("retry"));
   BOOST_TEST(has("crash-spool"));
}

BOOST_AUTO_TEST_CASE(log_otlp_export_disabled_does_not_export_and_api_is_unavailable) {
   auto harness = plugin_harness{};
   auto document = forge::config::core::document{};
   document.set("plugins.log.otlp.export-enabled", false);
   harness.configure(document);
   harness.provide_and_start();

   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, api->metrics()),
                     log_otlp::exceptions::exporter_unavailable);
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, api->flush()),
                     log_otlp::exceptions::exporter_unavailable);

   harness.shutdown();
}

BOOST_AUTO_TEST_CASE(log_otlp_export_disabled_keeps_named_routes_on_the_console_parent_without_an_exporter) {
   forge::configure_logging(forge::logging_config::default_config());
   auto shared_sink = std::make_shared<capture_sink>();
   auto console_parent = forge::logger::get("default");
   console_parent.add_sink(shared_sink);
   forge::logger::update("default", console_parent);

   auto harness = plugin_harness{};
   auto document = plugin_config("http://127.0.0.1:4318", {logger_route("spine.runtime", "debug")});
   document.set("plugins.log.otlp.export-enabled", false);
   harness.configure(document);
   harness.provide_and_start();

   auto named = forge::logger::get("spine.runtime");
   BOOST_REQUIRE(named.get_parent() != nullptr);
   named.debug("named route remains visible without OTLP");

   BOOST_REQUIRE_EQUAL(shared_sink->records.size(), 1U);
   BOOST_TEST(shared_sink->records.front().logger == "spine.runtime");
   BOOST_TEST(shared_sink->records.front().message == "named route remains visible without OTLP");

   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, api->metrics()),
                     log_otlp::exceptions::exporter_unavailable);

   harness.shutdown();
}

BOOST_AUTO_TEST_CASE(log_otlp_application_shell_separates_lifecycle_and_export_switches) {
   auto application = shell_application{};
   const auto registry = application.describe_config();
   const auto has_field = [&](std::string_view section, std::string_view field) {
      return std::ranges::any_of(registry.components(), [&](const auto& component) {
         return component.section == section &&
                std::ranges::any_of(component.fields, [&](const auto& value) { return value.name == field; });
      });
   };

   BOOST_TEST(has_field("plugins", "log.otlp.enabled"));
   BOOST_TEST(has_field("plugins.log.otlp", "export-enabled"));
   BOOST_TEST(!has_field("plugins.log.otlp", "enabled"));

   auto document = forge::config::core::document{};
   document.set("plugins.log.otlp.enabled", true);
   document.set("plugins.log.otlp.export-enabled", false);
   application.configure(document);
   forge::asio::blocking::run(application.runtime(), application.startup());

   auto api = application.apis().get<log_otlp::api>(log_otlp::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(application.runtime(), api->metrics()),
                     log_otlp::exceptions::exporter_unavailable);

   forge::asio::blocking::run(application.runtime(), application.shutdown());
}

BOOST_AUTO_TEST_CASE(log_otlp_exports_default_and_named_logger_routes) {
   forge::configure_logging(forge::logging_config{});

   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   harness.configure(
       plugin_config(collector.endpoint(), {logger_route("default"), logger_route("plugin.dynamic", "debug")}));
   harness.provide_and_start();

   ilog("default route exported ${value}", ("value", "one"));
   auto named = forge::logger::get("plugin.dynamic");
   forge_ilog(named, "named route exported ${value}", ("value", "two"));

   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   forge::asio::blocking::run(harness.runtime, api->flush());

   BOOST_REQUIRE(collector.wait_for_requests(1));
   const auto requests = collector.requests();
   BOOST_REQUIRE(!requests.empty());
   const auto body =
       std::accumulate(requests.begin(), requests.end(), std::string{}, [](std::string out, const auto& request) {
          out += request.body;
          return out;
       });
   expect_contains(body, "default route exported one");
   expect_contains(body, "named route exported two");
   expect_contains(body, "\"logger\"");
   expect_contains(body, "default");
   expect_contains(body, "plugin.dynamic");

   const auto snapshot = forge::asio::blocking::run(harness.runtime, api->metrics());
   BOOST_TEST(snapshot.enqueued_records == 2U);
   BOOST_TEST(snapshot.exported_records == 2U);

   harness.shutdown();
}

BOOST_AUTO_TEST_CASE(log_otlp_resolves_secret_headers_at_startup_without_config_material) {
   forge::configure_logging(forge::logging_config{});

   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   auto document = plugin_config(collector.endpoint(), {logger_route("default")});
   document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{secret_header(
                                                "Authorization", "otlp/cloud-token", "otlp.logs.authorization")});
   harness.install_secrets(std::make_shared<test_secrets_api>());
   harness.configure(document);
   harness.provide_and_start();

   ilog("secret header export");
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   forge::asio::blocking::run(harness.runtime, api->flush());

   BOOST_REQUIRE(collector.wait_for_requests(1));
   const auto requests = collector.requests();
   BOOST_REQUIRE(!requests.empty());
   BOOST_TEST(requests.front().authorization == "Bearer sensitive");

   harness.shutdown();
}

BOOST_AUTO_TEST_CASE(log_otlp_keeps_literal_headers_compatible) {
   forge::configure_logging(forge::logging_config{});

   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   auto document = plugin_config(collector.endpoint(), {logger_route("default")});
   document.set("plugins.log.otlp.headers",
                forge::config::core::value::array_type{literal_header("Authorization", "Bearer literal")});
   harness.configure(document);
   harness.provide_and_start();

   ilog("literal header export");
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   forge::asio::blocking::run(harness.runtime, api->flush());

   BOOST_REQUIRE(collector.wait_for_requests(1));
   const auto requests = collector.requests();
   BOOST_REQUIRE(!requests.empty());
   BOOST_TEST(requests.front().authorization == "Bearer literal");

   harness.shutdown();
}

BOOST_AUTO_TEST_CASE(log_otlp_redacts_secret_material_when_resolution_is_denied) {
   auto harness = plugin_harness{};
   auto document = plugin_config("http://localhost:4318", {logger_route("default")});
   document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{secret_header(
                                                "Authorization", "otlp/cloud-token", "otlp.logs.authorization")});
   harness.install_secrets(std::make_shared<test_secrets_api>(true));
   harness.configure(document);

   try {
      harness.provide_and_start();
      BOOST_FAIL("denied secret resolution must fail startup");
   } catch (const log_otlp::exceptions::startup_failed& error) {
      const auto message = std::string{error.what()};
      BOOST_TEST(message.find("otlp/cloud-token") != std::string::npos);
      BOOST_TEST(message.find("sensitive") == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(log_otlp_rejects_unsafe_secret_header_bytes_at_startup) {
   auto harness = plugin_harness{};
   auto document = plugin_config("http://localhost:4318", {logger_route("default")});
   document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{secret_header(
                                                "Authorization", "otlp/cloud-token", "otlp.logs.authorization")});
   harness.install_secrets(std::make_shared<test_secrets_api>(false, "Bearer token\r\nInjected: true"));
   harness.configure(document);

   BOOST_CHECK_THROW(harness.provide_and_start(), log_otlp::exceptions::startup_failed);
}

BOOST_AUTO_TEST_CASE(log_otlp_preflights_secret_headers_with_http_provider_policy) {
   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{secret_header(
                                                   "Host", "otlp/cloud-token", "otlp.logs.authorization")});
      harness.install_secrets(std::make_shared<test_secrets_api>());
      harness.configure(document);
      BOOST_CHECK_THROW(harness.provide_and_start(), log_otlp::exceptions::startup_failed);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      document.set("plugins.log.otlp.headers",
                   forge::config::core::value::array_type{
                       secret_header("X-Cloud-Token-One", "otlp/cloud-token", "otlp.logs.authorization"),
                       secret_header("X-Cloud-Token-Two", "otlp/cloud-token", "otlp.logs.authorization")});
      harness.install_secrets(std::make_shared<test_secrets_api>(false, std::string(5U * 1024U, 'x')));
      harness.configure(document);
      BOOST_CHECK_THROW(harness.provide_and_start(), log_otlp::exceptions::startup_failed);
   }
}

BOOST_AUTO_TEST_CASE(log_otlp_rejects_missing_or_ambiguous_header_sources) {
   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{secret_header(
                                                   "Authorization", "otlp/cloud-token", "otlp.logs.authorization")});
      harness.configure(document);
      BOOST_CHECK_THROW(harness.provide_and_start(), log_otlp::exceptions::startup_failed);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      auto header = literal_header("Authorization", "literal");
      auto& object = std::get<forge::config::core::value::object_type>(header.storage);
      object.emplace("secret-id", forge::config::core::value{"otlp/cloud-token"});
      object.emplace("purpose", forge::config::core::value{"otlp.logs.authorization"});
      document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{std::move(header)});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      document.set("plugins.log.otlp.headers",
                   forge::config::core::value::array_type{forge::config::core::value{
                       forge::config::core::value::object_type{{"name", forge::config::core::value{"X-Empty"}}}}});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      auto header = secret_header("Authorization", "otlp/cloud-token", "otlp.logs.authorization");
      auto& object = std::get<forge::config::core::value::object_type>(header.storage);
      object.emplace("value", forge::config::core::value{std::string{}});
      document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{std::move(header)});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("default")});
      document.set("plugins.log.otlp.headers", forge::config::core::value::array_type{
                                                   forge::config::core::value{forge::config::core::value::object_type{
                                                       {"name", forge::config::core::value{"Authorization"}},
                                                       {"secret-id", forge::config::core::value{"otlp/cloud-token"}},
                                                   }}});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(log_otlp_rejects_invalid_config_through_schema_and_domain_validation) {
   {
      auto harness = plugin_harness{};
      auto document = forge::config::core::document{};
      document.set("plugins.log.otlp.protocol", std::string{"grpc"});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }

   {
      auto harness = plugin_harness{};
      auto document = forge::config::core::document{};
      document.set("plugins.log.otlp.endpoint", std::string{"not a url"});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("dup"), logger_route("dup")});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }

   {
      auto harness = plugin_harness{};
      auto document = plugin_config("http://localhost:4318", {logger_route("bad\nname")});
      BOOST_CHECK_THROW(harness.configure(document), log_otlp::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_SUITE_END()
