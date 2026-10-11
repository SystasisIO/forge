module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <stop_token>

module forge.plugins.net.p2p.node.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.net.p2p.node;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.pubsub_exceptions;
import forge.plugins.net.p2p.node.pubsub_types;

#include "details/pubsub_policy.hxx"
#include "details/project_message.hxx"
#include "details/pubsub_service.hxx"
#include "details/plugin_pubsub_api_impl.hxx"

namespace forge::plugins::net::p2p::node {

using detail::handler_record;
using detail::partial_record;
using detail::topic_state;
using detail::project_message;
using detail::to_ms;

plugin::pubsub_api_impl::pubsub_api_impl(std::shared_ptr<detail::pubsub_service> impl) : impl_{std::move(impl)} {}

boost::asio::awaitable<message> plugin::pubsub_api_impl::publish(
    forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, publish_options options) {
   return publish_owned(impl_, std::move(subject), std::move(data), options);
}

boost::asio::awaitable<subscription> plugin::pubsub_api_impl::subscribe(
    forge::net::p2p::pubsub::topic subject, handler callback, subscribe_options options) {
   return subscribe_owned(impl_, std::move(subject), std::move(callback), options);
}

boost::asio::awaitable<void> plugin::pubsub_api_impl::unsubscribe(subscription value) {
   return unsubscribe_owned(impl_, std::move(value));
}

boost::asio::awaitable<message> plugin::pubsub_api_impl::publish_owned(
    std::shared_ptr<detail::pubsub_service> self, forge::net::p2p::pubsub::topic subject,
    std::vector<std::uint8_t> data, publish_options options) {
   auto source = std::shared_ptr<forge::net::p2p::node>{};
   auto sign = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_node_locked();
      self->ensure_topic_allowed_locked(subject);
      if (data.size() > self->settings.max_message_size) {
         FORGE_THROW_EXCEPTION(pubsub_exceptions::message_too_large, "P2P PubSub message exceeds configured limit",
                               forge::exceptions::ctx("topic", subject.value));
      }
      sign = options.sign.value_or(self->settings.sign_publishes);
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_operation(); }};
   auto published = co_await self->operations.publish(source,
       std::move(subject), std::move(data), forge::net::p2p::pubsub::publish_options{.sign = sign});
   {
      const auto lock = std::scoped_lock{self->mutex};
      ++self->messages_published;
   }
   co_return project_message(source->local_peer(), published);
}

std::vector<subscription> plugin::pubsub_api_impl::subscriptions() const {
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

::forge::plugins::net::p2p::node::pubsub_snapshot plugin::pubsub_api_impl::snapshot() const {
   const auto self = impl_;
   const auto source = self->begin_operation();
   auto completed = boost::scope::scope_exit{[&] { self->finish_operation(); }};
   auto result = ::forge::plugins::net::p2p::node::pubsub_snapshot{};
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
   result.core = source->pubsub_snapshot();
   return result;
}

} // namespace forge::plugins::net::p2p::node
