module;

#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>

#include <memory>
#include <utility>

module forge.plugins.log.otlp.plugin;

import forge.asio.runtime;
import forge.asio.notification;
import forge.crypto.core.secret_bytes;
import forge.log.logger;
import forge.otlp.crash;
import forge.otlp.log_exporter;
import forge.otlp.log_sink;
import forge.plugins.log.otlp.api;
import forge.plugins.log.otlp.types;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::log::otlp {

plugin::api_impl::api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<flush_result> plugin::api_impl::flush(flush_request) {
   return flush_owned(state_);
}

boost::asio::awaitable<flush_result> plugin::api_impl::flush_owned(std::shared_ptr<impl> state) {
   auto exporter = state->admit_exporter();
   auto release = boost::scope::scope_exit{[&exporter, state] {
      // Release the runtime owner before advertising that the caller is joined.
      exporter.reset();
      state->release_call();
   }};
   co_await exporter->async_flush();
   co_return flush_result{};
}

boost::asio::awaitable<::forge::plugins::log::otlp::metrics> plugin::api_impl::metrics(metrics_request) {
   return metrics_owned(state_);
}

boost::asio::awaitable<::forge::plugins::log::otlp::metrics>
plugin::api_impl::metrics_owned(std::shared_ptr<impl> state) {
   auto exporter = state->admit_exporter();
   auto release = boost::scope::scope_exit{[&exporter, state] {
      exporter.reset();
      state->release_call();
   }};
   co_return impl::current_metrics(exporter->metrics());
}

} // namespace forge::plugins::log::otlp
