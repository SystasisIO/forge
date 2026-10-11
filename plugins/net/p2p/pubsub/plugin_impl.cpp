module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <stop_token>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.types;

#include "details/project_message.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

bool plugin::impl::try_begin_handler() {
   const auto lock = std::scoped_lock{mutex};
   if (stopping || active_handlers >= settings.max_active_handlers) {
      ++messages_dropped;
      return false;
   }
   ++active_handlers;
   return true;
}

void plugin::impl::finish_handler() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      --active_handlers;
   }
   changed.notify();
}

void plugin::impl::record_handler_failure() {
   const auto lock = std::scoped_lock{mutex};
   ++handler_failures;
}

boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
plugin::impl::call_handler(std::shared_ptr<handler_record> handler, message value) {
   if (!try_begin_handler()) { co_return forge::net::p2p::pubsub::validation_result::retry; }
   auto completed = boost::scope::scope_exit{[&] { finish_handler(); }};
   try {
      if (handler->deadline.count() <= 0) {
         co_return co_await handler->callback(std::move(value));
      }
      const auto executor = co_await boost::asio::this_coro::executor;
      const auto deadline = handler->deadline;
      const auto self = shared_from_this();
      auto guarded_handler =
          [self, handler = std::move(handler), value = std::move(value)]() mutable
          -> boost::asio::awaitable<std::optional<forge::net::p2p::pubsub::validation_result>> {
         try { co_return co_await handler->callback(std::move(value)); }
         catch (...) {
            self->record_handler_failure();
            co_return std::nullopt;
         }
      };
      auto timer = boost::asio::steady_timer{executor};
      timer.expires_after(deadline);
      using namespace boost::asio::experimental::awaitable_operators;
      // The operator joins the losing operation; a deadline is not permission to detach user code.
      auto result = co_await (guarded_handler() || timer.async_wait(boost::asio::use_awaitable));
      if (result.index() == 0) {
         const auto& value = std::get<0>(result);
         co_return value.value_or(forge::net::p2p::pubsub::validation_result::retry);
      }
      record_handler_failure();
   } catch (...) {
      record_handler_failure();
   }
   co_return forge::net::p2p::pubsub::validation_result::retry;
}

boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
plugin::impl::handle_event_owned(std::shared_ptr<impl> self, forge::net::p2p::pubsub::event event) {
   if (!self) { co_return forge::net::p2p::pubsub::validation_result::ignore; }
   auto handlers = std::vector<std::shared_ptr<handler_record>>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->stopping) { co_return forge::net::p2p::pubsub::validation_result::ignore; }
      if (event.value.data.size() > self->settings.max_message_size) {
         ++self->messages_dropped;
         co_return forge::net::p2p::pubsub::validation_result::ignore;
      }
      if (const auto found = self->topics.find(event.value.subject.value); found != self->topics.end()) {
         for (const auto& [_, record] : found->second->handlers) {
            if (record->committed && !record->removing) { handlers.push_back(record); }
         }
         const auto& partial = found->second->partial;
         if (partial && partial->committed && partial->admission) { handlers.push_back(partial->fallback); }
      }
      if (handlers.empty()) { co_return forge::net::p2p::pubsub::validation_result::ignore; }
      ++self->active_events;
   }
   auto completed = boost::scope::scope_exit{[&] {
      // Destruction of user captures is part of the owned event, never under the plugin mutex.
      handlers.clear();
      {
         const auto lock = std::scoped_lock{self->mutex};
         --self->active_events;
      }
      self->changed.notify();
   }};
   auto final_result = forge::net::p2p::pubsub::validation_result::ignore;
   for (const auto& handler : handlers) {
      auto result = co_await self->call_handler(handler, project_message(event.source, event.value));
      if (result == forge::net::p2p::pubsub::validation_result::reject) {
         final_result = forge::net::p2p::pubsub::validation_result::reject;
      } else if (result == forge::net::p2p::pubsub::validation_result::retry &&
                 final_result != forge::net::p2p::pubsub::validation_result::reject) {
         final_result = forge::net::p2p::pubsub::validation_result::retry;
      } else if (result == forge::net::p2p::pubsub::validation_result::accept &&
                 final_result != forge::net::p2p::pubsub::validation_result::reject &&
                 final_result != forge::net::p2p::pubsub::validation_result::retry) {
         final_result = forge::net::p2p::pubsub::validation_result::accept;
      }
   }
   const auto lock = std::scoped_lock{self->mutex};
   ++self->messages_delivered;
   switch (final_result) {
   case forge::net::p2p::pubsub::validation_result::accept: ++self->messages_accepted; break;
   case forge::net::p2p::pubsub::validation_result::reject: ++self->messages_rejected; break;
   case forge::net::p2p::pubsub::validation_result::ignore: ++self->messages_ignored; break;
   case forge::net::p2p::pubsub::validation_result::retry: ++self->messages_retried; break;
   }
   co_return final_result;
}

} // namespace forge::plugins::net::p2p::pubsub
