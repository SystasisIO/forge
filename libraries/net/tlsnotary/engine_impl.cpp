module;

#include <boost/asio/awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.net.tlsnotary.engine;

import forge.net.transport.stream;
import forge.asio.child_signal_registration;
import forge.asio.gate;
import forge.asio.notification;
#include "details/native_exchange.hxx"
#include "details/engine_impl.hxx"
#include "details/engine_impl_entry.hxx"

namespace forge::net::tlsnotary {

engine::impl::impl(forge::asio::runtime& runtime_value, forge::asio::compute::executor compute_value,
                   engine_options options_value)
    : runtime{runtime_value}, compute{std::move(compute_value)}, options{std::move(options_value)} {
   if (!compute.valid() || !options.worker_path.is_absolute() || options.max_active == 0 || options.max_active > 64 ||
       options.max_queued > 1024)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "TLSNotary requires explicit worker, compute executor and bounded admission");
}

boost::asio::awaitable<void> engine::impl::check(boost::asio::awaitable<result> operation) {
   static_cast<void>(co_await std::move(operation));
}

boost::asio::awaitable<result>
engine::impl::execute(std::shared_ptr<impl> self,
                      boost::compat::move_only_function<std::shared_ptr<detail::native_exchange>()> make_exchange,
                      std::uint64_t id, std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   auto record = std::make_shared<entry>();
   record->id = id;
   auto active = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->counters.stopped)
         FORGE_THROW_EXCEPTION(exceptions::stopped, "TLSNotary admission is stopped");
      if (std::chrono::steady_clock::now() >= deadline)
         FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "TLSNotary admission deadline exceeded");
      for (const auto& item : self->active)
         if (item->id == record->id)
            FORGE_THROW_EXCEPTION(exceptions::invalid_request, "duplicate live session identity");
      for (const auto& item : self->queue)
         if (item->id == record->id)
            FORGE_THROW_EXCEPTION(exceptions::invalid_request, "duplicate live session identity");
      if (self->queue.empty() && self->active.size() < self->options.max_active) {
         self->active.push_back(record);
         active = true;
      } else {
         if (self->queue.size() >= self->options.max_queued) {
            ++self->counters.rejected;
            FORGE_THROW_EXCEPTION(exceptions::overloaded, "TLSNotary admission limit reached");
         }
         self->queue.push_back(record);
      }
      ++self->counters.accepted;
   }

   auto success = false;
   const auto release = boost::scope::scope_exit{[&] {
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (record->exchange && !record->exchange->quiescent()) {
            record->retired = true;
            self->counters.stopped = true;
         } else {
            std::erase(self->queue, record);
            std::erase(self->active, record);
         }

         if (success)
            ++self->counters.completed;
         else
            ++self->counters.failed;
      }

      self->changed.notify();
   }};
   auto failure = std::exception_ptr{};
   auto value = result{};
   try {
      auto exchange = make_exchange();
      {
         const auto lock = std::scoped_lock{self->mutex};
         record->exchange = std::move(exchange);
      }

      const auto cancel = std::stop_callback{stop, [record, self] {
                                                record->stop.request_stop();
                                                record->exchange->request_stop();
                                                self->changed.notify();
                                             }};
      if (record->stop.stop_requested())
         record->exchange->request_stop();
      while (!active) {
         const auto epoch = self->changed.epoch();
         {
            const auto lock = std::scoped_lock{self->mutex};
            if (self->counters.stopped)
               FORGE_THROW_EXCEPTION(exceptions::stopped, "TLSNotary session stopped before launch");
            if (record->stop.stop_requested())
               FORGE_THROW_EXCEPTION(exceptions::cancelled, "TLSNotary session cancelled before launch");
            if (std::chrono::steady_clock::now() >= deadline)
               FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "TLSNotary queue deadline exceeded");
            if (self->queue.front() == record && self->active.size() < self->options.max_active) {
               self->active.push_back(record);
               self->queue.pop_front();
               active = true;
            }
         }

         if (!active) {
            try {
               static_cast<void>(co_await self->changed.async_wait_until(epoch, deadline));
            } catch (...) {
               if (std::chrono::steady_clock::now() >= deadline)
                  FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "TLSNotary queue deadline exceeded");
               throw;
            }
         }
      }

      self->changed.notify();
      if (record->stop.stop_requested())
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "TLSNotary session cancelled before launch");
      if (std::chrono::steady_clock::now() >= deadline)
         FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "TLSNotary admission deadline exceeded");
      {
         const auto lock = std::scoped_lock{self->signal_mutex};
         if (!self->signal_owner.registered()) {
            try {
               self->signal_owner.acquire(self->runtime.context().get_executor());
            } catch (...) {
               FORGE_THROW_EXCEPTION(exceptions::worker_failed, "TLSNotary child signal registration failed");
            }
         }
      }

      value = co_await detail::native_exchange::run(record->exchange);
   } catch (...) {
      failure = std::current_exception();
   }

   if (failure) {
      if (record->exchange)
         co_await detail::native_exchange::discard(record->exchange);
      std::rethrow_exception(failure);
   }

   success = true;
   co_return value;
}

bool engine::impl::cancel(std::uint64_t id) noexcept {
   auto target = std::shared_ptr<entry>{};
   auto exchange = std::shared_ptr<detail::native_exchange>{};
   {
      const auto lock = std::scoped_lock{mutex};
      for (const auto& item : active)
         if (item->id == id)
            target = item;
      for (const auto& item : queue)
         if (item->id == id)
            target = item;
      if (target)
         exchange = target->exchange;
   }

   if (!target)
      return false;
   target->stop.request_stop();
   if (exchange)
      exchange->request_stop();
   changed.notify();
   return true;
}

void engine::impl::request_stop() noexcept {
   std::array<std::shared_ptr<entry>, 1088> records{};
   std::array<std::shared_ptr<detail::native_exchange>, 1088> exchanges{};
   auto count = std::size_t{};
   {
      const auto lock = std::scoped_lock{mutex};
      counters.stopped = true;
      for (const auto& item : active) {
         records[count] = item;
         exchanges[count++] = item->exchange;
      }

      for (const auto& item : queue) {
         records[count] = item;
         exchanges[count++] = item->exchange;
      }
   }

   // Caller hooks run without admission or publication locks.
   for (std::size_t n = 0; n < count; ++n) {
      records[n]->stop.request_stop();
      if (exchanges[n])
         exchanges[n]->request_stop();
   }

   changed.notify();
}

boost::asio::awaitable<void> engine::impl::drain(std::shared_ptr<impl> self) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   self->request_stop();
   for (;;) {
      const auto epoch = self->changed.epoch();
      auto retained = std::shared_ptr<entry>{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (self->active.empty() && self->queue.empty())
            break;
         for (const auto& record : self->active)
            if (record->retired) {
               retained = record;
               break;
            }

         if (!retained)
            for (const auto& record : self->queue)
               if (record->retired) {
                  retained = record;
                  break;
               }
      }

      if (retained) {
         co_await detail::native_exchange::discard(retained->exchange);
         if (!retained->exchange->quiescent())
            FORGE_THROW_EXCEPTION(exceptions::worker_failed, "TLSNotary child cleanup remains incomplete");
         {
            const auto lock = std::scoped_lock{self->mutex};
            std::erase(self->active, retained);
            std::erase(self->queue, retained);
         }

         self->changed.notify();
      } else
         static_cast<void>(co_await self->changed.async_wait(epoch));
   }

   const auto lock = std::scoped_lock{self->signal_mutex};
   try {
      self->signal_owner.release();
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "TLSNotary child signal release failed");
   }
}

metrics engine::impl::snapshot() const {
   const auto lock = std::scoped_lock{mutex};
   auto result = counters;
   result.active = active.size();
   result.queued = queue.size();
   return result;
}
} // namespace forge::net::tlsnotary
