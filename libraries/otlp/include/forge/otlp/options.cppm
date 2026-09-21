module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

export module forge.otlp.options;

import forge.net.http.client;

export namespace forge::otlp {

struct attribute {
   std::string key;
   std::string value;
};

struct resource {
   std::vector<attribute> attributes;
};

struct batch_options {
   std::size_t max_records = 512;
   std::size_t max_bytes = 512 * 1024;
   std::chrono::milliseconds flush_interval{5'000};
};

struct queue_limits {
   std::size_t max_records = 8 * 1024;
   std::size_t max_bytes = 8 * 1024 * 1024;
};

struct retry_policy {
   std::uint32_t max_attempts = 3;
   std::chrono::milliseconds base_delay{100};
   std::chrono::milliseconds max_delay{5'000};
};

struct log_exporter_options {
   std::string endpoint = "http://localhost:4318";
   std::string logs_path = "/v1/logs";
   std::vector<attribute> headers;
   resource resource;
   batch_options batch;
   queue_limits queue;
   retry_policy retry;
   std::chrono::milliseconds request_timeout{30'000};
   std::chrono::milliseconds shutdown_timeout{5'000};
   std::string user_agent = "FORGE-OTLP-Exporter-Cpp/1.0.0";
   forge::net::http::client_options http_client;
};

struct exporter_metrics {
   std::uint64_t enqueued_records = 0;
   std::uint64_t dropped_records = 0;
   std::uint64_t exported_records = 0;
   std::uint64_t failed_records = 0;
   std::uint64_t export_attempts = 0;
   std::uint64_t retry_attempts = 0;
   std::size_t queue_depth = 0;
   std::size_t queue_bytes = 0;
};

struct export_result {
   std::uint64_t submitted_records = 0;
   std::uint64_t exported_records = 0;
   std::uint64_t failed_records = 0;
};

} // namespace forge::otlp
