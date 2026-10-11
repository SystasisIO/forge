module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <stop_token>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.net.p2p.pubsub;
import forge.net.p2p.identity;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

boost::asio::awaitable<subscription> plugin::api_impl::subscribe_owned(
    std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject,
    handler callback, subscribe_options options) {
   auto operation = subscribe_transition(std::move(self), std::move(subject), std::move(callback), options);
   callback = {};
   co_return co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       std::move(operation), boost::asio::use_awaitable);
}

boost::asio::awaitable<void> plugin::api_impl::unsubscribe_owned(
    std::shared_ptr<plugin::impl> self, subscription value) {
   co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       unsubscribe_transition(std::move(self), std::move(value)), boost::asio::use_awaitable);
}

boost::asio::awaitable<subscription> plugin::api_impl::subscribe_transition(
    std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject,
    handler callback, subscribe_options options) {
   if (!callback) {
      FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub subscription requires handler");
   }
   auto record = std::make_shared<handler_record>();
   record->subject = subject;
   record->callback = std::move(callback);
   callback = {};
   auto candidate = std::make_shared<topic_state>();
   candidate->subject = subject;
   auto topic = std::shared_ptr<topic_state>{};
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      self->ensure_topic_allowed_locked(subject);
      const auto found = self->topics.find(subject.value);
      if (found == self->topics.end() && self->topics.size() >= self->settings.max_topics) {
         FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub topic limit reached");
      }
      topic = found == self->topics.end() ? candidate : found->second;
      if (topic->handlers.size() + static_cast<std::size_t>(static_cast<bool>(topic->partial)) >= self->settings.max_handlers_per_topic ||
          self->next_subscription == (std::numeric_limits<std::uint64_t>::max)()) {
         FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub subscription limit reached");
      }
      record->id = self->next_subscription++;
      record->deadline = options.handler_deadline.count() > 0
          ? options.handler_deadline : to_ms(self->settings.handler_deadline_ms);
      topic->handlers.emplace(record->id, record);
      if (found == self->topics.end()) { self->topics.emplace(subject.value, topic); }
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_topic_operation(topic, record, true); }};
   // Release the gate before the last participant can retire the topic.
   auto ticket = forge::asio::gate::ticket{};
   auto failure = std::exception_ptr{};
   auto attempted_join = false;
   try {
      // Gate's cancellation filter belongs to the joined child, not the native join that follows.
      ticket = co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
          topic->transition.acquire(), boost::asio::use_awaitable);
      auto join = false;
      {
         const auto lock = std::scoped_lock{self->mutex};
         (void)self->require_source_locked();
         if (topic->leave_error) { std::rethrow_exception(topic->leave_error); }
         if (topic->join_error) { std::rethrow_exception(topic->join_error); }
         join = !topic->joined;
         if (join) { topic->native_dirty = true; }
      }
      if (join) {
         attempted_join = true;
         const auto weak = std::weak_ptr<plugin::impl>{self};
         (void)co_await source->async_join_topic(subject, [weak](forge::net::p2p::pubsub::event event) {
            return plugin::impl::handle_event_owned(weak.lock(), std::move(event));
         });
      }
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub subscribe canceled");
      }
      {
         const auto lock = std::scoped_lock{self->mutex};
         (void)self->require_source_locked();
         topic->joined = true;
         record->committed = true;
      }
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure && attempted_join) {
      // Native subscribe may mutate before throwing. Compensation retains the same gate and source.
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      auto cleanup_error = std::exception_ptr{};
      try { co_await source->async_leave_topic(subject); }
      catch (...) { cleanup_error = std::current_exception(); }
      const auto lock = std::scoped_lock{self->mutex};
      topic->join_error = failure;
      topic->leave_error = cleanup_error;
      topic->joined = false;
      topic->native_dirty = static_cast<bool>(cleanup_error);
   }
   if (failure) { std::rethrow_exception(failure); }
   co_return subscription{.id = record->id, .subject = std::move(subject)};
}

boost::asio::awaitable<void> plugin::api_impl::unsubscribe_transition(
    std::shared_ptr<plugin::impl> self, subscription value) {
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   auto topic = std::shared_ptr<topic_state>{};
   auto record = std::shared_ptr<handler_record>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      const auto found = self->topics.find(value.subject.value);
      if (found != self->topics.end()) {
         topic = found->second;
         if (const auto entry = topic->handlers.find(value.id); entry != topic->handlers.end()) {
            record = entry->second;
         }
      }
      if (!record || !record->committed || record->removing) {
         FORGE_THROW_EXCEPTION(exceptions::subscription_not_found, "P2P PubSub subscription was not found or is leaving");
      }
      record->removing = true;
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_topic_operation(topic, record, false); }};
   auto ticket = co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       topic->transition.acquire(), boost::asio::use_awaitable);
   auto leave = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      (void)self->require_source_locked();
      // A pre-gate reservation owns quota, not the native topic. The same gate excludes
      // an in-progress enable/repair, so only a committed or dirty native owner can retain it.
      const auto& partial = topic->partial;
      leave = !partial || !(partial->committed || partial->attempted || partial->cleanup_pending);
      for (const auto& [id, other] : topic->handlers) {
         if (id != value.id && other->committed) { leave = false; break; }
      }
   }
   if (leave) {
      // Once admitted, finish leave's native mutation even if the caller cancels.
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      auto failure = std::exception_ptr{};
      try { co_await source->async_leave_topic(value.subject); }
      catch (...) { failure = std::current_exception(); }
      {
         const auto lock = std::scoped_lock{self->mutex};
         topic->leave_error = failure;
         if (!failure) { topic->joined = false; topic->native_dirty = false; }
      }
      if (failure) { std::rethrow_exception(failure); }
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      topic->handlers.erase(value.id);
   }
}

} // namespace forge::plugins::net::p2p::pubsub
