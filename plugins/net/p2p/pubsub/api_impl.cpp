module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.types;

#include "details/config.hxx"
#include "details/project_message.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

plugin::api_impl::api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

boost::asio::awaitable<message> plugin::api_impl::publish(
    forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, publish_options options) {
   return publish_owned(impl_, std::move(subject), std::move(data), options);
}

boost::asio::awaitable<subscription> plugin::api_impl::subscribe(
    forge::net::p2p::pubsub::topic subject, handler callback, subscribe_options options) {
   return subscribe_owned(impl_, std::move(subject), std::move(callback), options);
}

boost::asio::awaitable<void> plugin::api_impl::unsubscribe(subscription value) {
   return unsubscribe_owned(impl_, std::move(value));
}

boost::asio::awaitable<message> plugin::api_impl::publish_owned(
    std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject,
    std::vector<std::uint8_t> data, publish_options options) {
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   auto sign = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      self->ensure_topic_allowed_locked(subject);
      if (data.size() > self->settings.max_message_size) {
         FORGE_THROW_EXCEPTION(exceptions::message_too_large, "P2P PubSub message exceeds configured limit",
                               forge::exceptions::ctx("topic", subject.value));
      }
      sign = options.sign.value_or(self->settings.sign_publishes);
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_operation(); }};
   auto published = co_await source->async_publish_message(
       std::move(subject), std::move(data), forge::net::p2p::pubsub::publish_options{.sign = sign});
   {
      const auto lock = std::scoped_lock{self->mutex};
      ++self->messages_published;
   }
   co_return project_message(source->local_peer(), published);
}

std::vector<subscription> plugin::api_impl::subscriptions() const {
   const auto self = impl_;
   const auto source = self->begin_operation();
   auto completed = boost::scope::scope_exit{[&] { self->finish_operation(); }};
   const auto lock = std::scoped_lock{self->mutex};
   auto out = std::vector<subscription>{};
   for (const auto& [_, topic] : self->topics) {
      for (const auto& [id, record] : topic->handlers) {
         if (record->committed) { out.push_back(subscription{.id = id, .subject = record->subject}); }
      }
   }
   return out;
}

::forge::plugins::net::p2p::pubsub::snapshot plugin::api_impl::snapshot() const {
   const auto self = impl_;
   const auto source = self->begin_operation();
   auto completed = boost::scope::scope_exit{[&] { self->finish_operation(); }};
   auto result = ::forge::plugins::net::p2p::pubsub::snapshot{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      result.topics = self->topics.size();
      for (const auto& [_, topic] : self->topics) {
         for (const auto& [id, record] : topic->handlers) {
            if (record->committed) { ++result.subscriptions; }
         }
      }
      result.active_handlers = self->active_handlers;
      result.messages_published = self->messages_published;
      result.messages_delivered = self->messages_delivered;
      result.messages_accepted = self->messages_accepted;
      result.messages_rejected = self->messages_rejected;
      result.messages_ignored = self->messages_ignored;
      result.messages_retried = self->messages_retried;
      result.messages_dropped = self->messages_dropped;
      result.handler_failures = self->handler_failures;
   }
   result.core = source->snapshot();
   return result;
}

} // namespace forge::plugins::net::p2p::pubsub
