#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/log/macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
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

#include "../otlp/controlled_collector.hxx"

namespace otlp_log_fixtures {
struct credentials {
   std::string user;
   std::string token;
};
BOOST_DESCRIBE_STRUCT(credentials, (), (user, token))
} // namespace otlp_log_fixtures

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
import forge.codec.json;
import forge.crypto.core.types;
import forge.db.core.driver;
import forge.db.core.record;
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
import forge.plugins.db.store.api;
import forge.plugins.db.store.plugin;
import forge.variant.value;
import forge.schema.object;

template <> struct forge::schema::rules<otlp_log_fixtures::credentials> {
   static forge::schema::object_schema<otlp_log_fixtures::credentials> define() {
      auto schema = forge::schema::object<otlp_log_fixtures::credentials>();
      static_cast<void>(schema.field<&otlp_log_fixtures::credentials::user>("user-name"));
      schema.field<&otlp_log_fixtures::credentials::token>("token").secret();
      return schema;
   }
};

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

std::size_t exported_event_count(const std::vector<collected_request>& requests,
                                 std::string_view logger, std::string_view event) {
   auto count = std::size_t{};
   for (const auto& request : requests) {
      const auto parsed = forge::codec::json::read_value(request.body);
      BOOST_REQUIRE(parsed.ok());
      for (const auto& resource : parsed.value.get_object()["resourceLogs"].get_array()) {
         for (const auto& scope : resource.get_object()["scopeLogs"].get_array()) {
            for (const auto& record : scope.get_object()["logRecords"].get_array()) {
               auto matching_logger = false;
               auto matching_event = false;
               for (const auto& attribute : record.get_object()["attributes"].get_array()) {
                  const auto& item = attribute.get_object();
                  const auto key = item["key"].as_string();
                  if (key == "logger") {
                     matching_logger = item["value"].get_object()["stringValue"].as_string() == logger;
                  } else if (key == "event") {
                     matching_event = item["value"].get_object()["stringValue"].as_string() == event;
                  }
               }
               count += matching_logger && matching_event;
            }
         }
      }
   }
   return count;
}

class fake_collector {
 public:
   explicit fake_collector(forge::asio::runtime& runtime, bool unavailable = false)
       : server_(runtime, {.bind_address = "127.0.0.1", .port = 0},
                 [this](forge::net::http::route_context& context) { return handle(context); }),
         unavailable_{unavailable} {
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

      auto response = forge::net::http::response{
          unavailable_ ? forge::net::http::status::service_unavailable : forge::net::http::status::ok,
          context.request.version()};
      response.set(forge::net::http::field::content_type, "application/json");
      response.body() = "{}";
      response.prepare_payload();
      co_return response;
   }

   forge::net::http::server server_;
   bool unavailable_;
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

   void provide_and_initialize() {
      auto provider = forge::api::core::installer{apis};
      forge::asio::blocking::run(runtime, plugin.provide(provider));
      auto context = forge::app::plugin_context{scheduler, apis, signals, events, &diagnostics};
      forge::asio::blocking::run(runtime, plugin.initialize(context));
   }

   void provide_and_start() {
      provide_and_initialize();
      forge::asio::blocking::run(runtime, plugin.after_initialize());
      forge::asio::blocking::run(runtime, plugin.startup());
   }

   void shutdown() {
      forge::asio::blocking::run(runtime, plugin.shutdown());
   }
};

class shell_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(log_otlp::plugin::descriptor());
   }
};

class db_shell_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(log_otlp::plugin::descriptor());
      registry.register_plugin(forge::plugins::db::store::plugin::descriptor());
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

boost::asio::awaitable<bool> observe_management_closed(std::shared_ptr<log_otlp::api> api) {
   const auto executor = co_await boost::asio::this_coro::executor;
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   while (std::chrono::steady_clock::now() < deadline) {
      try {
         static_cast<void>(co_await api->metrics(log_otlp::metrics_request{}));
      } catch (const log_otlp::exceptions::exporter_unavailable&) {
         co_return true;
      }
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
   }
   co_return false;
}

boost::asio::awaitable<void> sample_management_metrics(std::shared_ptr<log_otlp::api> api) {
   const auto executor = co_await boost::asio::this_coro::executor;
   for (auto count = 0; count != 128; ++count) {
      try {
         static_cast<void>(co_await api->metrics(log_otlp::metrics_request{}));
      } catch (const log_otlp::exceptions::exporter_unavailable&) {
         co_return;
      }
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
   }
}

template <typename T> void require_concurrent_ready(std::future<T>& future) {
   BOOST_REQUIRE(future.wait_for(3s) == std::future_status::ready);
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
   dlog(named, "named route remains visible without OTLP");

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

   ilog("default", "default route exported ${value}", ("value", "one"));
   auto named = forge::logger::get("plugin.dynamic");
   ilog(named, "named route exported ${value}", ("value", "two"));

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

BOOST_AUTO_TEST_CASE(log_otlp_captures_after_initialize_and_stop_without_duplicate_attachment) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   harness.configure(plugin_config(collector.endpoint(), {logger_route("plugin.lifecycle")}));
   harness.provide_and_initialize();
   forge::asio::blocking::run(harness.runtime, harness.plugin.after_initialize());
   ilog("plugin.lifecycle", "fixture after_initialize");
   forge::asio::blocking::run(harness.runtime, harness.plugin.after_initialize());
   forge::asio::blocking::run(harness.runtime, harness.plugin.startup());
   harness.plugin.request_stop();
   ilog("plugin.lifecycle", "fixture producer shutdown");
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   forge::asio::blocking::run(harness.runtime, api->flush());
   const auto metrics = forge::asio::blocking::run(harness.runtime, api->metrics());
   BOOST_TEST(metrics.enqueued_records == 2U);
   BOOST_TEST(metrics.exported_records == 2U);
   harness.shutdown();
   ilog("plugin.lifecycle", "fixture detached");
   const auto requests = collector.requests();
   const auto body = std::accumulate(requests.begin(), requests.end(), std::string{},
                                     [](std::string out, const auto& request) { return out + request.body; });
   expect_contains(body, "fixture after_initialize");
   expect_contains(body, "fixture producer shutdown");
   BOOST_TEST(body.find("fixture detached") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(log_otlp_direct_startup_retains_compatibility_without_phase_barrier) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   harness.configure(plugin_config(collector.endpoint(), {logger_route("plugin.direct")}));
   harness.provide_and_initialize();
   forge::asio::blocking::run(harness.runtime, harness.plugin.startup());
   forge::asio::blocking::run(harness.runtime, harness.plugin.startup());
   ilog("plugin.direct", "direct startup", ("event", "fixture.direct"));
   harness.shutdown();
   BOOST_TEST(exported_event_count(collector.requests(), "plugin.direct", "fixture.direct") == 1U);
}

BOOST_AUTO_TEST_CASE(log_otlp_failure_after_attachment_detaches_and_can_restart) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   const auto path = std::filesystem::temp_directory_path() /
       ("forge-otlp-crash-path-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   auto cleanup = boost::scope::scope_exit{[&] { std::filesystem::remove_all(path); }};
   { auto file = std::ofstream{path}; file << "not a directory"; }
   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   auto document = plugin_config(collector.endpoint(), {logger_route("plugin.rollback")});
   document.set("plugins.log.otlp.crash-spool.enabled", true);
   document.set("plugins.log.otlp.crash-spool.directory", (path / "spool").string());
   harness.configure(document);
   harness.provide_and_initialize();
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, harness.plugin.after_initialize()),
                     log_otlp::exceptions::startup_failed);
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, api->metrics()),
                     log_otlp::exceptions::exporter_unavailable);
   ilog("plugin.rollback", "must stay local", ("event", "fixture.failed"));
   document.set("plugins.log.otlp.crash-spool.enabled", false);
   harness.configure(document);
   forge::asio::blocking::run(harness.runtime, harness.plugin.after_initialize());
   ilog("plugin.rollback", "restarted", ("event", "fixture.restarted"));
   harness.shutdown();
   BOOST_TEST(exported_event_count(collector.requests(), "plugin.rollback", "fixture.failed") == 0U);
   BOOST_TEST(exported_event_count(collector.requests(), "plugin.rollback", "fixture.restarted") == 1U);
}

#if defined(__unix__) || defined(__APPLE__)
BOOST_AUTO_TEST_CASE(log_otlp_resend_failure_restores_installed_crash_capture) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   const auto path = std::filesystem::temp_directory_path() /
       ("forge-otlp-resend-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   auto cleanup = boost::scope::scope_exit{[&] { std::filesystem::remove_all(path); }};
   std::filesystem::create_directories(path);
   std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                std::filesystem::perm_options::replace);
   const auto malformed = path / "crash-malformed.spool";
   { auto file = std::ofstream{malformed}; file << "invalid record"; }
   const auto blocked_quarantine = path / "crash-malformed.spool.bad";
   std::filesystem::create_directory(blocked_quarantine);
   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   auto document = plugin_config(collector.endpoint(), {logger_route("plugin.resend")});
   document.set("plugins.log.otlp.crash-spool.enabled", true);
   document.set("plugins.log.otlp.crash-spool.directory", path.string());
   harness.configure(document);
   harness.provide_and_initialize();
   const auto previous_terminate = std::get_terminate();
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, harness.plugin.after_initialize()),
                     log_otlp::exceptions::startup_failed);
   BOOST_CHECK(std::get_terminate() == previous_terminate);
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime, api->metrics()),
                     log_otlp::exceptions::exporter_unavailable);
   std::filesystem::remove(blocked_quarantine);
   forge::asio::blocking::run(harness.runtime, harness.plugin.after_initialize());
   ilog("plugin.resend", "restarted", ("event", "fixture.restarted"));
   harness.shutdown();
   BOOST_CHECK(std::get_terminate() == previous_terminate);
   BOOST_TEST(exported_event_count(collector.requests(), "plugin.resend", "fixture.restarted") == 1U);
}
#endif

BOOST_AUTO_TEST_CASE(log_otlp_application_exports_database_open_and_shutdown) {
   const auto backends = std::vector<std::string>{
#if FORGE_HAS_MDBX
       "mdbx",
#endif
#if FORGE_HAS_ROCKSDB
       "rocksdb",
#endif
   };
   for (const auto& backend : backends) {
      for (const auto unavailable : {false, true}) {
         BOOST_TEST_CONTEXT(backend) {
            BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
            auto application = db_shell_application{};
            auto collector = fake_collector{application.runtime(), unavailable};
            const auto path = std::filesystem::temp_directory_path() /
                              ("forge-otlp-db-" + backend + "-" +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            auto cleanup = boost::scope::scope_exit{[&] { std::filesystem::remove_all(path); }};
            auto document = plugin_config(
                collector.endpoint(), {logger_route("forge.db.store"), logger_route("forge.db." + backend, "debug")});
            using value = forge::config::core::value;
            document.set("plugins.log.otlp.enabled", true);
            document.set("plugins.db.store.enabled", true);
            document.set(
                "plugins.db.store.stores",
                value::array_type{value{value::object_type{{"name", value{"logging-fixture"}},
                                                           {"driver", value{backend}},
                                                           {"object", value{value::object_type{}}},
                                                           {"families", value{value::array_type{value{"audit"}}}},
                                                           {"path", value{path.string()}}}}});
            application.configure(document);
            forge::asio::blocking::run(application.runtime(), application.startup());
            auto stores = application.apis().get<forge::plugins::db::store::api>(forge::plugins::db::store::api::ref());
            auto store = forge::asio::blocking::run(application.runtime(), stores->store("logging-fixture"));
            auto transaction = forge::asio::blocking::run(application.runtime(), store.begin_transaction());
            const auto family = forge::db::core::family{"audit"};
            const auto key = forge::db::core::record_key{{std::byte{0x31}}};
            const auto payload = std::vector<std::byte>{std::byte{0x73}, std::byte{0x65}};
            forge::asio::blocking::run(application.runtime(), transaction.db_transaction().put(family, key, payload));
            forge::asio::blocking::run(application.runtime(), transaction.commit());
            auto telemetry = application.apis().get<log_otlp::api>(log_otlp::api::ref());
            forge::asio::blocking::run(application.runtime(), telemetry->flush());
            const auto metrics = forge::asio::blocking::run(application.runtime(), telemetry->metrics());
            if (unavailable) {
               BOOST_TEST(metrics.failed_records > 0U);
            }
            auto verify = forge::asio::blocking::run(application.runtime(), store.begin_transaction());
            const auto actual =
                forge::asio::blocking::run(application.runtime(), verify.db_transaction().get(family, key));
            BOOST_REQUIRE(actual.has_value());
            BOOST_CHECK(*actual == payload);
            forge::asio::blocking::run(application.runtime(), verify.rollback());
            forge::asio::blocking::run(application.runtime(), application.shutdown());
            BOOST_REQUIRE(collector.wait_for_requests(1));
            const auto requests = collector.requests();
            const auto body = std::accumulate(requests.begin(), requests.end(), std::string{},
                                              [](std::string out, const auto& request) { return out + request.body; });
            expect_contains(body, "forge.db." + backend);
            expect_contains(body, "forge.db.store");
            expect_contains(body, "plugin.started");
            expect_contains(body, "plugin.stopped");
            const auto driver_route = "forge.db." + backend;
            const auto opened = backend == "mdbx" ? "environment.opened" : "store.opened";
            const auto closed = backend == "mdbx" ? "environment.closed" : "store.closed";
            BOOST_TEST(exported_event_count(requests, driver_route, opened) == 1U);
            BOOST_TEST(exported_event_count(requests, driver_route, closed) == 1U);
            BOOST_TEST(body.find(path.string()) == std::string::npos);
         }
      }
   }
}

#if FORGE_HAS_MDBX
BOOST_AUTO_TEST_CASE(log_otlp_application_rolls_back_exporter_after_database_open_failure) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   auto application = db_shell_application{};
   auto collector = fake_collector{application.runtime()};
   const auto path = std::filesystem::temp_directory_path() /
       ("forge-otlp-db-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   auto cleanup = boost::scope::scope_exit{[&] { std::filesystem::remove_all(path); }};
   { auto file = std::ofstream{path}; file << "not a directory"; }
   auto document = plugin_config(collector.endpoint(), {logger_route("forge.db.store")});
   using value = forge::config::core::value;
   document.set("plugins.log.otlp.enabled", true);
   document.set("plugins.db.store.enabled", true);
   document.set("plugins.db.store.stores", value::array_type{value{value::object_type{
       {"name", value{"failure-fixture"}}, {"driver", value{"mdbx"}},
       {"object", value{value::object_type{}}}, {"path", value{(path / "db").string()}}}}});
   application.configure(document);
   BOOST_CHECK_THROW(forge::asio::blocking::run(application.runtime(), application.startup()), std::exception);
   auto telemetry = application.apis().get<log_otlp::api>(log_otlp::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(application.runtime(), telemetry->metrics()),
                     log_otlp::exceptions::exporter_unavailable);
   const auto requests = collector.requests();
   BOOST_TEST(exported_event_count(requests, "forge.db.store", "composition.failed") == 1U);
   for (const auto& request : requests) {
      BOOST_TEST(request.body.find(path.string()) == std::string::npos);
   }
}
#endif

BOOST_AUTO_TEST_CASE(log_otlp_macros_redact_nested_schema_values_before_interpolation_and_export_once) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   auto harness = plugin_harness{};
   auto collector = fake_collector{harness.runtime};
   harness.configure(plugin_config(collector.endpoint(), {logger_route("default"), logger_route("plugin.schema")}));
   harness.provide_and_start();

   auto capture = std::make_shared<capture_sink>();
   auto parent = forge::logger::get("default");
   auto named = forge::logger::get("plugin.schema");
   parent.add_sink(capture);
   named.add_sink(capture);
   const auto payload = std::vector<otlp_log_fixtures::credentials>{{"alice", "private-fixture-value"}};
   const auto line = __LINE__ + 1;
   ilog("plugin.schema", "nested ${payload}", ("payload", payload));
   BOOST_REQUIRE_EQUAL(capture->records.size(), 1U);
   const auto& record = capture->records.front();
   BOOST_TEST(record.location.line() == line);
   BOOST_TEST(record.logger == "plugin.schema");
   BOOST_TEST(record.message.find("private-fixture-value") == std::string::npos);
   BOOST_TEST(record.message.find("<redacted>") != std::string::npos);
   BOOST_REQUIRE_EQUAL(record.fields.size(), 1U);
   BOOST_TEST(record.fields.front().value.find("private-fixture-value") == std::string::npos);

   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref());
   forge::asio::blocking::run(harness.runtime, api->flush());
   BOOST_REQUIRE(collector.wait_for_requests(1));
   const auto requests = collector.requests();
   const auto body = std::accumulate(requests.begin(), requests.end(), std::string{},
                                     [](std::string out, const auto& request) { return out + request.body; });
   expect_contains(body, "nested");
   expect_contains(body, "alice");
   expect_contains(body, "user-name");
   expect_contains(body, "<redacted>");
   expect_contains(body, "plugin.schema");
   expect_contains(body, "source.line");
   expect_contains(body, "source.function");
   BOOST_TEST(body.find("private-fixture-value") == std::string::npos);
   const auto metrics = forge::asio::blocking::run(harness.runtime, api->metrics());
   BOOST_TEST(metrics.enqueued_records == 1U);
   BOOST_TEST(metrics.exported_records == 1U);
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

BOOST_AUTO_TEST_CASE(log_otlp_shutdown_joins_admitted_call_with_blocked_external_continuation) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   auto harness = plugin_harness{};
   auto caller = forge::asio::runtime{{.worker_threads = 1}};
   auto observer = forge::asio::runtime{{.worker_threads = 2}};
   auto caller_strand = boost::asio::make_strand(caller.context());
   auto collector = otlp_concurrency_fixtures::controlled_collector{harness.runtime};
   auto document = plugin_config(collector.endpoint(), {logger_route("plugin.join")});
   document.set("plugins.log.otlp.request-timeout-ms", std::uint64_t{10000});
   document.set("plugins.log.otlp.shutdown-timeout-ms", std::uint64_t{5000});
   harness.configure(document);
   harness.provide_and_start();
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref()).shared();
   // Constructing an awaitable is not admission and must not block shutdown.
   auto never_started = api->flush(log_otlp::flush_request{});
   ilog("plugin.join", "before stop", ("event", "fixture.before-stop"));
   auto flush = boost::asio::co_spawn(caller_strand, api->flush(log_otlp::flush_request{}),
                                      boost::asio::use_future);
   BOOST_REQUIRE(collector.wait_for_requests(1));

   auto entered = std::make_shared<std::promise<void>>();
   auto blocked = entered->get_future();
   auto unblock = std::make_shared<std::promise<void>>();
   auto gate = unblock->get_future().share();
   auto release_caller = boost::scope::scope_exit{[unblock] { unblock->set_value(); }};
   boost::asio::post(caller_strand, [entered, gate] {
      entered->set_value();
      gate.wait();
   });
   require_concurrent_ready(blocked);
   blocked.get();
   harness.plugin.request_stop();
   const auto before_shutdown = forge::asio::blocking::run(observer, api->metrics(log_otlp::metrics_request{}));
   BOOST_TEST(before_shutdown.enqueued_records == 1U);
   ilog("plugin.join", "after stop", ("event", "fixture.after-stop"));
   auto samples = boost::asio::co_spawn(observer.context(), sample_management_metrics(api), boost::asio::use_future);
   auto shutdown = boost::asio::co_spawn(harness.runtime.context(), harness.plugin.shutdown(), boost::asio::use_future);
   auto closed = boost::asio::co_spawn(observer.context(), observe_management_closed(api), boost::asio::use_future);
   require_concurrent_ready(closed);
   BOOST_REQUIRE(closed.get());
   collector.release();
   BOOST_REQUIRE(collector.wait_for_requests(2));
   // The transport is released, but flush cannot release its exporter on the
   // blocked caller strand. Shutdown must join that accepted call, not just I/O.
   BOOST_CHECK(shutdown.wait_for(100ms) == std::future_status::timeout);
   BOOST_CHECK(flush.wait_for(0ms) == std::future_status::timeout);
   unblock->set_value();
   release_caller.set_active(false);
   require_concurrent_ready(flush);
   require_concurrent_ready(shutdown);
   require_concurrent_ready(samples);
   static_cast<void>(flush.get());
   shutdown.get();
   samples.get();
   BOOST_CHECK_THROW(forge::asio::blocking::run(observer, std::move(never_started)),
                     log_otlp::exceptions::exporter_unavailable);
   BOOST_CHECK_THROW(forge::asio::blocking::run(observer, api->flush(log_otlp::flush_request{})),
                     log_otlp::exceptions::exporter_unavailable);
   BOOST_CHECK_THROW(forge::asio::blocking::run(observer, api->metrics(log_otlp::metrics_request{})),
                     log_otlp::exceptions::exporter_unavailable);
   const auto bodies = collector.bodies();
   const auto body = std::accumulate(bodies.begin(), bodies.end(), std::string{});
   expect_contains(body, "fixture.before-stop");
   expect_contains(body, "fixture.after-stop");
}

BOOST_AUTO_TEST_CASE(log_otlp_cancelled_management_call_releases_admission_and_preserves_sink) {
   BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
   auto harness = plugin_harness{};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto caller_strand = boost::asio::make_strand(caller.context());
   auto collector = otlp_concurrency_fixtures::controlled_collector{harness.runtime};
   harness.configure(plugin_config(collector.endpoint(), {logger_route("plugin.cancel")}));
   harness.provide_and_start();
   auto api = harness.apis.get<log_otlp::api>(log_otlp::api::ref()).shared();
   ilog("plugin.cancel", "before cancellation");
   auto signal = boost::asio::cancellation_signal{};
   auto cancelled = boost::asio::co_spawn(caller_strand, api->flush(log_otlp::flush_request{}),
       boost::asio::bind_cancellation_slot(signal.slot(), boost::asio::use_future));
   BOOST_REQUIRE(collector.wait_for_requests(1));
   boost::asio::post(caller_strand, [&] { signal.emit(boost::asio::cancellation_type::all); });
   require_concurrent_ready(cancelled);
   BOOST_CHECK_THROW(static_cast<void>(cancelled.get()), boost::system::system_error);
   harness.plugin.request_stop();
   ilog("plugin.cancel", "final producer log");
   auto surviving = boost::asio::co_spawn(caller.context(), api->flush(log_otlp::flush_request{}),
                                         boost::asio::use_future);
   collector.release();
   require_concurrent_ready(surviving);
   static_cast<void>(surviving.get());
   const auto metrics = forge::asio::blocking::run(caller, api->metrics(log_otlp::metrics_request{}));
   BOOST_TEST(metrics.exported_records == 2U);
   BOOST_TEST(metrics.failed_records == 0U);
   harness.shutdown();
}

BOOST_AUTO_TEST_CASE(log_otlp_held_api_and_delayed_call_are_safe_after_plugin_and_runtime_destruction) {
   auto held = std::shared_ptr<log_otlp::api>{};
   auto delayed = std::optional<boost::asio::awaitable<log_otlp::flush_result>>{};
   {
      BOOST_REQUIRE(forge::configure_logging(forge::logging_config{}));
      auto harness = plugin_harness{};
      auto collector = otlp_concurrency_fixtures::controlled_collector{harness.runtime};
      harness.configure(plugin_config(collector.endpoint(), {logger_route("plugin.held")}));
      harness.provide_and_start();
      held = harness.apis.get<log_otlp::api>(log_otlp::api::ref()).shared();
      delayed.emplace(held->flush(log_otlp::flush_request{}));
      harness.plugin.request_stop();
      // No pending log work, so an unstarted awaitable cannot delay shutdown.
      harness.shutdown();
   }
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   BOOST_CHECK_THROW(forge::asio::blocking::run(caller, std::move(*delayed)),
                     log_otlp::exceptions::exporter_unavailable);
   BOOST_CHECK_THROW(forge::asio::blocking::run(caller, held->metrics(log_otlp::metrics_request{})),
                     log_otlp::exceptions::exporter_unavailable);
   BOOST_CHECK_THROW(forge::asio::blocking::run(caller, held->flush(log_otlp::flush_request{})),
                     log_otlp::exceptions::exporter_unavailable);
}

BOOST_AUTO_TEST_SUITE_END()
