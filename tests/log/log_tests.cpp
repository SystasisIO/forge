#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import forge.exceptions;
import forge.log.appender;
import forge.log.log_message;
import forge.log.logger;
import forge.log.logger_config;
import forge.log.record;
import forge.variant.value;

namespace {

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      records.push_back(record);
   }

   std::vector<forge::log_record> records;
};

class capture_appender final : public forge::appender {
 public:
   void initialize() override {}

   void log(const forge::log_message& message) override {
      messages.push_back(message);
   }

   std::vector<forge::log_message> messages;
};

std::string read_file(const std::filesystem::path& path) {
   auto input = std::ifstream{path};
   return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(log_test_suite)

BOOST_AUTO_TEST_CASE(default_configuration_is_constructible_by_module_consumers) {
   const auto config = forge::logging_config::default_config();

   BOOST_REQUIRE_EQUAL(config.appenders.size(), 2U);
   BOOST_REQUIRE_EQUAL(config.loggers.size(), 1U);
   BOOST_CHECK_EQUAL(config.loggers.front().name, forge::default_logger_name);
   BOOST_REQUIRE_EQUAL(config.loggers.front().appenders.size(), 1U);
   BOOST_CHECK_EQUAL(config.loggers.front().appenders.front(), "stderr");
}

BOOST_AUTO_TEST_CASE(disabled_level_does_not_build_record) {
   auto logger = forge::logger{"test.disabled"};
   logger.set_log_level(forge::log_level::error);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);

   bool evaluated = false;
   forge_log(logger, forge::log_level::debug, "hidden", forge::log_field_provider{[&] {
                evaluated = true;
                return forge::log_field{"expensive", "value"};
             }});

   BOOST_TEST(!evaluated);
   BOOST_TEST(sink->records.empty());
}

BOOST_AUTO_TEST_CASE(direct_log_overloads_obey_originating_logger_policy) {
   auto logger = forge::logger{"test.direct.disabled"};
   logger.set_log_level(forge::log_level::error);
   auto sink = std::make_shared<capture_sink>();
   auto appender = std::make_shared<capture_appender>();
   logger.add_sink(sink);
   logger.add_appender(appender);

   logger.log(forge::log_record{.level = forge::log_level::debug, .message = "hidden record"});
   logger.log(
       forge::log_message{forge::log_context{forge::log_level::info, __FILE__, __LINE__, __func__}, "hidden message"});

   BOOST_TEST(sink->records.empty());
   BOOST_TEST(appender->messages.empty());

   logger.set_enabled(false);
   logger.set_log_level(forge::log_level::all);
   logger.log(forge::log_record{.level = forge::log_level::error, .message = "disabled record"});
   logger.log(forge::log_message{forge::log_context{forge::log_level::error, __FILE__, __LINE__, __func__},
                                 "disabled message"});

   BOOST_TEST(sink->records.empty());
   BOOST_TEST(appender->messages.empty());
}

BOOST_AUTO_TEST_CASE(error_record_captures_stacktrace_and_redacts_secrets) {
   auto logger = forge::logger{"test.error"};
   logger.set_log_level(forge::log_level::debug);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);

   logger.error("failed login", {forge::log_ctx("user", "alice"), forge::log_secret("token", "abc123")});

   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   const auto& record = sink->records.front();
   BOOST_TEST(record.message == "failed login");
   BOOST_REQUIRE_EQUAL(record.fields.size(), 2U);
   BOOST_TEST(record.fields[0].value == "alice");
   BOOST_TEST(record.fields[1].value == "<redacted>");
   BOOST_REQUIRE(record.stacktrace.has_value());
   BOOST_TEST(!record.stacktrace->backend.empty());
}

BOOST_AUTO_TEST_CASE(jsonl_sink_writes_redacted_structured_record) {
   const auto path = std::filesystem::temp_directory_path() / "forge-log-jsonl-test.jsonl";
   std::filesystem::remove(path);

   auto logger = forge::logger{"test.jsonl"};
   logger.set_log_level(forge::log_level::debug);
   logger.add_sink(std::make_shared<forge::jsonl_sink>(path));
   logger.info("configured", {forge::log_ctx("port", 8080), forge::log_secret("password", "secret")});

   const auto text = read_file(path);
   BOOST_TEST(text.find("\"logger\":\"test.jsonl\"") != std::string::npos);
   BOOST_TEST(text.find("\"message\":\"configured\"") != std::string::npos);
   BOOST_TEST(text.find("\"port\":\"8080\"") != std::string::npos);
   BOOST_TEST(text.find("\"password\":\"<redacted>\"") != std::string::npos);
   BOOST_TEST(text.find("secret") == std::string::npos);

   std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(legacy_log_message_macros_reach_structured_sinks) {
   auto logger = forge::logger{"test.legacy.sink"};
   logger.set_log_level(forge::log_level::debug);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);

   forge_ilog(logger, "peer connected ${peer}", ("peer", "node-7"));

   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   const auto& record = sink->records.front();
   BOOST_TEST(record.logger == "test.legacy.sink");
   BOOST_TEST(record.level.value == forge::log_level::info);
   BOOST_TEST(record.message == "peer connected node-7");
   BOOST_REQUIRE_EQUAL(record.fields.size(), 1U);
   BOOST_TEST(record.fields.front().key == "peer");
   BOOST_TEST(record.fields.front().value == "node-7");
}

BOOST_AUTO_TEST_CASE(hierarchy_additively_routes_structured_records_once_with_origin_name) {
   auto parent = forge::logger{"test.parent"};
   parent.set_log_level(forge::log_level::debug);
   auto child = forge::logger{"test.child", parent};
   child.set_log_level(forge::log_level::debug);

   auto shared_appender = std::make_shared<capture_appender>();
   auto shared_sink = std::make_shared<capture_sink>();
   parent.add_appender(shared_appender);
   child.add_appender(shared_appender);
   parent.add_sink(shared_sink);
   child.add_sink(shared_sink);

   child.info("producer heartbeat", {forge::log_ctx("head", 42), forge::log_secret("credential", "hidden")});

   BOOST_REQUIRE_EQUAL(shared_sink->records.size(), 1U);
   const auto& record = shared_sink->records.front();
   BOOST_TEST(record.logger == "test.child");
   BOOST_TEST(record.message == "producer heartbeat");
   BOOST_REQUIRE_EQUAL(record.fields.size(), 2U);
   BOOST_TEST(record.fields[0].value == "42");
   BOOST_TEST(record.fields[1].value == "<redacted>");

   BOOST_REQUIRE_EQUAL(shared_appender->messages.size(), 1U);
   const auto rendered = shared_appender->messages.front().get_limited_message();
   BOOST_TEST(rendered.find("test.child") != std::string::npos);
   BOOST_TEST(rendered.find("head=42") != std::string::npos);
   BOOST_TEST(rendered.find("credential=<redacted>") != std::string::npos);
   BOOST_TEST(rendered.find("hidden") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(hierarchy_additively_routes_legacy_messages_to_parent_sinks_once) {
   auto parent = forge::logger{"test.legacy.parent"};
   parent.set_log_level(forge::log_level::debug);
   auto child = forge::logger{"test.legacy.child", parent};
   child.set_log_level(forge::log_level::debug);

   auto shared_sink = std::make_shared<capture_sink>();
   parent.add_sink(shared_sink);
   child.add_sink(shared_sink);

   forge_ilog(child, "peer connected ${peer}", ("peer", "node-7"));

   BOOST_REQUIRE_EQUAL(shared_sink->records.size(), 1U);
   const auto& record = shared_sink->records.front();
   BOOST_TEST(record.logger == "test.legacy.child");
   BOOST_TEST(record.message == "peer connected node-7");
   BOOST_REQUIRE_EQUAL(record.fields.size(), 1U);
   BOOST_TEST(record.fields.front().key == "peer");
   BOOST_TEST(record.fields.front().value == "node-7");
}

BOOST_AUTO_TEST_CASE(named_logger_inherits_the_configured_default_route_and_preserves_its_name) {
   auto parent = forge::logger{"default"};
   parent.set_log_level(forge::log_level::info);
   auto sink = std::make_shared<capture_sink>();
   parent.add_sink(sink);
   forge::logger::update("default", parent);

   auto child = forge::logger::get("test.configured-child");
   child.info("ready");

   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   BOOST_TEST(sink->records.front().logger == "test.configured-child");
   BOOST_TEST(sink->records.front().message == "ready");
}

BOOST_AUTO_TEST_CASE(exception_chain_can_be_routed_to_logger) {
   auto logger = forge::logger{"test.exception"};
   logger.set_log_level(forge::log_level::debug);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);

   forge::exceptions::set_log_sink([&](std::string_view message) {
      logger.error("exception captured", {forge::log_ctx("chain", message), forge::log_secret("token", "hidden")});
   });

   try {
      try {
         throw std::runtime_error{"inner"};
      }
      FORGE_CAPTURE_AND_LOG("outer", forge::exceptions::ctx("phase", "startup"),
                            forge::exceptions::secret("password", "secret"))
   } catch (...) {
      BOOST_FAIL("FORGE_CAPTURE_AND_LOG must not rethrow");
   }

   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   const auto& record = sink->records.front();
   BOOST_TEST(record.message == "exception captured");
   BOOST_TEST(record.fields.front().value.find("outer") != std::string::npos);
   BOOST_TEST(record.fields.front().value.find("inner") != std::string::npos);
   BOOST_TEST(record.fields.front().value.find("secret") == std::string::npos);
   BOOST_TEST(record.fields.back().value == "<redacted>");

   forge::exceptions::set_log_sink({});
}

BOOST_AUTO_TEST_SUITE_END()
