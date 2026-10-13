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
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

module forge.otlp.log_exporter;

import forge.asio.runtime;
import forge.asio.notification;
import forge.exceptions;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.types;
import forge.codec.json;
import forge.log.record;

#include "details/otlp_export_request.hxx"
#include "details/log_exporter_impl.hxx"

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

log_exporter::impl::impl(forge::asio::runtime& runtime_value, log_exporter_options options_value)
    : runtime(runtime_value), options(std::move(options_value)), strand(asio::make_strand(runtime.context())),
      flush_timer(strand), shutdown_timer(strand) {
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

void log_exporter::impl::validate_options() const {
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

bool log_exporter::impl::enqueue(const forge::log_record& record) {
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

void log_exporter::impl::on_enqueue(bool should_flush) {
   if (closed_snapshot()) {
      return;
   }
   if (should_flush) {
      flush_timer.cancel();
      ++flush_timer_epoch;
      timer_armed = false;
      start_flush();
      return;
   }
   arm_flush_timer();
}

void log_exporter::impl::arm_flush_timer() {
   if (flush_active || timer_armed || closed_snapshot()) {
      return;
   }
   timer_armed = true;
   const auto epoch = ++flush_timer_epoch;
   flush_timer.expires_after(options.batch.flush_interval);
   flush_timer.async_wait([self = shared_from_this(), epoch](boost::system::error_code error) {
      if (epoch != self->flush_timer_epoch) {
         return;
      }
      self->timer_armed = false;
      if (!error && !self->closed_snapshot()) {
         self->start_flush();
      }
   });
}

void log_exporter::impl::start_flush() {
   if (flush_active || abandoned || empty_queue()) {
      return;
   }
   flush_active = true;
   auto operation = std::make_shared<export_operation>();
   flush_operation = operation;
   asio::co_spawn(strand, drain(), asio::bind_cancellation_slot(operation->cancellation.slot(),
       [self = shared_from_this(), operation](std::exception_ptr error) {
          self->flush_active = false;
          self->complete_operation(operation, error);
          if (error && self->shutdown_operation && !self->shutdown_expired()) {
             self->shutdown_error = error;
          }
          if (!self->empty_queue() && !self->closed_snapshot()) {
             self->arm_flush_timer();
          }
          self->finish_shutdown();
       }));
}

awaitable<void> log_exporter::impl::wait_for_flush() {
   if (shutdown_operation) {
      co_await wait_operation(shutdown_operation);
      co_return;
   }
   start_flush();
   if (flush_active) {
      auto operation = flush_operation;
      co_await wait_operation(std::move(operation));
   }
}

awaitable<void> log_exporter::impl::shutdown() {
   // Once initiated, cleanup is shared and cannot be abandoned by one caller.
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   if (!shutdown_operation) {
      begin_shutdown();
   }
   co_await wait_operation(shutdown_operation);
}

void log_exporter::impl::begin_shutdown() {
   {
      const auto lock = std::scoped_lock{mutex};
      closed = true;
   }
   shutdown_operation = std::make_shared<export_operation>();
   const auto now = std::chrono::steady_clock::now();
   const auto remaining = std::chrono::steady_clock::time_point::max() - now;
   shutdown_deadline = options.shutdown_timeout >= remaining
                           ? std::chrono::steady_clock::time_point::max()
                           : now + options.shutdown_timeout;
   flush_timer.cancel();
   ++flush_timer_epoch;
   timer_armed = false;
   shutdown_timer.expires_at(*shutdown_deadline);
   shutdown_timer.async_wait([self = shared_from_this()](boost::system::error_code error) {
      if (!error) {
         self->drop_all_queued();
         self->cancel_exports();
         self->finish_shutdown();
      }
   });
   start_flush();
   finish_shutdown();
}

void log_exporter::impl::finish_shutdown() {
   if (!shutdown_operation || shutdown_operation->done || flush_active || !direct_operations.empty()) {
      return;
   }
   drop_all_queued();
   shutdown_timer.cancel();
   {
      const auto lock = std::scoped_lock{mutex};
      shutdown_finished = true;
   }
   complete_operation(shutdown_operation, shutdown_error);
}

void log_exporter::impl::cancel_exports() {
   if (flush_active) {
      flush_operation->cancellation.emit(asio::cancellation_type::all);
   }
   // Emission may resume a coroutine inline; retain a stable owner snapshot.
   const auto pending = direct_operations;
   for (const auto& operation : pending) {
      operation->cancellation.emit(asio::cancellation_type::all);
   }
}

void log_exporter::impl::close_without_flush() {
   {
      const auto lock = std::scoped_lock{mutex};
      closed = true;
      if (shutdown_finished) {
         return;
      }
   }
   asio::post(strand, [self = shared_from_this()] {
      self->abandoned = true;
      self->flush_timer.cancel();
      ++self->flush_timer_epoch;
      self->timer_armed = false;
      self->drop_all_queued();
      self->cancel_exports();
      self->finish_shutdown();
   });
}

awaitable<void> log_exporter::impl::drain() {
   for (;;) {
      if (shutdown_expired()) {
         drop_all_queued();
         co_return;
      }

      auto batch = take_batch();
      if (batch.empty()) {
         co_return;
      }

      auto exported = false;
      try {
         exported = co_await export_batch(batch);
      } catch (...) {
         record_failed(batch.size());
         throw;
      }
      if (exported) {
         record_exported(batch.size());
      } else {
         record_failed(batch.size());
      }
   }
}

awaitable<export_result> log_exporter::impl::export_now(std::vector<forge::log_record> records) {
   if (closed_snapshot()) {
      FORGE_THROW_EXCEPTION(exceptions::queue_closed, "OTLP exporter is closed");
   }
   auto operation = std::make_shared<export_operation>();
   operation->result.submitted_records = records.size();
   if (records.empty()) {
      co_return operation->result;
   }
   const auto count = records.size();
   direct_operations.push_back(operation);
   asio::co_spawn(strand, export_records(std::move(records)),
       asio::bind_cancellation_slot(operation->cancellation.slot(),
           [self = shared_from_this(), operation, count](std::exception_ptr error, bool exported) {
              if (!error && exported) {
                 self->record_exported(count);
                 operation->result.exported_records = count;
              } else {
                 self->record_direct_failed(count);
                 operation->result.failed_records = count;
              }
              std::erase(self->direct_operations, operation);
              self->complete_operation(operation, error);
              self->finish_shutdown();
           }));
   auto cancel_pending = boost::scope::scope_exit{[operation] {
      if (!operation->done) {
         operation->cancellation.emit(asio::cancellation_type::all);
      }
   }};
   co_await wait_operation(operation);
   co_return operation->result;
}

awaitable<bool> log_exporter::impl::export_records(std::vector<forge::log_record> records) {
   co_return co_await export_batch(records);
}

awaitable<void> log_exporter::impl::wait_operation(std::shared_ptr<export_operation> operation) {
   while (!operation->done) {
      const auto observed = operation->changed.epoch();
      if (!operation->done) {
         co_await operation->changed.async_wait(observed);
      }
   }
   if (operation->error) {
      std::rethrow_exception(operation->error);
   }
}

void log_exporter::impl::complete_operation(const std::shared_ptr<export_operation>& operation, std::exception_ptr error) {
   operation->error = error;
   operation->done = true;
   operation->changed.notify();
}

awaitable<void> log_exporter::impl::flush_owned(std::shared_ptr<impl> owner) {
   co_await owner->wait_for_flush();
}

awaitable<void> log_exporter::impl::shutdown_owned(std::shared_ptr<impl> owner) {
   co_await owner->shutdown();
}

awaitable<export_result> log_exporter::impl::export_owned(std::shared_ptr<impl> owner, std::vector<forge::log_record> records) {
   co_return co_await owner->export_now(std::move(records));
}

std::vector<forge::log_record> log_exporter::impl::take_batch() {
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

awaitable<bool> log_exporter::impl::export_batch(const std::vector<forge::log_record>& records) {
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
         // HTTP installs a request-specific cancellation filter. Give it a
         // separate coroutine chain so the following retry timer stays cancellable.
         auto response = co_await asio::co_spawn(
             strand, client->async_request(std::move(request), {.timeout = timeout}), use_awaitable);
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
         if (shutdown_expired() || attempt >= options.retry.max_attempts) {
            co_return false;
         }
         delay = retry_delay(attempt);
         should_retry = true;
      }

      if (should_retry) {
         const auto cancellation = co_await asio::this_coro::cancellation_state;
         if (cancellation.cancelled() != asio::cancellation_type::none) {
            co_return false;
         }
         ++attempt;
         record_retry();
         if (!co_await sleep_before_retry(delay)) {
            co_return false;
         }
      }
   }
}

forge::net::http::request log_exporter::impl::make_request(const std::string& payload) const {
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

std::chrono::milliseconds log_exporter::impl::retry_delay(std::uint32_t attempt) {
   auto delay = exponential_delay(options.retry, attempt);
   if (delay.count() <= 0) {
      return 0ms;
   }
   const auto jitter_bound = std::max<std::int64_t>(1, delay.count() / 10);
   const auto jitter = std::chrono::milliseconds{static_cast<std::int64_t>(jitter_source() % jitter_bound)};
   return cap_delay(delay + jitter, options.retry.max_delay);
}

awaitable<bool> log_exporter::impl::sleep_before_retry(std::chrono::milliseconds delay) {
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

std::chrono::milliseconds log_exporter::impl::remaining_request_timeout() const {
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

bool log_exporter::impl::shutdown_expired() const {
   return abandoned || (shutdown_deadline.has_value() && std::chrono::steady_clock::now() >= *shutdown_deadline);
}

std::string log_exporter::impl::encode_logs(const std::vector<forge::log_record>& records) const {
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



bool log_exporter::impl::empty_queue() const {
   const auto lock = std::scoped_lock{mutex};
   return queue.empty();
}

bool log_exporter::impl::closed_snapshot() const {
   const auto lock = std::scoped_lock{mutex};
   return closed;
}

void log_exporter::impl::drop_all_queued() {
   const auto lock = std::scoped_lock{mutex};
   current_metrics.dropped_records += queue.size();
   queue.clear();
   queue_bytes = 0;
   refresh_queue_metrics_locked();
}

void log_exporter::impl::record_export_attempt() {
   const auto lock = std::scoped_lock{mutex};
   ++current_metrics.export_attempts;
}

void log_exporter::impl::record_retry() {
   const auto lock = std::scoped_lock{mutex};
   ++current_metrics.retry_attempts;
}

void log_exporter::impl::record_exported(std::size_t count) {
   const auto lock = std::scoped_lock{mutex};
   current_metrics.exported_records += count;
   refresh_queue_metrics_locked();
}

void log_exporter::impl::record_failed(std::size_t count) {
   const auto lock = std::scoped_lock{mutex};
   current_metrics.failed_records += count;
   current_metrics.dropped_records += count;
   refresh_queue_metrics_locked();
}

void log_exporter::impl::record_direct_failed(std::size_t count) {
   const auto lock = std::scoped_lock{mutex};
   current_metrics.failed_records += count;
   refresh_queue_metrics_locked();
}

void log_exporter::impl::refresh_queue_metrics_locked() const {
   current_metrics.queue_depth = queue.size();
   current_metrics.queue_bytes = queue_bytes;
}

exporter_metrics log_exporter::impl::metrics() const {
   const auto lock = std::scoped_lock{mutex};
   auto snapshot = current_metrics;
   snapshot.queue_depth = queue.size();
   snapshot.queue_bytes = queue_bytes;
   return snapshot;
}


} // namespace forge::otlp
