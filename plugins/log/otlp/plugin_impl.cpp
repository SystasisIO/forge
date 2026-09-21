module;

#include <boost/asio/awaitable.hpp>

module forge.plugins.log.otlp.plugin;

import forge.crypto.core.secret_bytes;
import forge.log.logger;
import forge.otlp.crash;
import forge.otlp.log_exporter;
import forge.otlp.log_sink;
import forge.plugins.crypto.secrets.api;
import forge.plugins.log.otlp.exceptions;
import forge.plugins.log.otlp.types;

#include "details/plugin_impl.hxx"

namespace forge::plugins::log::otlp {

bool plugin::impl::available() const noexcept {
   return static_cast<bool>(exporter);
}

metrics plugin::impl::current_metrics() const {
   if (!exporter) {
      throw exceptions::exporter_unavailable{"OTLP logs exporter is not available"};
   }
   const auto snapshot = exporter->metrics();
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

boost::asio::awaitable<void> plugin::impl::flush() {
   if (!exporter) {
      throw exceptions::exporter_unavailable{"OTLP logs exporter is not available"};
   }
   co_await exporter->async_flush();
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
