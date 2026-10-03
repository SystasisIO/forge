#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace log_fixtures {

struct credentials {
   std::string user;
   std::string password;
};
BOOST_DESCRIBE_STRUCT(credentials, (), (user, password))

struct nested {
   std::vector<credentials> entries;
   int count = 0;
};

class opaque {
   int _value = 7;
};

struct secret_value {
   std::string value;
};
bool diagnostic_is_secret(const secret_value&) {
   return true;
}

struct failing_value {};

} // namespace log_fixtures

import forge.exceptions;
import forge.log.logger;
import forge.log.logger_config;
import forge.log.record;
import forge.schema.object;
import forge.variant.value;

namespace log_fixtures {
void to_variant(const failing_value&, forge::variant&) {
   throw std::runtime_error{"must-not-be-rendered"};
}
} // namespace log_fixtures

template <> struct forge::schema::rules<log_fixtures::credentials> {
   static forge::schema::object_schema<log_fixtures::credentials> define() {
      auto schema = forge::schema::object<log_fixtures::credentials>();
      static_cast<void>(schema.field<&log_fixtures::credentials::user>("user-name"));
      schema.field<&log_fixtures::credentials::password>("password").secret();
      return schema;
   }
};

namespace {

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      records.push_back(record);
   }
   std::vector<forge::log_record> records;
};

std::string read_file(const std::filesystem::path& path) {
   auto input = std::ifstream{path};
   return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(log_test_suite)

BOOST_AUTO_TEST_CASE(default_configuration_uses_a_structured_console_sink) {
   const auto config = forge::logging_config::default_config();
   BOOST_REQUIRE_EQUAL(config.sinks.size(), 1U);
   BOOST_TEST(config.sinks.front().type == "console");
   BOOST_REQUIRE_EQUAL(config.loggers.size(), 1U);
   BOOST_TEST(config.loggers.front().name == forge::default_logger_name);
   BOOST_REQUIRE_EQUAL(config.loggers.front().sinks.size(), 1U);
   BOOST_TEST(config.loggers.front().sinks.front() == "stderr");
}

BOOST_AUTO_TEST_CASE(filtering_is_lazy_and_logger_expression_is_evaluated_once) {
   auto logger = forge::logger{"test.lazy"};
   logger.set_log_level(forge::log_level::error);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   int logger_evaluations = 0;
   int message_evaluations = 0;
   int value_evaluations = 0;
   const auto route = [&]() -> forge::logger& {
      ++logger_evaluations;
      return logger;
   };
   const auto message = [&] {
      ++message_evaluations;
      return std::string{"value ${value}"};
   };
   dlog(route(), message(), ("value", ++value_evaluations));
   BOOST_TEST(logger_evaluations == 1);
   BOOST_TEST(message_evaluations == 0);
   BOOST_TEST(value_evaluations == 0);
   BOOST_TEST(sink->records.empty());

   elog(route(), message(), ("value", ++value_evaluations));
   BOOST_TEST(logger_evaluations == 2);
   BOOST_TEST(message_evaluations == 1);
   BOOST_TEST(value_evaluations == 1);
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   BOOST_TEST(sink->records.front().message == "value 1");

   auto null_logger = forge::logger{nullptr};
   ilog(null_logger, message(), ("value", ++value_evaluations));
   BOOST_TEST(message_evaluations == 1);
   BOOST_TEST(value_evaluations == 1);
}

BOOST_AUTO_TEST_CASE(named_object_and_default_forms_preserve_levels_and_callsite) {
   auto sink = std::make_shared<capture_sink>();
   auto parent = forge::logger{"default"};
   parent.set_log_level(forge::log_level::all);
   parent.add_sink(sink);
   forge::logger::update("default", parent);

   tlog("test.named", "trace", ("value", 1));
   dlog(std::string{"test.named"}, "debug");
   auto named = forge::logger::get("test.named");
   const auto line = __LINE__ + 1;
   ilog(named, "field ${value}", ("value", "visible"));
   wlog(std::string_view{"test.named"}, "warn");
   elog(named, "error");
   ilog("short default message");

   BOOST_REQUIRE_EQUAL(sink->records.size(), 6U);
   const auto& record = sink->records[2];
   BOOST_TEST(record.logger == "test.named");
   BOOST_TEST(record.message == "field visible");
   BOOST_TEST(record.location.line() == line);
   BOOST_TEST(std::string{record.location.file_name()} == __FILE__);
   BOOST_TEST(!record.thread_id.empty());
   BOOST_TEST(!record.thread_name.empty());
   BOOST_TEST(record.timestamp.time_since_epoch().count() > 0);
   BOOST_TEST(sink->records[0].level.value == forge::log_level::all);
   BOOST_TEST(sink->records[1].level.value == forge::log_level::debug);
   BOOST_TEST(sink->records[2].level.value == forge::log_level::info);
   BOOST_TEST(sink->records[3].level.value == forge::log_level::warn);
   BOOST_TEST(sink->records[4].level.value == forge::log_level::error);
   BOOST_TEST(sink->records[5].logger == "default");
}

BOOST_AUTO_TEST_CASE(macro_internal_route_does_not_shadow_caller_logger_or_field_expressions) {
   auto forge_log_route_ = forge::logger{"test.hygiene"};
   forge_log_route_.set_log_level(forge::log_level::info);
   auto sink = std::make_shared<capture_sink>();
   forge_log_route_.add_sink(sink);
   ilog(forge_log_route_, forge_log_route_.get_name());
   ilog(forge_log_route_, "route ${name}", ("name", forge_log_route_.get_name()));
   BOOST_REQUIRE_EQUAL(sink->records.size(), 2U);
   BOOST_TEST(sink->records.front().message == "test.hygiene");
   BOOST_TEST(sink->records.back().message == "route test.hygiene");
}

BOOST_AUTO_TEST_CASE(schema_redaction_precedes_interpolation_through_nested_aggregate_containers) {
   auto logger = forge::logger{"test.diagnostic"};
   logger.set_log_level(forge::log_level::info);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   const auto payload = log_fixtures::nested{{{"alice", "private-fixture-value"}}, 1};
   ilog(logger, "payload ${payload}", ("payload", payload)("password", "ordinary-public-string"));

   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   const auto& record = sink->records.front();
   BOOST_TEST(record.message.find("private-fixture-value") == std::string::npos);
   BOOST_TEST(record.message.find("<redacted>") != std::string::npos);
   BOOST_TEST(record.message.find("user-name") != std::string::npos);
   BOOST_TEST(record.message.find("alice") != std::string::npos);
   BOOST_TEST(record.fields.front().value.find("private-fixture-value") == std::string::npos);
   BOOST_TEST(record.fields[1].value == "ordinary-public-string");
}

BOOST_AUTO_TEST_CASE(secret_unsupported_and_failed_values_use_safe_diagnostics) {
   auto logger = forge::logger{"test.safe"};
   logger.set_log_level(forge::log_level::info);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   ilog(logger, "${secret} ${unknown} ${failure}",
        ("secret", log_fixtures::secret_value{"private-fixture-value"})("unknown", log_fixtures::opaque{})(
            "failure", log_fixtures::failing_value{}));
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   BOOST_TEST(sink->records.front().message == "<redacted> <unsupported> <diagnostic-error>");
}

BOOST_AUTO_TEST_CASE(full_record_preserves_metadata_with_macro_level_and_logger_route) {
   auto logger = forge::logger{"test.record"};
   logger.set_log_level(forge::log_level::info);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   const auto location = std::source_location::current();
   const auto timestamp = std::chrono::sys_time<std::chrono::microseconds>{std::chrono::microseconds{123456}};
   auto record = forge::log_record{
       .level = forge::log_level::off,
       .logger = "ignored-route",
       .component = "execution",
       .message = "failure ${token}",
       .fields = {forge::log_secret("token", "private-fixture-value")},
       .timestamp = timestamp,
       .thread_id = "thread-42",
       .thread_name = "fixture",
       .location = location,
       .stacktrace = forge::stacktrace_snapshot{.backend = "fixture", .unavailable_reason = "no-frames"},
       .exception_chain = "outer -> inner",
   };
   elog(logger, record);
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   const auto& actual = sink->records.front();
   BOOST_TEST(actual.level.value == forge::log_level::error);
   BOOST_TEST(actual.logger == "test.record");
   BOOST_TEST(actual.component == record.component);
   BOOST_TEST(actual.timestamp.time_since_epoch().count() == timestamp.time_since_epoch().count());
   BOOST_TEST(actual.thread_id == record.thread_id);
   BOOST_TEST(actual.thread_name == record.thread_name);
   BOOST_TEST(actual.location.line() == location.line());
   BOOST_TEST(actual.exception_chain == record.exception_chain);
   BOOST_REQUIRE(actual.stacktrace.has_value());
   BOOST_TEST(actual.stacktrace->backend == "fixture");
   BOOST_TEST(actual.message == "failure <redacted>");
   BOOST_TEST(forge::format_json_log_record(actual).find("private-fixture-value") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(full_record_without_location_uses_macro_callsite_and_filters_before_construction) {
   auto logger = forge::logger{"test.record.lazy"};
   logger.set_log_level(forge::log_level::error);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   int evaluations = 0;
   const auto build = [&] {
      ++evaluations;
      return forge::log_record{.message = "full record"};
   };
   ilog(logger, build());
   BOOST_TEST(evaluations == 0);
   const auto line = __LINE__ + 1;
   elog(logger, build());
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   BOOST_TEST(evaluations == 1);
   BOOST_TEST(sink->records.front().location.line() == line);
}

BOOST_AUTO_TEST_CASE(direct_record_and_convenience_overloads_keep_origin_filtering) {
   auto logger = forge::logger{"test.direct"};
   logger.set_log_level(forge::log_level::error);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   logger.log(forge::log_record{.level = forge::log_level::debug, .message = "hidden"});
   logger.info("hidden");
   BOOST_TEST(sink->records.empty());
   logger.error("direct ${value}", {forge::log_ctx("value", 42)});
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   BOOST_TEST(sink->records.front().message == "direct 42");
   BOOST_REQUIRE(sink->records.front().stacktrace.has_value());
   logger.set_enabled(false);
   logger.error("disabled");
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
}

BOOST_AUTO_TEST_CASE(hierarchy_routes_once_with_origin_name_even_with_a_cycle) {
   auto parent = forge::logger{"test.parent"};
   auto child = forge::logger{"test.child", parent};
   child.set_log_level(forge::log_level::info);
   parent.set_parent(child);
   auto shared = std::make_shared<capture_sink>();
   parent.add_sink(shared);
   child.add_sink(shared);
   child.add_sink(shared);
   ilog(child, "head ${head}", ("head", 42));
   BOOST_REQUIRE_EQUAL(shared->records.size(), 1U);
   BOOST_TEST(shared->records.front().logger == "test.child");
   BOOST_TEST(shared->records.front().message == "head 42");
   parent.set_parent(nullptr);
}

BOOST_AUTO_TEST_CASE(console_file_jsonl_and_capture_receive_the_same_redacted_event) {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto base = std::filesystem::temp_directory_path() / ("forge-log-test-" + std::to_string(nonce));
   const auto text_path = std::filesystem::path{base.string() + ".log"};
   const auto json_path = std::filesystem::path{base.string() + ".jsonl"};
   auto logger = forge::logger{"test.sinks"};
   logger.set_log_level(forge::log_level::info);
   auto capture = std::make_shared<capture_sink>();
   logger.add_sink(capture);
   logger.add_sink(std::make_shared<forge::file_sink>(text_path, false));
   logger.add_sink(std::make_shared<forge::jsonl_sink>(json_path, false));
   logger.add_sink(std::make_shared<forge::console_sink>(false));
   auto console = std::ostringstream{};
   auto* previous = std::cout.rdbuf(console.rdbuf());
   ilog(logger, "user ${payload}", ("payload", log_fixtures::credentials{"alice", "private-fixture-value"}));
   std::cout.rdbuf(previous);
   BOOST_REQUIRE_EQUAL(capture->records.size(), 1U);
   const auto canonical = forge::format_text_log_record(capture->records.front()) + "\n";
   BOOST_TEST(console.str() == canonical);
   BOOST_TEST(read_file(text_path) == canonical);
   BOOST_TEST(read_file(json_path) == forge::format_json_log_record(capture->records.front()) + "\n");
   BOOST_TEST(canonical.find("private-fixture-value") == std::string::npos);
   std::filesystem::remove(text_path);
   std::filesystem::remove(json_path);
}

BOOST_AUTO_TEST_CASE(sink_configuration_preserves_additive_routes_and_rejects_unknown_references) {
   auto config = forge::logging_config{};
   config.sinks.push_back({.name = "console", .type = "console"});
   auto parent = forge::logger_config{"default"};
   parent.level = forge::log_level::info;
   parent.sinks = {"console"};
   config.loggers.push_back(parent);
   auto child = forge::logger_config{"test.configured"};
   child.sinks = {"console"};
   config.loggers.push_back(child);
   BOOST_TEST(forge::configure_logging(config));
   auto logger = forge::logger::get("test.configured");
   BOOST_REQUIRE(logger.get_parent() != nullptr);
   BOOST_TEST(logger.get_log_level().value == forge::log_level::info);
   config.loggers.back().sinks = {"missing"};
   BOOST_TEST(!forge::configure_logging(config));
   BOOST_CHECK(forge::logger::get("test.configured") != nullptr);
}

BOOST_AUTO_TEST_CASE(exception_chain_can_be_routed_to_the_record_path) {
   auto logger = forge::logger{"test.exception"};
   logger.set_log_level(forge::log_level::error);
   auto sink = std::make_shared<capture_sink>();
   logger.add_sink(sink);
   forge::exceptions::set_log_sink(
       [&](std::string_view message) { elog(logger, "exception captured", ("chain", message)); });
   try {
      try {
         throw std::runtime_error{"inner"};
      }
      FORGE_CAPTURE_AND_LOG("outer", forge::exceptions::ctx("phase", "startup"),
                            forge::exceptions::secret("password", "private-fixture-value"))
   } catch (...) {
      BOOST_FAIL("FORGE_CAPTURE_AND_LOG must not rethrow");
   }
   forge::exceptions::set_log_sink({});
   BOOST_REQUIRE_EQUAL(sink->records.size(), 1U);
   BOOST_TEST(sink->records.front().fields.front().value.find("outer") != std::string::npos);
   BOOST_TEST(sink->records.front().fields.front().value.find("inner") != std::string::npos);
   BOOST_TEST(sink->records.front().fields.front().value.find("private-fixture-value") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(invalid_config_preserves_existing_file_and_routes_before_sink_construction) {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto path = std::filesystem::temp_directory_path() / ("forge-log-validation-" + std::to_string(nonce));
   {
      auto file = std::ofstream{path};
      file << "existing-content";
   }
   auto parent = forge::logger{"default"};
   parent.set_log_level(forge::log_level::info);
   auto capture = std::make_shared<capture_sink>();
   parent.add_sink(capture);
   forge::logger::update("default", parent);
   const auto fresh = [&] {
      auto config = forge::logging_config{};
      config.sinks.push_back({.name = "file",
                              .type = "file",
                              .args = forge::mutable_variant_object{}("path", path.string())("append", false)});
      return config;
   };
   auto duplicate_sink = fresh();
   duplicate_sink.sinks.push_back(duplicate_sink.sinks.front());
   BOOST_TEST(!forge::configure_logging(duplicate_sink));
   auto duplicate_default = fresh();
   duplicate_default.loggers.emplace_back("default");
   duplicate_default.loggers.emplace_back("default");
   BOOST_TEST(!forge::configure_logging(duplicate_default));
   auto duplicate_named = fresh();
   duplicate_named.loggers.emplace_back("named");
   duplicate_named.loggers.emplace_back("named");
   BOOST_TEST(!forge::configure_logging(duplicate_named));
   auto missing_sink = fresh();
   missing_sink.loggers.emplace_back("default");
   missing_sink.loggers.back().sinks = {"missing"};
   BOOST_TEST(!forge::configure_logging(missing_sink));
   auto invalid_type = fresh();
   invalid_type.sinks.push_back({.name = "bad", .type = "unknown"});
   BOOST_TEST(!forge::configure_logging(invalid_type));
   auto invalid_args = fresh();
   invalid_args.sinks.push_back(
       {.name = "bad", .type = "console", .args = forge::mutable_variant_object{}("stream", "unknown")});
   BOOST_TEST(!forge::configure_logging(invalid_args));
   BOOST_TEST(read_file(path) == "existing-content");
   ilog("record after rejected config");
   BOOST_REQUIRE_EQUAL(capture->records.size(), 1U);
   BOOST_TEST(capture->records.front().message == "record after rejected config");
   std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(concurrent_console_sink_outputs_complete_records) {
   auto logger = forge::logger{"test.concurrent"};
   logger.set_log_level(forge::log_level::info);
   logger.add_sink(std::make_shared<forge::console_sink>(false));
   auto output = std::ostringstream{};
   auto* previous = std::cout.rdbuf(output.rdbuf());
   auto workers = std::vector<std::thread>{};
   for (int worker = 0; worker < 4; ++worker) {
      workers.emplace_back([&, worker] {
         for (int item = 0; item < 50; ++item) {
            ilog(logger, "event ${worker}/${item}", ("worker", worker)("item", item));
         }
      });
   }
   for (auto& worker : workers) {
      worker.join();
   }
   std::cout.rdbuf(previous);
   auto input = std::istringstream{output.str()};
   std::string line;
   std::size_t count = 0;
   while (std::getline(input, line)) {
      ++count;
      const auto event = line.find(" info test.concurrent event ");
      BOOST_REQUIRE(event != std::string::npos);
      BOOST_TEST(line.find(" info test.concurrent event ", event + 1) == std::string::npos);
      BOOST_TEST(line.find(" worker=") != std::string::npos);
      BOOST_TEST(line.find(" item=") != std::string::npos);
   }
   BOOST_TEST(count == 200U);
}

BOOST_AUTO_TEST_SUITE_END()
