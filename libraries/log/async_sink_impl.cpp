module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.log.async_sink;

import forge.asio.compute;
import forge.asio.notification;
import forge.log.exceptions;

#include "details/async_sink_impl.hxx"

namespace forge {

thread_local bool async_sink::impl::_in_downstream = false;

async_sink::impl::impl(std::shared_ptr<sink> downstream, forge::asio::compute::executor executor,
                       async_sink_options options)
    : _downstream{std::move(downstream)}, _executor{std::move(executor)}, _options{options} {
   if (!_downstream || !_executor.valid() || _options.max_records == 0 || _options.max_bytes == 0) {
      throw log::exceptions::invalid_config{"async sink requires a downstream, compute executor and nonzero bounds"};
   }
}

async_sink::impl::~impl() {
   // Normally the pump emptied this list. An incorrectly stopped external pool
   // can discard its job; even then do not recursively destroy a long list.
   while (_head) {
      auto retired = std::move(_head);
      _head = std::move(retired->next);
   }
}

void async_sink::impl::increment(std::uint64_t& counter) noexcept {
   if (counter != std::numeric_limits<std::uint64_t>::max()) {
      ++counter;
   }
}

bool async_sink::impl::add_bytes(std::size_t& bytes, std::size_t amount, std::size_t limit) noexcept {
   if (bytes > limit || amount > limit - bytes) {
      return false;
   }
   bytes += amount;
   return true;
}

bool async_sink::impl::record_bytes(const log_record& record, bool use_capacity,
                                    std::size_t limit, std::size_t& bytes) noexcept {
   bytes = 0;
   auto string_bytes = [&](const std::string& value) {
      return add_bytes(bytes, use_capacity ? value.capacity() : value.size(), limit) &&
             add_bytes(bytes, 1, limit);
   };
   auto array_bytes = [&](std::size_t count, std::size_t element_size) {
      if (bytes > limit || count > (limit - bytes) / element_size) {
         return false;
      }
      return add_bytes(bytes, count * element_size, limit);
   };
   if (!add_bytes(bytes, sizeof(queued_record), limit) || !string_bytes(record.logger) ||
       !string_bytes(record.component) || !string_bytes(record.message) ||
       !string_bytes(record.thread_id) || !string_bytes(record.thread_name) ||
       !string_bytes(record.exception_chain) ||
       !array_bytes(use_capacity ? record.fields.capacity() : record.fields.size(), sizeof(log_field))) {
      return false;
   }
   for (const auto& field : record.fields) {
      if (!string_bytes(field.key)) {
         return false;
      }
      // The preflight measures the redacted copy, never the original secret.
      if (!use_capacity && field.redacted) {
         if (!add_bytes(bytes, sizeof("<redacted>"), limit)) {
            return false;
         }
      } else if (!string_bytes(field.value)) {
         return false;
      }
   }
   if (record.stacktrace) {
      const auto& trace = *record.stacktrace;
      if (!string_bytes(trace.backend) || !string_bytes(trace.unavailable_reason) ||
          !array_bytes(use_capacity ? trace.frames.capacity() : trace.frames.size(), sizeof(stacktrace_frame))) {
         return false;
      }
      for (const auto& frame : trace.frames) {
         if (!string_bytes(frame.name) || !string_bytes(frame.source_file)) {
            return false;
         }
      }
   }
   return true;
}

log_record async_sink::impl::copy_record(const log_record& record) {
   auto result = log_record{
       .level = record.level,
       .logger = record.logger,
       .component = record.component,
       .message = record.message,
       .fields = {},
       .timestamp = record.timestamp,
       .thread_id = record.thread_id,
       .thread_name = record.thread_name,
       .location = record.location,
       .stacktrace = record.stacktrace,
       .exception_chain = record.exception_chain,
   };
   result.fields.reserve(record.fields.size());
   for (const auto& field : record.fields) {
      result.fields.push_back({field.key, field.redacted ? "<redacted>" : field.value, field.redacted});
   }
   return result;
}

void async_sink::impl::enqueue(const log_record& record) noexcept {
   const auto lock = std::scoped_lock{_mutex};
   if (_in_downstream) {
      increment(_metrics.dropped);
      increment(_metrics.suppressed_reentrant);
      return;
   }
   if (_metrics.closed || _metrics.queued_records >= _options.max_records ||
       _metrics.accepted == std::numeric_limits<std::uint64_t>::max()) {
      increment(_metrics.dropped);
      return;
   }

   const auto available = _options.max_bytes - _metrics.queued_bytes;
   auto bytes = std::size_t{};
   if (!record_bytes(record, false, available, bytes)) {
      increment(_metrics.dropped);
      return;
   }

   try {
      // Copies stay inside bounded admission, rather than accumulating one
      // unaccounted payload per concurrent producer outside the queue lock.
      auto pending = std::make_unique<queued_record>();
      pending->record = copy_record(record);
      if (!record_bytes(pending->record, true, available, pending->bytes)) {
         increment(_metrics.dropped);
         return;
      }

      if (_pump_scheduled) {
         auto* tail = pending.get();
         bytes = pending->bytes;
         if (_tail) {
            _tail->next = std::move(pending);
         } else {
            _head = std::move(pending);
         }
         _tail = tail;
         ++_metrics.queued_records;
         _metrics.queued_bytes += bytes;
         ++_metrics.accepted;
         return;
      }

      // An idle sink has no accepted work. Install the first record provisionally
      // and commit its accounting only after the executor admits the ONE pump.
      // compute::try_submit posts (never invokes inline); drain takes this lock
      // before touching the record and calls downstream only after unlocking.
      if (_pump_generation == std::numeric_limits<std::uint64_t>::max()) {
         increment(_metrics.dropped);
         return;
      }
      _head = std::move(pending);
      _tail = _head.get();
      _pump_scheduled = true;
      ++_pump_generation;
      const auto generation = _pump_generation;
      try {
         auto submitted = _executor.try_submit({}, [owner = shared_from_this(), generation] {
            owner->drain(generation);
         });
         if (submitted) {
            ++_metrics.queued_records;
            _metrics.queued_bytes += _head->bytes;
            ++_metrics.accepted;
            return;
         }
      } catch (...) {
         // Rejected/throwing submission cannot leave an accepted queue without
         // a pump. A generation check also ignores any stale posted callback.
      }
      _head.reset();
      _tail = nullptr;
      _pump_scheduled = false;
      increment(_metrics.dropped);
   } catch (...) {
      // Allocation/copy failures are admission drops, not synchronous stderr I/O.
      increment(_metrics.dropped);
   }
}

void async_sink::impl::drain(std::uint64_t generation) noexcept {
   for (;;) {
      auto active = std::unique_ptr<queued_record>{};
      {
         const auto lock = std::scoped_lock{_mutex};
         if (!_pump_scheduled || generation != _pump_generation) {
            return;
         }
         if (!_head) {
            _pump_scheduled = false;
            return;
         }
         active = std::move(_head);
         _head = std::move(active->next);
         if (!_head) {
            _tail = nullptr;
         }
      }

      auto failed = false;
      const auto previous = _in_downstream;
      _in_downstream = true;
      try {
         _downstream->log(active->record);
      } catch (...) {
         failed = true;
      }
      _in_downstream = previous;

      const auto bytes = active->bytes;
      active.reset();
      {
         const auto lock = std::scoped_lock{_mutex};
         --_metrics.queued_records;
         _metrics.queued_bytes -= bytes;
         ++_completed;
         if (failed) {
            ++_metrics.failed;
         } else {
            ++_metrics.delivered;
         }
      }
      _changed.notify();
   }
}

void async_sink::impl::close() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   _metrics.closed = true;
}

async_sink_metrics async_sink::impl::snapshot() const noexcept {
   const auto lock = std::scoped_lock{_mutex};
   return _metrics;
}

std::uint64_t async_sink::impl::capture_fence(bool close_admission) noexcept {
   const auto lock = std::scoped_lock{_mutex};
   if (close_admission) {
      _metrics.closed = true;
   }
   return _metrics.accepted;
}

std::chrono::steady_clock::time_point async_sink::impl::deadline_after(
    std::chrono::milliseconds timeout) noexcept {
   const auto now = std::chrono::steady_clock::now();
   if (timeout <= std::chrono::milliseconds::zero()) {
      return now;
   }
   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::time_point::max() - now);
   if (timeout >= remaining) {
      return std::chrono::steady_clock::time_point::max();
   }
   return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
}

boost::asio::awaitable<bool> async_sink::impl::wait_fence(
    std::shared_ptr<impl> owner, std::uint64_t fence, std::chrono::steady_clock::time_point deadline) {
   for (;;) {
      // Observe first, then check progress: a notify between checking and
      // subscribing is remembered by notification's sticky epoch mechanism.
      const auto observed = owner->_changed.epoch();
      {
         const auto lock = std::scoped_lock{owner->_mutex};
         if (owner->_completed >= fence) {
            co_return true;
         }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         co_return false;
      }
      static_cast<void>(co_await owner->_changed.async_wait_until(observed, deadline));
   }
}

} // namespace forge
