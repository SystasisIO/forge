module;

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <utility>

module forge.plugins.log.otlp.plugin;

import forge.asio.runtime;
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
   co_await state_->flush();
   co_return flush_result{};
}

boost::asio::awaitable<::forge::plugins::log::otlp::metrics> plugin::api_impl::metrics(metrics_request) {
   co_return state_->current_metrics();
}

} // namespace forge::plugins::log::otlp
