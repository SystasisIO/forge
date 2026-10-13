module;

#include <boost/asio/awaitable.hpp>

#include <utility>

module forge.plugins.log.otlp.api;

namespace forge::plugins::log::otlp {

boost::asio::awaitable<void> api::discard_flush_result(boost::asio::awaitable<flush_result> operation) {
   static_cast<void>(co_await std::move(operation));
}

} // namespace forge::plugins::log::otlp
