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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.net.p2p.node;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;

#include "details/project_message.hxx"
#include "details/pubsub_policy.hxx"
#include "details/pubsub_service.hxx"
#include "details/plugin_pubsub_api_impl.hxx"

namespace forge::plugins::net::p2p::node {

using detail::handler_record;
using detail::partial_record;
using detail::topic_state;
using detail::project_message;
using detail::to_ms;

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::pubsub_api_impl::enable_partial(
    forge::net::p2p::pubsub::topic subject, handler fallback,
    forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options) {
   return enable_partial_owned(impl_, std::move(subject), std::move(fallback), std::move(options), fallback_options);
}

boost::asio::awaitable<void> plugin::pubsub_api_impl::disable_partial(forge::net::p2p::pubsub::partial_topic token) {
   return disable_partial_owned(impl_, std::move(token));
}

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::pubsub_api_impl::enable_partial_owned(
    std::shared_ptr<detail::pubsub_service> self, forge::net::p2p::pubsub::topic subject, handler fallback,
    forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options) {
   auto operation = enable_partial_transition(std::move(self), std::move(subject), std::move(fallback),
                                               std::move(options), fallback_options);
   fallback = {}; options = {};
   co_return co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       std::move(operation), boost::asio::use_awaitable);
}

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::pubsub_api_impl::enable_partial_transition(
    std::shared_ptr<detail::pubsub_service> self, forge::net::p2p::pubsub::topic subject, handler fallback,
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
   auto source = std::shared_ptr<forge::net::p2p::node>{};
   self->admit_operation([&] {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_node_locked();
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
   });
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
      self->admit_operation([&] {
         const auto lock = std::scoped_lock{self->mutex};
         static_cast<void>(self->require_node_locked());
         if (topic->leave_error) { std::rethrow_exception(topic->leave_error); }
         if (topic->join_error) { std::rethrow_exception(topic->join_error); }
         record->attempted = true;
         topic->native_dirty = true;
      });
      const auto weak = std::weak_ptr<detail::pubsub_service>{self};
      const auto weak_record = std::weak_ptr<partial_record>{record};
      auto native = forge::net::p2p::pubsub::partial_options{
          .requests_partial = record->callbacks.requests_partial,
          .receive = [weak, weak_record](forge::net::p2p::pubsub::partial_event event, std::stop_token stop) {
             return detail::pubsub_service::receive_partial_owned(weak, weak_record, std::move(event), stop);
          },
          .gossip = [weak, weak_record](forge::net::p2p::pubsub::partial_gossip_event event, std::stop_token stop) {
             return detail::pubsub_service::gossip_partial_owned(weak, weak_record, std::move(event), stop);
          }};
      auto token = co_await self->operations.enable_partial(source, subject, detail::pubsub_service::full_dispatch(self), std::move(native));
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      self->admit_operation([&] {
         const auto lock = std::scoped_lock{self->mutex};
         record->token = std::move(token);
         static_cast<void>(self->require_node_locked());
         if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
            FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "Partial enable canceled");
         }
         record->admission = record->committed = true;
         topic->joined = true;
      });
   } catch (...) { failure = std::current_exception(); }
   if (failure && record->attempted) {
      record->stop.request_stop();
      auto cleanup = std::exception_ptr{};
      // Native enable can mutate and throw before returning its token. Restore the full mux, not an unrelated leave.
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      try {
         co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
             detail::pubsub_service::restore_full_owned(self, topic, source), boost::asio::use_awaitable);
      } catch (...) { cleanup = std::current_exception(); }
      auto retired_error = std::exception_ptr{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         record->cleanup_pending = static_cast<bool>(cleanup);
         retired_error.swap(topic->leave_error);
         topic->leave_error = cleanup;
         if (!cleanup) { topic->partial.reset(); }
      }
   }
   if (failure) { std::rethrow_exception(failure); }
   co_return record->token;
}

boost::asio::awaitable<void> plugin::pubsub_api_impl::disable_partial_owned(std::shared_ptr<detail::pubsub_service> self,
    forge::net::p2p::pubsub::partial_topic token) {
   co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       disable_partial_transition(std::move(self), std::move(token)), boost::asio::use_awaitable);
}

boost::asio::awaitable<void> plugin::pubsub_api_impl::disable_partial_transition(std::shared_ptr<detail::pubsub_service> self,
    forge::net::p2p::pubsub::partial_topic token) {
   auto source = std::shared_ptr<forge::net::p2p::node>{};
   auto topic = std::shared_ptr<topic_state>{};
   auto record = std::shared_ptr<partial_record>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_node_locked(true);
      if (self->shutdown && !self->shutdown->done) {
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub cleanup is already running");
      }
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
      static_cast<void>(self->require_node_locked(true));
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
         co_await detail::pubsub_service::restore_full_owned(self, topic, source);
      } else {
         co_await self->operations.disable_partial(source, token);
         auto ordinary = false;
         {
            const auto lock = std::scoped_lock{self->mutex};
            for (const auto& [_, handler] : topic->handlers) { ordinary = ordinary || handler->committed; }
         }
         if (!ordinary) { co_await self->operations.unsubscribe(source, topic->subject); }
         {
            const auto lock = std::scoped_lock{self->mutex};
            topic->joined = topic->native_dirty = ordinary;
         }
      }
   } catch (...) { failure = std::current_exception(); }
   auto retired_error = std::exception_ptr{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      retired_error.swap(topic->leave_error);
      topic->leave_error = failure;
      if (!failure) { topic->partial.reset(); }
   }
   // Never resurrect admission/token validity after a potentially committed native downgrade.
   if (failure) { std::rethrow_exception(failure); }
}

} // namespace forge::plugins::net::p2p::node
