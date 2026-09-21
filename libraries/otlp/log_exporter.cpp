module;

#include <forge/exceptions/macros.hpp>

#include <boost/describe.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

module forge.otlp.log_exporter;

import forge.asio.runtime;
import forge.exceptions;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.types;
import forge.codec.json;
import forge.log.log_message;

namespace forge::otlp {
namespace {

namespace asio = boost::asio;
using asio::awaitable;
using asio::use_awaitable;
using namespace std::chrono_literals;

constexpr auto instrumentation_scope_name = std::string_view{"forge.log"};
constexpr auto instrumentation_scope_version = std::string_view{"1.0.0"};

bool positive(std::chrono::milliseconds value) {
   return value.count() > 0;
}

std::string severity_text(forge::log_level level) {
   switch (static_cast<forge::log_level::values>(static_cast<int>(level))) {
   case forge::log_level::debug:
      return "DEBUG";
   case forge::log_level::info:
      return "INFO";
   case forge::log_level::warn:
      return "WARN";
   case forge::log_level::error:
      return "ERROR";
   case forge::log_level::all:
      return "TRACE";
   case forge::log_level::off:
      return "UNSPECIFIED";
   }
   return "UNSPECIFIED";
}

int severity_number(forge::log_level level) {
   switch (static_cast<forge::log_level::values>(static_cast<int>(level))) {
   case forge::log_level::all:
      return 1;
   case forge::log_level::debug:
      return 5;
   case forge::log_level::info:
      return 9;
   case forge::log_level::warn:
      return 13;
   case forge::log_level::error:
      return 17;
   case forge::log_level::off:
      return 0;
   }
   return 0;
}

std::uint64_t timestamp_nanos(const forge::log_record& record) {
   const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(record.timestamp.time_since_epoch());
   if (nanos.count() <= 0) {
      return 0;
   }
   return static_cast<std::uint64_t>(nanos.count());
}

std::size_t estimate_record_bytes(const forge::log_record& record) {
   auto result = std::size_t{256};
   result += record.logger.size() + record.component.size() + record.message.size();
   result += record.thread_id.size() + record.thread_name.size() + record.exception_chain.size();
   for (const auto& field : record.fields) {
      result += field.key.size() + field.value.size() + 48;
   }
   if (record.stacktrace.has_value()) {
      result += 128 + record.stacktrace->frames.size() * 96;
   }
   return result;
}

std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view value) {
   if (value.empty()) {
      return std::nullopt;
   }

   auto seconds = std::uint64_t{0};
   const auto* begin = value.data();
   const auto* end = value.data() + value.size();
   const auto parsed = std::from_chars(begin, end, seconds);
   if (parsed.ec != std::errc{} || parsed.ptr != end) {
      return std::nullopt;
   }
   const auto max_seconds =
       static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max() / 1000);
   if (seconds > max_seconds) {
      return std::chrono::milliseconds::max();
   }
   return std::chrono::seconds{seconds};
}

bool retryable_status(forge::net::http::status status) {
   return status == forge::net::http::status::too_many_requests || status == forge::net::http::status::bad_gateway ||
          status == forge::net::http::status::service_unavailable ||
          status == forge::net::http::status::gateway_timeout;
}

bool success_status(forge::net::http::status status) {
   const auto value = static_cast<unsigned>(status);
   return value >= 200 && value < 300;
}

std::chrono::milliseconds cap_delay(std::chrono::milliseconds value, std::chrono::milliseconds max) {
   if (max.count() <= 0) {
      return 0ms;
   }
   if (value.count() <= 0) {
      return 0ms;
   }
   return std::min(value, max);
}

std::chrono::milliseconds exponential_delay(const retry_policy& retry, std::uint32_t attempt) {
   auto delay = retry.base_delay;
   for (std::uint32_t index = 0; index < attempt; ++index) {
      if (delay > retry.max_delay / 2) {
         delay = retry.max_delay;
         break;
      }
      delay *= 2;
   }
   return cap_delay(delay, retry.max_delay);
}

struct otlp_string_value {
   std::string stringValue;
};

struct otlp_attribute {
   std::string key;
   otlp_string_value value;
};

struct otlp_resource {
   std::vector<otlp_attribute> attributes;
};

struct otlp_scope {
   std::string name;
   std::string version;
};

struct otlp_log_body {
   std::string stringValue;
};

struct otlp_log_record {
   std::string timeUnixNano;
   int severityNumber = 0;
   std::string severityText;
   otlp_log_body body;
   std::vector<otlp_attribute> attributes;
};

struct otlp_scope_log {
   otlp_scope scope;
   std::vector<otlp_log_record> logRecords;
};

struct otlp_resource_log {
   otlp_resource resource;
   std::vector<otlp_scope_log> scopeLogs;
};

struct otlp_export_request {
   std::vector<otlp_resource_log> resourceLogs;
};

BOOST_DESCRIBE_STRUCT(otlp_string_value, (), (stringValue))
BOOST_DESCRIBE_STRUCT(otlp_attribute, (), (key, value))
BOOST_DESCRIBE_STRUCT(otlp_resource, (), (attributes))
BOOST_DESCRIBE_STRUCT(otlp_scope, (), (name, version))
BOOST_DESCRIBE_STRUCT(otlp_log_body, (), (stringValue))
BOOST_DESCRIBE_STRUCT(otlp_log_record, (), (timeUnixNano, severityNumber, severityText, body, attributes))
BOOST_DESCRIBE_STRUCT(otlp_scope_log, (), (scope, logRecords))
BOOST_DESCRIBE_STRUCT(otlp_resource_log, (), (resource, scopeLogs))
BOOST_DESCRIBE_STRUCT(otlp_export_request, (), (resourceLogs))

[[nodiscard]] otlp_attribute make_string_attribute(std::string key, std::string value) {
   return otlp_attribute{.key = std::move(key), .value = otlp_string_value{.stringValue = std::move(value)}};
}

[[nodiscard]] std::vector<otlp_attribute> record_attributes(const forge::log_record& record) {
   auto attributes = std::vector<otlp_attribute>{};
   attributes.reserve(record.fields.size() + 10);
   attributes.push_back(make_string_attribute("logger", record.logger));
   attributes.push_back(make_string_attribute("component", record.component));
   attributes.push_back(make_string_attribute("thread.id", record.thread_id));
   attributes.push_back(make_string_attribute("thread.name", record.thread_name));
   attributes.push_back(make_string_attribute("source.file", record.location.file_name()));
   attributes.push_back(make_string_attribute("source.function", record.location.function_name()));
   attributes.push_back(make_string_attribute("source.line", std::to_string(record.location.line())));
   if (!record.exception_chain.empty()) {
      attributes.push_back(make_string_attribute("exception.chain", record.exception_chain));
   }
   for (const auto& field : record.fields) {
      attributes.push_back(make_string_attribute(field.key, field.redacted ? std::string{"<redacted>"} : field.value));
   }
   if (record.stacktrace.has_value()) {
      attributes.push_back(make_string_attribute("stacktrace.backend", record.stacktrace->backend));
      if (!record.stacktrace->unavailable_reason.empty()) {
         attributes.push_back(
             make_string_attribute("stacktrace.unavailable_reason", record.stacktrace->unavailable_reason));
      }
      auto formatted = std::string{};
      for (const auto& frame : record.stacktrace->frames) {
         if (!formatted.empty()) {
            formatted.push_back('\n');
         }
         formatted += '#';
         formatted += std::to_string(frame.index);
         formatted += ' ';
         formatted += frame.name;
         formatted += " 0x";
         formatted += std::to_string(frame.address);
         if (!frame.source_file.empty()) {
            formatted += ' ';
            formatted += frame.source_file;
            formatted += ':';
            formatted += std::to_string(frame.source_line);
         }
      }
      if (!formatted.empty()) {
         attributes.push_back(make_string_attribute("stacktrace.frames", std::move(formatted)));
      }
   }
   return attributes;
}

[[nodiscard]] otlp_log_record make_log_record(const forge::log_record& record) {
   return otlp_log_record{
       .timeUnixNano = std::to_string(timestamp_nanos(record)),
       .severityNumber = severity_number(record.level),
       .severityText = severity_text(record.level),
       .body = otlp_log_body{.stringValue = record.message},
       .attributes = record_attributes(record),
   };
}

} // namespace

struct log_exporter::impl : std::enable_shared_from_this<impl> {
   struct queued_record {
      forge::log_record record;
      std::size_t bytes = 0;
   };

   struct flush_waiter {
      explicit flush_waiter(asio::any_io_executor executor_value)
          : timer(std::move(executor_value), (std::chrono::steady_clock::time_point::max)()) {}

      asio::steady_timer timer;
      bool done = false;
      std::exception_ptr error;
   };

   impl(forge::asio::runtime& runtime_value, log_exporter_options options_value)
       : runtime(runtime_value), options(std::move(options_value)), strand(asio::make_strand(runtime.context())),
         flush_timer(strand, (std::chrono::steady_clock::time_point::max)()) {
      validate_options();
      try {
         endpoint = forge::net::http::parse_base_url(options.endpoint);
      } catch (const std::exception& error) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid OTLP endpoint",
                               forge::exceptions::ctx("endpoint", options.endpoint),
                               forge::exceptions::ctx("reason", error.what()));
      }
      client = std::make_unique<forge::net::http::client>(runtime, endpoint, std::move(options.http_client));
   }

   void validate_options() const {
      if (options.endpoint.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP endpoint must not be empty");
      }
      if (options.logs_path.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP logs path must not be empty");
      }
      if (options.batch.max_records == 0 || options.batch.max_bytes == 0) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP batch limits must be positive");
      }
      if (options.queue.max_records == 0 || options.queue.max_bytes == 0) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP queue limits must be positive");
      }
      if (!positive(options.batch.flush_interval)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP flush interval must be positive");
      }
      if (!positive(options.request_timeout)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP request timeout must be positive");
      }
      if (!positive(options.shutdown_timeout)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP shutdown timeout must be positive");
      }
      if (!positive(options.retry.base_delay) || !positive(options.retry.max_delay)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP retry delays must be positive");
      }
   }

   bool enqueue(const forge::log_record& record) {
      const auto bytes = estimate_record_bytes(record);
      auto should_flush = false;
      {
         const auto lock = std::scoped_lock{mutex};
         if (closed || bytes > options.queue.max_bytes || queue.size() >= options.queue.max_records ||
             bytes > options.queue.max_bytes - queue_bytes) {
            ++current_metrics.dropped_records;
            return false;
         }

         queue.push_back(queued_record{.record = record, .bytes = bytes});
         queue_bytes += bytes;
         ++current_metrics.enqueued_records;
         refresh_queue_metrics_locked();
         should_flush = queue.size() >= options.batch.max_records || queue_bytes >= options.batch.max_bytes;
      }

      asio::post(strand, [self = shared_from_this(), should_flush] { self->on_enqueue(should_flush); });
      return true;
   }

   void on_enqueue(bool should_flush) {
      if (should_flush) {
         flush_timer.cancel();
         timer_armed = false;
         start_flush();
         return;
      }
      arm_flush_timer();
   }

   void arm_flush_timer() {
      if (flush_active || timer_armed) {
         return;
      }
      timer_armed = true;
      flush_timer.expires_after(options.batch.flush_interval);
      asio::co_spawn(
          strand,
          [self = shared_from_this()]() -> awaitable<void> {
             auto error = boost::system::error_code{};
             co_await self->flush_timer.async_wait(asio::redirect_error(use_awaitable, error));
             if (!error) {
                self->timer_armed = false;
                self->start_flush();
             }
          },
          asio::detached);
   }

   void start_flush() {
      if (flush_active) {
         return;
      }
      if (empty_queue()) {
         complete_waiters(nullptr);
         return;
      }
      flush_active = true;
      asio::co_spawn(
          strand,
          [self = shared_from_this()]() -> awaitable<void> {
             auto error = std::exception_ptr{};
             try {
                co_await self->drain();
             } catch (...) {
                error = std::current_exception();
             }
             self->flush_active = false;
             self->complete_waiters(error);
             if (!self->empty_queue() && !self->closed_snapshot()) {
                self->arm_flush_timer();
             }
          },
          asio::detached);
   }

   awaitable<void> wait_for_flush() {
      if (empty_queue() && !flush_active) {
         co_return;
      }

      auto waiter = std::make_shared<flush_waiter>(strand);
      waiters.push_back(waiter);
      start_flush();

      while (!waiter->done) {
         auto error = boost::system::error_code{};
         co_await waiter->timer.async_wait(asio::redirect_error(use_awaitable, error));
         static_cast<void>(error);
      }
      if (waiter->error) {
         std::rethrow_exception(waiter->error);
      }
   }

   awaitable<void> shutdown() {
      co_await asio::post(strand, use_awaitable);
      {
         const auto lock = std::scoped_lock{mutex};
         closed = true;
      }
      shutdown_deadline = std::chrono::steady_clock::now() + options.shutdown_timeout;
      flush_timer.cancel();
      timer_armed = false;
      co_await wait_for_flush();
      drop_all_queued();
      complete_waiters(nullptr);
   }

   void close_without_flush() {
      {
         const auto lock = std::scoped_lock{mutex};
         closed = true;
         current_metrics.dropped_records += queue.size();
         queue.clear();
         queue_bytes = 0;
         refresh_queue_metrics_locked();
      }
      flush_timer.cancel();
   }

   awaitable<void> drain() {
      for (;;) {
         if (shutdown_expired()) {
            drop_all_queued();
            co_return;
         }

         auto batch = take_batch();
         if (batch.empty()) {
            co_return;
         }

         const auto exported = co_await export_batch(batch);
         if (exported) {
            record_exported(batch.size());
         } else {
            record_failed(batch.size());
         }
      }
   }

   awaitable<export_result> export_now(std::vector<forge::log_record> records) {
      co_await asio::post(strand, use_awaitable);
      auto result = export_result{.submitted_records = records.size()};
      if (records.empty()) {
         co_return result;
      }

      const auto exported = co_await export_batch(records);
      if (exported) {
         record_exported(records.size());
         result.exported_records = records.size();
      } else {
         record_direct_failed(records.size());
         result.failed_records = records.size();
      }
      co_return result;
   }

   std::vector<forge::log_record> take_batch() {
      auto records = std::vector<forge::log_record>{};
      const auto lock = std::scoped_lock{mutex};
      auto bytes = std::size_t{0};
      while (!queue.empty() && records.size() < options.batch.max_records) {
         const auto next_bytes = queue.front().bytes;
         if (!records.empty() && next_bytes > options.batch.max_bytes - std::min(bytes, options.batch.max_bytes)) {
            break;
         }
         bytes += next_bytes;
         queue_bytes -= std::min(queue_bytes, next_bytes);
         records.push_back(std::move(queue.front().record));
         queue.pop_front();
      }
      refresh_queue_metrics_locked();
      return records;
   }

   awaitable<bool> export_batch(const std::vector<forge::log_record>& records) {
      const auto payload = encode_logs(records);
      auto attempt = std::uint32_t{0};
      for (;;) {
         if (shutdown_expired()) {
            co_return false;
         }

         record_export_attempt();
         auto should_retry = false;
         auto delay = std::chrono::milliseconds{0};
         try {
            auto request = make_request(payload);
            const auto timeout = remaining_request_timeout();
            if (timeout.count() <= 0) {
               co_return false;
            }
            auto response = co_await client->async_request(std::move(request), {.timeout = timeout});
            if (success_status(response.result())) {
               co_return true;
            }
            if (!retryable_status(response.result()) || attempt >= options.retry.max_attempts) {
               co_return false;
            }
            const auto retry_after = response.find(forge::net::http::field::retry_after);
            auto response_delay = retry_after != response.end()
                                      ? parse_retry_after(std::string_view{retry_after->value()})
                                      : std::optional<std::chrono::milliseconds>{};
            delay = response_delay.value_or(retry_delay(attempt));
            should_retry = true;
         } catch (...) {
            if (attempt >= options.retry.max_attempts) {
               co_return false;
            }
            delay = retry_delay(attempt);
            should_retry = true;
         }

         if (should_retry) {
            ++attempt;
            record_retry();
            if (!co_await sleep_before_retry(delay)) {
               co_return false;
            }
         }
      }
   }

   forge::net::http::request make_request(const std::string& payload) const {
      auto request = forge::net::http::request{};
      request.method(forge::net::http::method::post);
      request.target(endpoint.make_target(options.logs_path));
      request.version(11);
      request.body() = payload;
      request.set(forge::net::http::field::content_type, "application/json");
      if (!options.user_agent.empty()) {
         request.set(forge::net::http::field::user_agent, options.user_agent);
      }
      for (const auto& header : options.headers) {
         request.set(header.key, header.value);
      }
      request.prepare_payload();
      return request;
   }

   std::chrono::milliseconds retry_delay(std::uint32_t attempt) {
      auto delay = exponential_delay(options.retry, attempt);
      if (delay.count() <= 0) {
         return 0ms;
      }
      const auto jitter_bound = std::max<std::int64_t>(1, delay.count() / 10);
      const auto jitter = std::chrono::milliseconds{static_cast<std::int64_t>(jitter_source() % jitter_bound)};
      return cap_delay(delay + jitter, options.retry.max_delay);
   }

   awaitable<bool> sleep_before_retry(std::chrono::milliseconds delay) {
      if (delay.count() <= 0) {
         co_return !shutdown_expired();
      }
      if (shutdown_deadline.has_value()) {
         const auto now = std::chrono::steady_clock::now();
         if (now >= *shutdown_deadline) {
            co_return false;
         }
         const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*shutdown_deadline - now);
         if (remaining <= delay) {
            co_return false;
         }
      }
      auto timer = asio::steady_timer{strand};
      timer.expires_after(delay);
      co_await timer.async_wait(use_awaitable);
      co_return true;
   }

   std::chrono::milliseconds remaining_request_timeout() const {
      if (!shutdown_deadline.has_value()) {
         return options.request_timeout;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= *shutdown_deadline) {
         return 0ms;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*shutdown_deadline - now);
      return std::min(options.request_timeout, std::max(1ms, remaining));
   }

   bool shutdown_expired() const {
      return shutdown_deadline.has_value() && std::chrono::steady_clock::now() >= *shutdown_deadline;
   }

   std::string encode_logs(const std::vector<forge::log_record>& records) const {
      auto attributes = std::vector<otlp_attribute>{};
      attributes.reserve(options.resource.attributes.size());
      for (const auto& entry : options.resource.attributes) {
         attributes.push_back(make_string_attribute(entry.key, entry.value));
      }

      auto log_records = std::vector<otlp_log_record>{};
      log_records.reserve(records.size());
      for (const auto& record : records) {
         log_records.push_back(make_log_record(record));
      }

      auto request = otlp_export_request{
          .resourceLogs =
              {
                  otlp_resource_log{
                      .resource = otlp_resource{.attributes = std::move(attributes)},
                      .scopeLogs =
                          {
                              otlp_scope_log{
                                  .scope =
                                      otlp_scope{
                                          .name = std::string{instrumentation_scope_name},
                                          .version = std::string{instrumentation_scope_version},
                                      },
                                  .logRecords = std::move(log_records),
                              },
                          },
                  },
              },
      };

      auto encoded = forge::codec::json::write(request);
      if (!encoded.ok()) {
         const auto message = encoded.diagnostics.empty() ? std::string{"unknown JSON encoding error"}
                                                          : encoded.diagnostics.front().message;
         FORGE_THROW_EXCEPTION(exceptions::export_failed, "failed to encode OTLP logs",
                               forge::exceptions::ctx("reason", message));
      }
      return std::move(encoded.text);
   }

   void complete_waiters(std::exception_ptr error) {
      auto pending = std::move(waiters);
      waiters.clear();
      for (auto& waiter : pending) {
         waiter->error = error;
         waiter->done = true;
         waiter->timer.cancel();
      }
   }

   bool empty_queue() const {
      const auto lock = std::scoped_lock{mutex};
      return queue.empty();
   }

   bool closed_snapshot() const {
      const auto lock = std::scoped_lock{mutex};
      return closed;
   }

   void drop_all_queued() {
      const auto lock = std::scoped_lock{mutex};
      current_metrics.dropped_records += queue.size();
      queue.clear();
      queue_bytes = 0;
      refresh_queue_metrics_locked();
   }

   void record_export_attempt() {
      const auto lock = std::scoped_lock{mutex};
      ++current_metrics.export_attempts;
   }

   void record_retry() {
      const auto lock = std::scoped_lock{mutex};
      ++current_metrics.retry_attempts;
   }

   void record_exported(std::size_t count) {
      const auto lock = std::scoped_lock{mutex};
      current_metrics.exported_records += count;
      refresh_queue_metrics_locked();
   }

   void record_failed(std::size_t count) {
      const auto lock = std::scoped_lock{mutex};
      current_metrics.failed_records += count;
      current_metrics.dropped_records += count;
      refresh_queue_metrics_locked();
   }

   void record_direct_failed(std::size_t count) {
      const auto lock = std::scoped_lock{mutex};
      current_metrics.failed_records += count;
      refresh_queue_metrics_locked();
   }

   void refresh_queue_metrics_locked() const {
      current_metrics.queue_depth = queue.size();
      current_metrics.queue_bytes = queue_bytes;
   }

   exporter_metrics metrics() const {
      const auto lock = std::scoped_lock{mutex};
      auto snapshot = current_metrics;
      snapshot.queue_depth = queue.size();
      snapshot.queue_bytes = queue_bytes;
      return snapshot;
   }

   forge::asio::runtime& runtime;
   log_exporter_options options;
   asio::strand<asio::io_context::executor_type> strand;
   asio::steady_timer flush_timer;
   forge::net::http::base_url endpoint;
   std::unique_ptr<forge::net::http::client> client;

   mutable std::mutex mutex;
   mutable exporter_metrics current_metrics;
   std::deque<queued_record> queue;
   std::size_t queue_bytes = 0;
   bool closed = false;

   bool flush_active = false;
   bool timer_armed = false;
   std::optional<std::chrono::steady_clock::time_point> shutdown_deadline;
   std::vector<std::shared_ptr<flush_waiter>> waiters;
   std::minstd_rand jitter_source{0x0f7c10};
};

log_exporter::log_exporter(forge::asio::runtime& runtime, log_exporter_options options)
    : impl_(std::make_shared<impl>(runtime, std::move(options))) {}

log_exporter::~log_exporter() {
   if (impl_) {
      impl_->close_without_flush();
   }
}

bool log_exporter::enqueue(const forge::log_record& record) {
   return impl_->enqueue(record);
}

exporter_metrics log_exporter::metrics() const {
   return impl_->metrics();
}

boost::asio::awaitable<export_result> log_exporter::async_export(std::vector<forge::log_record> records) {
   co_return co_await impl_->export_now(std::move(records));
}

boost::asio::awaitable<void> log_exporter::async_flush() {
   co_await asio::post(impl_->strand, use_awaitable);
   co_await impl_->wait_for_flush();
}

boost::asio::awaitable<void> log_exporter::async_shutdown() {
   co_await impl_->shutdown();
}

} // namespace forge::otlp
