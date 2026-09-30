module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <algorithm>
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

module forge.plugins.net.p2p.pubsub.plugin;

import forge.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.types;

#include "details/project_message.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {
namespace {

[[nodiscard]] bool contains_topic(const std::vector<std::string>& values, const std::string& topic) {
   return std::ranges::find(values, topic) != values.end();
}

} // namespace

forge::plugins::net::p2p::node::pubsub_source& plugin::impl::require_source() const {
   if (!initialized || !source) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P PubSub plugin is not initialized");
   }
   return *source;
}

void plugin::impl::ensure_topic_allowed(const forge::net::p2p::pubsub::topic& subject) const {
   if (subject.value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::topic_not_allowed, "P2P PubSub topic is empty");
   }
   if (!settings.allowed_topics.empty() && !contains_topic(settings.allowed_topics, subject.value)) {
      FORGE_THROW_EXCEPTION(exceptions::topic_not_allowed, "P2P PubSub topic is not allowed",
                          forge::exceptions::ctx("topic", subject.value));
   }
   if (contains_topic(settings.denied_topics, subject.value)) {
      FORGE_THROW_EXCEPTION(exceptions::topic_not_allowed, "P2P PubSub topic is denied",
                          forge::exceptions::ctx("topic", subject.value));
   }
}

std::vector<handler_record> plugin::impl::handlers_for(const std::string& topic) const {
   auto lock = std::scoped_lock{mutex};
   auto out = std::vector<handler_record>{};
   if (const auto found = topics.find(topic); found != topics.end()) {
      out.reserve(found->second.handlers.size());
      for (const auto& [_, handler] : found->second.handlers) {
         out.push_back(handler);
      }
   }
   return out;
}

bool plugin::impl::try_begin_handler() {
   auto lock = std::scoped_lock{mutex};
   if (active_handlers >= settings.max_active_handlers) {
      ++messages_dropped;
      return false;
   }
   ++active_handlers;
   return true;
}

void plugin::impl::finish_handler() {
   auto lock = std::scoped_lock{mutex};
   if (active_handlers > 0) {
      --active_handlers;
   }
}

void plugin::impl::record_handler_failure() {
   auto lock = std::scoped_lock{mutex};
   ++handler_failures;
}

void plugin::impl::record_drop() {
   auto lock = std::scoped_lock{mutex};
   ++messages_dropped;
}

boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
plugin::impl::call_handler(handler_record handler, message value) {
   if (!try_begin_handler()) {
      co_return forge::net::p2p::pubsub::validation_result::retry;
   }

   if (handler.deadline.count() <= 0) {
      try {
         auto result = co_await handler.callback(std::move(value));
         finish_handler();
         co_return result;
      } catch (...) {
         finish_handler();
         record_handler_failure();
         co_return forge::net::p2p::pubsub::validation_result::retry;
      }
   }

   auto executor = co_await boost::asio::this_coro::executor;
   auto self = shared_from_this();
   auto guarded_handler =
      [self, handler = std::move(handler), value = std::move(value)]() mutable
      -> boost::asio::awaitable<std::optional<forge::net::p2p::pubsub::validation_result>> {
      try {
         co_return co_await handler.callback(std::move(value));
      } catch (...) {
         self->record_handler_failure();
         co_return std::nullopt;
      }
   };
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(handler.deadline);

   try {
      using namespace boost::asio::experimental::awaitable_operators;
      auto result = co_await (guarded_handler() || timer.async_wait(boost::asio::use_awaitable));
      finish_handler();
      if (result.index() == 0) {
         const auto& value = std::get<0>(result);
         if (value.has_value()) {
            co_return *value;
         }
         co_return forge::net::p2p::pubsub::validation_result::retry;
      }
      record_handler_failure();
   } catch (...) {
      finish_handler();
      record_handler_failure();
   }
   co_return forge::net::p2p::pubsub::validation_result::retry;
}

boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
plugin::impl::handle_event(forge::net::p2p::pubsub::event event) {
   if (event.value.data.size() > settings.max_message_size) {
      record_drop();
      co_return forge::net::p2p::pubsub::validation_result::ignore;
   }

   auto handlers = handlers_for(event.value.subject.value);
   if (handlers.empty()) {
      co_return forge::net::p2p::pubsub::validation_result::ignore;
   }

   auto final_result = forge::net::p2p::pubsub::validation_result::ignore;
   for (auto& handler : handlers) {
      auto result = co_await call_handler(handler, project_message(event.source, event.value));
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

   auto lock = std::scoped_lock{mutex};
   ++messages_delivered;
   switch (final_result) {
   case forge::net::p2p::pubsub::validation_result::accept:
      ++messages_accepted;
      break;
   case forge::net::p2p::pubsub::validation_result::reject:
      ++messages_rejected;
      break;
   case forge::net::p2p::pubsub::validation_result::ignore:
      ++messages_ignored;
      break;
   case forge::net::p2p::pubsub::validation_result::retry:
      ++messages_retried;
      break;
   }
   co_return final_result;
}

} // namespace forge::plugins::net::p2p::pubsub
