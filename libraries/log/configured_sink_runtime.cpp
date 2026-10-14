module;

#include <boost/asio/awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

module forge.log.logger_config;

import forge.asio.compute;
import forge.asio.notification;
import forge.log.async_sink;
import forge.log.exceptions;
import forge.variant.value;

#include "details/configured_sink_runtime.hxx"

namespace forge::detail {
namespace {

// Trivial process state remains valid for log attempts from late destructors.
constinit std::atomic<bool> delivery_terminated{false};

std::chrono::steady_clock::time_point deadline_after(std::chrono::milliseconds timeout) noexcept {
   const auto now = std::chrono::steady_clock::now();
   if (timeout <= std::chrono::milliseconds::zero()) {
      return now;
   }
   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::time_point::max() - now);
   return timeout >= remaining ? std::chrono::steady_clock::time_point::max() : now + timeout;
}

std::chrono::milliseconds remaining_timeout(std::chrono::steady_clock::time_point deadline) noexcept {
   const auto now = std::chrono::steady_clock::now();
   return deadline <= now ? std::chrono::milliseconds::zero()
                         : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

void accumulate(async_sink_metrics& into, const async_sink_metrics& value) {
   into.accepted += value.accepted;
   into.delivered += value.delivered;
   into.dropped += value.dropped;
   into.failed += value.failed;
   into.suppressed_reentrant += value.suppressed_reentrant;
   into.queued_records += value.queued_records;
   into.queued_bytes += value.queued_bytes;
}

std::shared_ptr<sink> make_output(const sink_config& config) {
   const auto args = config.args.is_null() ? variant_object{} : config.args.get_object();
   if (config.type == "console") {
      auto stream = console_stream::by_level;
      if (args.contains("stream")) {
         const auto name = args["stream"].as_string();
         stream = name == "std_error" ? console_stream::standard_error
                  : name == "std_out" ? console_stream::standard_out : console_stream::by_level;
      }
      return std::make_shared<console_sink>(stream);
   }
   const auto path = std::filesystem::path{args["path"].as_string()};
   const auto append = !args.contains("append") || args["append"].as_bool();
   if (config.type == "jsonl") {
      return std::make_shared<jsonl_sink>(path, append);
   }
   return std::make_shared<file_sink>(path, append);
}

boost::asio::awaitable<bool> wait_all(std::vector<boost::asio::awaitable<bool>> waits) {
   for (auto& wait : waits) {
      if (!co_await std::move(wait)) {
         co_return false;
      }
   }
   co_return true;
}

} // namespace

configured_sink_runtime::configured_sink_runtime()
    : _workers{{.worker_threads = 1, .max_pending_tasks = max_retained_sinks,
                .max_waiting_submissions = 0, .thread_name = "forge-log"}} {}

configured_sink_runtime::~configured_sink_runtime() {
   delivery_terminated.store(true, std::memory_order_release);
   for (const auto& value : _sinks) {
      value->close();
   }
   // The compute owner joins below. Explicit shutdown is required to drain;
   // fallback may drop pending jobs and cannot kill an in-progress callback.
}

configured_sink_runtime& configured_sink_runtime::get() {
   static auto runtime = configured_sink_runtime{};
   return runtime;
}

bool configured_sink_runtime::terminated() noexcept {
   return delivery_terminated.load(std::memory_order_acquire);
}

configured_sink_runtime::configuration_operation
configured_sink_runtime::begin_configuration(const std::vector<sink_config>& config) {
   const auto count = static_cast<std::size_t>(std::ranges::count_if(config, &sink_config::asynchronous));
   return configuration_operation{*this, count};
}

configured_sink_runtime::configuration_operation::configuration_operation(
    configured_sink_runtime& owner, std::size_t count)
    : _owner{owner}, _reserved{count} {
   auto retired = std::vector<std::shared_ptr<async_sink>>{};
   {
      const auto lock = std::scoped_lock{_owner._mutex};
      if (_owner._stopping) {
         throw log::exceptions::invalid_config{"configured log delivery is closed"};
      }
      if (_owner._active_configurations == std::numeric_limits<std::size_t>::max()) {
         throw log::exceptions::invalid_config{"too many logging configuration operations"};
      }
      // No ownership or accounting changes before this allocation succeeds.
      retired.reserve(_owner._sinks.size());
      auto out = _owner._sinks.begin();
      for (auto in = _owner._sinks.begin(); in != _owner._sinks.end(); ++in) {
         if (in->use_count() == 1 && (*in)->snapshot().queued_records == 0) {
            (*in)->close();
            accumulate(_owner._retired, (*in)->snapshot());
            retired.push_back(std::move(*in));
         } else {
            if (out != in) {
               *out = std::move(*in);
            }
            ++out;
         }
      }
      _owner._sinks.erase(out, _owner._sinks.end());
      if (count > max_retained_sinks - _owner._sinks.size() - _owner._reserved) {
         throw log::exceptions::invalid_config{"configured log sink retention limit reached"};
      }
      _owner._reserved += count;
      ++_owner._active_configurations;
   }
   // Slow output teardown is still covered by this admitted operation, but
   // never holds the lifecycle lock.
   retired.clear();
}

configured_sink_runtime::configuration_operation::~configuration_operation() {
   // Rollback must release outputs BEFORE publishing operation completion.
   _prepared.clear();
   _owner.finish_configuration(_reserved);
}

void configured_sink_runtime::finish_configuration(std::size_t reserved) noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      _reserved -= reserved;
      --_active_configurations;
   }
   _changed.notify();
}

std::unordered_map<std::string, std::shared_ptr<sink>>
configured_sink_runtime::configuration_operation::prepare(const std::vector<sink_config>& config) {
   auto result = std::unordered_map<std::string, std::shared_ptr<sink>>{};
   _prepared.reserve(_reserved);
   for (const auto& value : config) {
      {
         const auto lock = std::scoped_lock{_owner._mutex};
         if (_owner._stopping) {
            throw log::exceptions::invalid_config{"configured log delivery is closed"};
         }
      }
      auto output = make_output(value);
      if (value.asynchronous) {
         auto adapter = std::make_shared<async_sink>(std::move(output), _owner._workers.get_executor(),
             async_sink_options{.max_records = value.max_records, .max_bytes = value.max_bytes});
         output = adapter;
         _prepared.push_back(std::move(adapter));
      }
      result.emplace(value.name, std::move(output));
   }
   return result;
}

std::unique_lock<std::mutex>
configured_sink_runtime::configuration_operation::acquire_publication() {
   auto lock = std::unique_lock{_owner._mutex};
   if (_owner._stopping || _published) {
      throw log::exceptions::invalid_config{"configured log publication is closed"};
   }
   // After reserve, shared_ptr insertion cannot throw or leave moved-from slots.
   _owner._sinks.reserve(_owner._sinks.size() + _prepared.size());
   _owner._sinks.insert(_owner._sinks.end(), _prepared.begin(), _prepared.end());
   _owner._reserved -= _reserved;
   _reserved = 0;
   _published = true;
   return lock;
}

boost::asio::awaitable<bool> configured_sink_runtime::flush(std::chrono::milliseconds timeout) {
   const auto deadline = deadline_after(timeout);
   auto waits = std::vector<boost::asio::awaitable<bool>>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      waits.reserve(_sinks.size());
      for (const auto& value : _sinks) {
         waits.push_back(value->async_flush(remaining_timeout(deadline)));
      }
   }
   return wait_all(std::move(waits));
}

boost::asio::awaitable<bool> configured_sink_runtime::shutdown(std::chrono::milliseconds timeout) {
   const auto deadline = deadline_after(timeout);
   auto waits = std::vector<boost::asio::awaitable<bool>>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      _stopping = true;
      for (const auto& value : _sinks) {
         value->close();
      }
      waits.reserve(_sinks.size());
      for (const auto& value : _sinks) {
         waits.push_back(value->async_shutdown(remaining_timeout(deadline)));
      }
   }
   return shutdown_owned(this, std::move(waits), deadline);
}

boost::asio::awaitable<bool> configured_sink_runtime::shutdown_owned(
    configured_sink_runtime* owner, std::vector<boost::asio::awaitable<bool>> waits,
    std::chrono::steady_clock::time_point deadline) {
   for (;;) {
      const auto observed = owner->_changed.epoch();
      {
         const auto lock = std::scoped_lock{owner->_mutex};
         if (owner->_active_configurations == 0) {
            break;
         }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         co_return false;
      }
      static_cast<void>(co_await owner->_changed.async_wait_until(observed, deadline));
   }
   // Each sink wait already owns the acceptance fence and deadline captured
   // by the synchronous shutdown facade. Preparation cannot extend its budget.
   if (!co_await wait_all(std::move(waits))) {
      co_return false;
   }
   co_await owner->_workers.shutdown();
   co_return true;
}

async_sink_metrics configured_sink_runtime::snapshot() {
   const auto lock = std::scoped_lock{_mutex};
   auto result = _retired;
   result.closed = _stopping;
   for (const auto& value : _sinks) {
      accumulate(result, value->snapshot());
   }
   return result;
}

} // namespace forge::detail
