module;

#include <boost/asio/awaitable.hpp>

module forge.plugins.log.otlp.plugin;

import forge.crypto.core.secret_bytes;
import forge.asio.notification;
import forge.log.logger;
import forge.otlp.crash;
import forge.otlp.log_exporter;
import forge.otlp.log_sink;
import forge.plugins.crypto.secrets.api;
import forge.plugins.log.otlp.exceptions;
import forge.plugins.log.otlp.types;

#include "details/plugin_impl.hxx"

namespace forge::plugins::log::otlp {
plugin::impl::impl(forge::app::plugin_lifecycle value) : lifecycle{std::move(value)} {}


std::shared_ptr<forge::otlp::log_exporter> plugin::impl::admit_exporter() {
   const auto generation = lifecycle.snapshot().generation;
   auto admitted = std::shared_ptr<forge::otlp::log_exporter>{};
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{admission_mutex};
      if (!exporter) {
         throw exceptions::exporter_unavailable{"OTLP logs exporter is not available"};
      }
      admitted = exporter;
      ++admitted_calls;
   })) {
      throw exceptions::exporter_unavailable{"OTLP logs exporter is not available"};
   }
   return admitted;
}

void plugin::impl::release_call() noexcept {
   {
      const auto lock = std::scoped_lock{admission_mutex};
      --admitted_calls;
   }
   calls_changed.notify();
}

boost::asio::awaitable<void> plugin::impl::wait_for_calls() {
   for (;;) {
      const auto observed = calls_changed.epoch();
      {
         const auto lock = std::scoped_lock{admission_mutex};
         if (admitted_calls == 0) {
            co_return;
         }
      }
      co_await calls_changed.async_wait(observed);
   }
}

void plugin::impl::publish_exporter(std::shared_ptr<forge::otlp::log_exporter> value) {
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{admission_mutex};
      exporter.swap(value);
   })) {
      throw exceptions::exporter_unavailable{"OTLP logs exporter is stopping"};
   }
}

std::shared_ptr<forge::otlp::log_exporter> plugin::impl::take_exporter() noexcept {
   const auto lock = std::scoped_lock{admission_mutex};
   return std::exchange(exporter, {});
}

void plugin::impl::clear_exporter() noexcept {
   auto retired = std::shared_ptr<forge::otlp::log_exporter>{};
   {
      const auto lock = std::scoped_lock{admission_mutex};
      retired.swap(exporter);
   }
}

metrics plugin::impl::current_metrics(const forge::otlp::exporter_metrics& snapshot) {
   return metrics{
       .enqueued_records = snapshot.enqueued_records,
       .exported_records = snapshot.exported_records,
       .failed_records = snapshot.failed_records,
       .dropped_records = snapshot.dropped_records,
       .retry_attempts = snapshot.retry_attempts,
       .batches = snapshot.export_attempts,
       .queue_bytes = static_cast<std::uint64_t>(snapshot.queue_bytes),
       .queue_records = static_cast<std::uint64_t>(snapshot.queue_depth),
   };
}

void plugin::impl::detach_sink() noexcept {
   if (!sink) {
      attached_loggers.clear();
      return;
   }
   for (auto& attached : attached_loggers) {
      try {
         attached.logger.remove_sink(sink);
         forge::logger::update(attached.name, attached.logger);
      } catch (...) {
      }
   }
   attached_loggers.clear();
}

} // namespace forge::plugins::log::otlp
