module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <atomic>
#include <functional>
#include <array>
#include <chrono>
#include <exception>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

module forge.plugins.chain.outbox.plugin;

import forge.api.chain.outbox;
import forge.api.core.exceptions;
import forge.asio.cancellation_scope;
import forge.asio.notification;
import forge.asio.task;
import forge.chain.protocol.info;
import forge.chain.transaction.types;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.transaction_query;
import forge.exceptions;
import forge.log.logger;

import forge.net.http.base_url;

#include "details/plugin_impl.hxx"

namespace forge::plugins::chain::outbox {

plugin::impl::impl(plugin_options value) : options{std::move(value)} {}

void plugin::impl::require_running() const {
   if (!running || stop.stop_requested()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain outbox is stopped");
   }
}

void plugin::impl::begin_call() {
   const auto lock = std::scoped_lock{calls_mutex};
   require_running();
   ++calls;
}

void plugin::impl::end_call() noexcept {
   const auto lock = std::scoped_lock{calls_mutex};
   if (--calls == 0) {
      calls_changed.notify();
   }
}

boost::asio::awaitable<void> plugin::impl::drain() {
   for (;;) {
      const auto epoch = calls_changed.epoch();
      {
         const auto lock = std::scoped_lock{calls_mutex};
         if (calls == 0) {
            co_return;
         }
      }
      co_await calls_changed.async_wait(epoch);
   }
}

boost::asio::awaitable<void> plugin::impl::recover(forge::asio::task::context& task) {
   auto wake = forge::asio::notification{};
   const auto sources = std::array{stop.get_token(), task.stop_token()};
   const auto parent = co_await boost::asio::this_coro::cancellation_state;
   auto cancellation = forge::asio::cancellation_scope{co_await boost::asio::this_coro::executor, sources, parent};
   co_await cancellation.run<void>([this, &wake](std::stop_token cancel) -> boost::asio::awaitable<void> {
      while (!cancel.stop_requested()) {
         try {
            co_await queue->recover(cancel);
         } catch (const std::exception&) {
            if (!cancel.stop_requested()) {
               wlog("forge.plugins.chain.outbox", "Outbox recovery pass failed");
            }
         }
         if (!cancel.stop_requested()) {
            co_await wake.async_wait_until(wake.epoch(), std::chrono::steady_clock::now() + options.delivery.poll_interval,
                                          cancel);
         }
      }
   });
}

} // namespace forge::plugins::chain::outbox
