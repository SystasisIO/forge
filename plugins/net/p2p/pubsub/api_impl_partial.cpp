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
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.types;

#include "details/plugin_impl.hxx"
#include "details/config.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::api_impl::enable_partial(
    forge::net::p2p::pubsub::topic subject, handler fallback,
    forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options) {
   return enable_partial_owned(impl_, std::move(subject), std::move(fallback), std::move(options), fallback_options);
}

boost::asio::awaitable<void> plugin::api_impl::disable_partial(forge::net::p2p::pubsub::partial_topic token) {
   return disable_partial_owned(impl_, std::move(token));
}

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::api_impl::enable_partial_owned(
    std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject, handler fallback,
    forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options) {
   auto operation = enable_partial_transition(std::move(self), std::move(subject), std::move(fallback),
                                               std::move(options), fallback_options);
   fallback = {}; options = {};
   co_return co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       std::move(operation), boost::asio::use_awaitable);
}

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::api_impl::enable_partial_transition(
    std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject, handler fallback,
    forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options) {
   if (!fallback || !options.receive || !options.gossip) {
      FORGE_THROW_EXCEPTION(exceptions::handler_limit, "Partial requires full fallback, receive and gossip handlers");
   }
   auto record = std::make_shared<partial_record>();
   record->fallback = std::make_shared<handler_record>();
   record->fallback->subject = subject;
   record->fallback->callback = std::move(fallback); fallback = {};
   record->callbacks = std::move(options); options = {};
   auto candidate = std::make_shared<topic_state>();
   candidate->subject = subject;
   auto topic = std::shared_ptr<topic_state>{};
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      self->ensure_topic_allowed_locked(subject);
      if (!self->settings.partial_messages) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Partial Messages are disabled");
      }
      const auto found = self->topics.find(subject.value);
      if (found == self->topics.end() && self->topics.size() >= self->settings.max_topics) {
         FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub topic limit reached");
      }
      topic = found == self->topics.end() ? candidate : found->second;
      if (topic->partial || topic->handlers.size() >= self->settings.max_handlers_per_topic) {
         FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P Partial owner or handler limit reached");
      }
      record->fallback->deadline = fallback_options.handler_deadline.count() > 0
          ? fallback_options.handler_deadline : to_ms(self->settings.handler_deadline_ms);
      if (found == self->topics.end()) { self->topics.emplace(subject.value, topic); }
      topic->partial = record; // Reserve before the gate, including the required fallback.
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] {
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (!record->attempted && topic->partial == record) { topic->partial.reset(); }
      }
      self->finish_partial_operation(topic, record);
   }};
   auto ticket = forge::asio::gate::ticket{};
   auto failure = std::exception_ptr{};
   try {
      ticket = co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
          topic->transition.acquire(), boost::asio::use_awaitable);
      {
         const auto lock = std::scoped_lock{self->mutex};
         static_cast<void>(self->require_source_locked());
         if (topic->leave_error) { std::rethrow_exception(topic->leave_error); }
         if (topic->join_error) { std::rethrow_exception(topic->join_error); }
         record->attempted = true;
         topic->native_dirty = true;
      }
      const auto weak = std::weak_ptr<plugin::impl>{self};
      const auto weak_record = std::weak_ptr<partial_record>{record};
      auto native = forge::net::p2p::pubsub::partial_options{
          .requests_partial = record->callbacks.requests_partial,
          .receive = [weak, weak_record](forge::net::p2p::pubsub::partial_event event, std::stop_token stop) {
             return plugin::impl::receive_partial_owned(weak, weak_record, std::move(event), stop);
          },
          .gossip = [weak, weak_record](forge::net::p2p::pubsub::partial_gossip_event event, std::stop_token stop) {
             return plugin::impl::gossip_partial_owned(weak, weak_record, std::move(event), stop);
          }};
      auto token = co_await source->async_enable_partial(subject, plugin::impl::full_dispatch(self), std::move(native));
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      {
         const auto lock = std::scoped_lock{self->mutex};
         record->token = std::move(token);
         static_cast<void>(self->require_source_locked());
         if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
            FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "Partial enable canceled");
         }
         record->admission = record->committed = true;
         topic->joined = true;
      }
   } catch (...) { failure = std::current_exception(); }
   if (failure && record->attempted) {
      record->stop.request_stop();
      auto cleanup = std::exception_ptr{};
      // Native enable can mutate and throw before returning its token. Restore the full mux, not an unrelated leave.
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      try {
         co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
             plugin::impl::restore_full_owned(self, topic, source), boost::asio::use_awaitable);
      } catch (...) { cleanup = std::current_exception(); }
      {
         const auto lock = std::scoped_lock{self->mutex};
         record->cleanup_pending = static_cast<bool>(cleanup);
         topic->leave_error = cleanup;
         if (!cleanup) { topic->partial.reset(); }
      }
   }
   if (failure) { std::rethrow_exception(failure); }
   co_return record->token;
}

boost::asio::awaitable<void> plugin::api_impl::disable_partial_owned(std::shared_ptr<plugin::impl> self,
    forge::net::p2p::pubsub::partial_topic token) {
   co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       disable_partial_transition(std::move(self), std::move(token)), boost::asio::use_awaitable);
}

boost::asio::awaitable<void> plugin::api_impl::disable_partial_transition(std::shared_ptr<plugin::impl> self,
    forge::net::p2p::pubsub::partial_topic token) {
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   auto topic = std::shared_ptr<topic_state>{};
   auto record = std::shared_ptr<partial_record>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      record = self->require_partial_locked(token, true);
      topic = self->topics.at(token.subject().value);
      record->removing = true;
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] {
      {
         const auto lock = std::scoped_lock{self->mutex};
         record->removing = false;
      }
      self->finish_partial_operation(topic, record);
   }};
   auto ticket = co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       topic->transition.acquire(), boost::asio::use_awaitable);
   auto repair = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      static_cast<void>(self->require_source_locked());
      if (topic->partial != record || record->token != token) {
         FORGE_THROW_EXCEPTION(exceptions::subscription_not_found, "Partial owner changed");
      }
      repair = record->cleanup_pending;
      record->admission = record->committed = false;
      record->cleanup_pending = true;
   }
   record->stop.request_stop();
   auto failure = std::exception_ptr{};
   try {
      if (repair) {
         co_await plugin::impl::restore_full_owned(self, topic, source);
      } else {
         co_await source->async_disable_partial(token);
         auto ordinary = false;
         {
            const auto lock = std::scoped_lock{self->mutex};
            for (const auto& [_, handler] : topic->handlers) { ordinary = ordinary || handler->committed; }
         }
         if (!ordinary) { co_await source->async_leave_topic(topic->subject); }
         {
            const auto lock = std::scoped_lock{self->mutex};
            topic->joined = topic->native_dirty = ordinary;
         }
      }
   } catch (...) { failure = std::current_exception(); }
   {
      const auto lock = std::scoped_lock{self->mutex};
      topic->leave_error = failure;
      if (!failure) { topic->partial.reset(); }
   }
   // Never resurrect admission/token validity after a potentially committed native downgrade.
   if (failure) { std::rethrow_exception(failure); }
}

} // namespace forge::plugins::net::p2p::pubsub
