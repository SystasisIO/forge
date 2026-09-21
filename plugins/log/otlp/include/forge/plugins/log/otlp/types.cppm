module;

#include <boost/describe.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.log.otlp.types;

import forge.log.log_message;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::log::otlp {

enum class protocol {
   http_json,
};

enum class overflow_policy {
   drop_new,
};

struct attribute {
   std::string key;
   std::string value;
};

struct header {
   std::string name;
   std::string value;
   std::optional<std::string> secret_id;
   std::optional<std::string> purpose;
};

struct resource_config {
   std::vector<attribute> attributes;
};

struct logger_route {
   std::string name = "default";
   bool enabled = true;
   std::string level = "info";
   bool export_logs = true;
};

struct queue_config {
   std::uint64_t max_records = 8192;
   std::uint64_t max_bytes = 8ULL * 1024ULL * 1024ULL;
   overflow_policy overflow = overflow_policy::drop_new;
};

struct batch_config {
   std::uint64_t max_records = 512;
   std::uint64_t max_bytes = 512ULL * 1024ULL;
   std::uint64_t flush_interval_ms = 5000;
};

struct retry_config {
   std::uint64_t max_attempts = 3;
   std::uint64_t base_delay_ms = 100;
   std::uint64_t max_delay_ms = 5000;
};

struct crash_spool_config {
   bool enabled = false;
   std::string directory = "./crash-spool";
   bool resend_on_startup = true;
};

struct config {
   bool export_enabled = true;
   std::string endpoint = "http://localhost:4318";
   std::string logs_path = "/v1/logs";
   protocol wire_protocol = protocol::http_json;
   std::vector<header> headers;
   std::vector<logger_route> loggers = {logger_route{}};
   resource_config resource;
   queue_config queue;
   batch_config batch;
   retry_config retry;
   std::uint64_t request_timeout_ms = 30000;
   std::uint64_t shutdown_timeout_ms = 5000;
   crash_spool_config crash_spool;
};

struct flush_request {};

struct flush_result {};

struct metrics_request {};

struct metrics {
   std::uint64_t enqueued_records = 0;
   std::uint64_t exported_records = 0;
   std::uint64_t failed_records = 0;
   std::uint64_t dropped_records = 0;
   std::uint64_t retry_attempts = 0;
   std::uint64_t batches = 0;
   std::uint64_t queue_bytes = 0;
   std::uint64_t queue_records = 0;
};

BOOST_DESCRIBE_ENUM(protocol, http_json)
BOOST_DESCRIBE_ENUM(overflow_policy, drop_new)
BOOST_DESCRIBE_STRUCT(attribute, (), (key, value))
BOOST_DESCRIBE_STRUCT(header, (), (name, value, secret_id, purpose))
BOOST_DESCRIBE_STRUCT(resource_config, (), (attributes))
BOOST_DESCRIBE_STRUCT(logger_route, (), (name, enabled, level, export_logs))
BOOST_DESCRIBE_STRUCT(queue_config, (), (max_records, max_bytes, overflow))
BOOST_DESCRIBE_STRUCT(batch_config, (), (max_records, max_bytes, flush_interval_ms))
BOOST_DESCRIBE_STRUCT(retry_config, (), (max_attempts, base_delay_ms, max_delay_ms))
BOOST_DESCRIBE_STRUCT(crash_spool_config, (), (enabled, directory, resend_on_startup))
BOOST_DESCRIBE_STRUCT(config, (),
                      (export_enabled, endpoint, logs_path, wire_protocol, headers, loggers, resource, queue, batch,
                       retry, request_timeout_ms, shutdown_timeout_ms, crash_spool))
BOOST_DESCRIBE_STRUCT(flush_request, (), ())
BOOST_DESCRIBE_STRUCT(flush_result, (), ())
BOOST_DESCRIBE_STRUCT(metrics_request, (), ())
BOOST_DESCRIBE_STRUCT(metrics, (),
                      (enqueued_records, exported_records, failed_records, dropped_records, retry_attempts, batches,
                       queue_bytes, queue_records))

} // namespace forge::plugins::log::otlp

export template <> struct forge::schema::rules<forge::plugins::log::otlp::attribute> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::attribute> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::attribute>();
      schema.field<&forge::plugins::log::otlp::attribute::key>("key").required().non_empty();
      schema.field<&forge::plugins::log::otlp::attribute::value>("value");
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::header> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::header> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::header>();
      schema.field<&forge::plugins::log::otlp::header::name>("name").required().non_empty();
      schema.field<&forge::plugins::log::otlp::header::value>("value").secret();
      schema.field<&forge::plugins::log::otlp::header::secret_id>("secret-id");
      schema.field<&forge::plugins::log::otlp::header::purpose>("purpose");
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::resource_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::resource_config> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::resource_config>();
      schema.field<&forge::plugins::log::otlp::resource_config::attributes>("attributes")
          .items<forge::plugins::log::otlp::attribute>()
          .unique_by<&forge::plugins::log::otlp::attribute::key>();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::logger_route> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::logger_route> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::logger_route>();
      schema.field<&forge::plugins::log::otlp::logger_route::name>("name").required().non_empty().default_value(
          "default");
      schema.field<&forge::plugins::log::otlp::logger_route::enabled>("enabled").default_value(true);
      schema.field<&forge::plugins::log::otlp::logger_route::level>("level").default_value("info").non_empty();
      schema.field<&forge::plugins::log::otlp::logger_route::export_logs>("export").default_value(true);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::queue_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::queue_config> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::queue_config>();
      schema.field<&forge::plugins::log::otlp::queue_config::max_records>("max-records")
          .default_value(std::uint64_t{8192})
          .range(1, 10'000'000);
      schema.field<&forge::plugins::log::otlp::queue_config::max_bytes>("max-bytes")
          .default_value(std::uint64_t{8ULL * 1024ULL * 1024ULL})
          .range(1, 1024ULL * 1024ULL * 1024ULL);
      schema.field<&forge::plugins::log::otlp::queue_config::overflow>("overflow").default_value("drop-new");
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::batch_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::batch_config> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::batch_config>();
      schema.field<&forge::plugins::log::otlp::batch_config::max_records>("max-records")
          .default_value(std::uint64_t{512})
          .range(1, 1'000'000);
      schema.field<&forge::plugins::log::otlp::batch_config::max_bytes>("max-bytes")
          .default_value(std::uint64_t{512ULL * 1024ULL})
          .range(1, 1024ULL * 1024ULL * 1024ULL);
      schema.field<&forge::plugins::log::otlp::batch_config::flush_interval_ms>("flush-interval-ms")
          .default_value(std::uint64_t{5000})
          .range(1, 86'400'000);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::retry_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::retry_config> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::retry_config>();
      schema.field<&forge::plugins::log::otlp::retry_config::max_attempts>("max-attempts")
          .default_value(std::uint64_t{3})
          .range(0, 1000);
      schema.field<&forge::plugins::log::otlp::retry_config::base_delay_ms>("base-delay-ms")
          .default_value(std::uint64_t{100})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::log::otlp::retry_config::max_delay_ms>("max-delay-ms")
          .default_value(std::uint64_t{5000})
          .range(1, 86'400'000);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::crash_spool_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::crash_spool_config> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::crash_spool_config>();
      schema.field<&forge::plugins::log::otlp::crash_spool_config::enabled>("enabled").default_value(false);
      schema.field<&forge::plugins::log::otlp::crash_spool_config::directory>("directory")
          .default_value("./crash-spool")
          .non_empty();
      schema.field<&forge::plugins::log::otlp::crash_spool_config::resend_on_startup>("resend-on-startup")
          .default_value(true);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::log::otlp::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::log::otlp::config> define() {
      auto schema = forge::schema::object<forge::plugins::log::otlp::config>();
      schema.field<&forge::plugins::log::otlp::config::export_enabled>("export-enabled").default_value(true);
      schema.field<&forge::plugins::log::otlp::config::endpoint>("endpoint")
          .default_value("http://localhost:4318")
          .non_empty();
      schema.field<&forge::plugins::log::otlp::config::logs_path>("logs-path").default_value("/v1/logs").non_empty();
      schema.field<&forge::plugins::log::otlp::config::wire_protocol>("protocol").default_value("http-json");
      schema.field<&forge::plugins::log::otlp::config::headers>("headers")
          .items<forge::plugins::log::otlp::header>()
          .unique_by<&forge::plugins::log::otlp::header::name>();
      schema.field<&forge::plugins::log::otlp::config::loggers>("loggers")
          .items<forge::plugins::log::otlp::logger_route>()
          .min_items(1)
          .unique_by<&forge::plugins::log::otlp::logger_route::name>();
      schema.field<&forge::plugins::log::otlp::config::resource>("resource");
      schema.field<&forge::plugins::log::otlp::config::queue>("queue");
      schema.field<&forge::plugins::log::otlp::config::batch>("batch");
      schema.field<&forge::plugins::log::otlp::config::retry>("retry");
      schema.field<&forge::plugins::log::otlp::config::request_timeout_ms>("request-timeout-ms")
          .default_value(std::uint64_t{30000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::log::otlp::config::shutdown_timeout_ms>("shutdown-timeout-ms")
          .default_value(std::uint64_t{5000})
          .range(1, 86'400'000);
      schema.field<&forge::plugins::log::otlp::config::crash_spool>("crash-spool");
      return schema;
   }
};
