module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
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
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.net.p2p.node;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;

#include "details/pubsub_policy.hxx"
#include "details/pubsub_service.hxx"

namespace forge::plugins::net::p2p::node::detail {

void pubsub_service::admit_operation(const std::function<void()>& install) const {
   const auto state = lifecycle.snapshot();
   if (!state.admission_open) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub admission is closed");
   }
   if (state.phase != forge::app::lifecycle_phase::started) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P PubSub plugin is not running");
   }
   if (!lifecycle.try_admit(state.generation, forge::app::lifecycle_phase::started, install)) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub admission is closed");
   }
}

std::shared_ptr<forge::net::p2p::node> pubsub_service::require_node_locked(bool cleanup) const {
   if (stopping && !cleanup) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub admission is closed");
   }
   if (!initialized || !source) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P PubSub plugin is not initialized");
   }
   if (!settings.enabled) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::unsupported_protocol, "P2P PubSub is disabled");
   }
   return source;
}

std::shared_ptr<forge::net::p2p::node> pubsub_service::begin_operation() {
   auto current = std::shared_ptr<forge::net::p2p::node>{};
   admit_operation([&] {
      const auto lock = std::scoped_lock{mutex};
      current = require_node_locked();
      ++active_operations;
   });
   return current;
}

void pubsub_service::finish_operation() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      --active_operations;
   }
   changed.notify();
}

void pubsub_service::finish_topic_operation(const std::shared_ptr<topic_state>& topic,
                                          std::shared_ptr<handler_record>& record,
                                          bool subscribing) noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      if (subscribing && !record->committed) { topic->handlers.erase(record->id); }
      if (!subscribing) { record->removing = false; }
      --topic->participants;
      if (topic->participants == 0 && topic->handlers.empty() && !topic->partial && !topic->native_dirty) {
         const auto found = topics.find(topic->subject.value);
         if (found != topics.end() && found->second == topic) { topics.erase(found); }
      }
   }
   // A last user capture may block in its destructor; it remains part of this owned operation.
   record.reset();
   finish_operation();
}

void pubsub_service::ensure_topic_allowed_locked(const forge::net::p2p::pubsub::topic& subject) const {
   const auto contains = [&](const auto& values) {
      return std::ranges::find(values, subject.value) != values.end();
   };
   if (subject.value.empty() || (!settings.allowed_topics.empty() && !contains(settings.allowed_topics)) ||
       contains(settings.denied_topics)) {
      FORGE_THROW_EXCEPTION(exceptions::topic_not_allowed, "P2P PubSub topic is not allowed",
                            forge::exceptions::ctx("topic", subject.value));
   }
}

void pubsub_service::initialize(pubsub_config value, std::shared_ptr<forge::net::p2p::node> current) {
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (stopping) {
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub admission is closed");
      }
      if (initialized || !current) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub is already initialized or has no node");
      }
      settings = std::move(value);
      source = std::move(current);
      initialized = true;
   })) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub initialization was stopped");
   }
}

void pubsub_service::request_stop() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      stopping = true;
   }
   // Active retired records also subscribe to this shared stop source.
   partial_stop.request_stop();
   // Keep transition gates usable for retained cleanup tokens. Normal waiters
   // recheck common admission after acquiring; shutdown joins all participants.
   changed.notify();
}

boost::asio::awaitable<void> pubsub_service::shutdown_owned(std::shared_ptr<pubsub_service> self) {
   co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       shutdown_joined(std::move(self)), boost::asio::use_awaitable);
}

boost::asio::awaitable<void> pubsub_service::shutdown_joined(std::shared_ptr<pubsub_service> self) {
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   self->request_stop();
   auto candidate = std::make_shared<shutdown_attempt>();
   auto attempt = std::shared_ptr<shutdown_attempt>{};
   auto leader = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (!self->shutdown || (self->shutdown->done && !self->shutdown->terminal)) {
         self->shutdown.swap(candidate);
         for (const auto& [_, topic] : self->topics) { topic->shutdown_attempted = false; }
         leader = true;
      }
      attempt = self->shutdown;
   }
   // Previous exception captures and unused candidates retire unlocked.
   candidate.reset();
   if (!leader) {
      for (;;) {
         const auto epoch = attempt->completed.epoch();
         auto done = false;
         auto error = std::exception_ptr{};
         {
            const auto lock = std::scoped_lock{self->mutex};
            done = attempt->done;
            error = attempt->error;
         }
         if (done) {
            if (error) { std::rethrow_exception(error); }
            co_return;
         }
         (void)co_await attempt->completed.async_wait(epoch);
      }
   }

   auto failure = std::exception_ptr{};
   auto source = std::shared_ptr<forge::net::p2p::node>{};
   auto retired = std::map<std::string, std::shared_ptr<topic_state>>{};
   try {
      for (;;) {
         const auto epoch = self->changed.epoch();
         auto idle = false;
         {
            const auto lock = std::scoped_lock{self->mutex};
            idle = self->active_operations == 0 && self->active_events == 0;
         }
         if (idle) { break; }
         (void)co_await self->changed.async_wait(epoch);
      }
      {
         const auto lock = std::scoped_lock{self->mutex};
         source = self->source;
      }
      for (;;) {
         auto topic = std::shared_ptr<topic_state>{};
         {
            const auto lock = std::scoped_lock{self->mutex};
            for (const auto& [_, current] : self->topics) {
               if (!current->shutdown_attempted) {
                  current->shutdown_attempted = true;
                  topic = current;
                  break;
               }
            }
         }
         if (!topic) { break; }
         auto handlers = std::map<std::uint64_t, std::shared_ptr<handler_record>>{};
         auto partial = std::shared_ptr<partial_record>{};
         auto leave = false;
         {
            const auto lock = std::scoped_lock{self->mutex};
            partial = topic->partial;
            leave = topic->native_dirty;
         }
         if (partial) { partial->stop.request_stop(); }
         auto error = std::exception_ptr{};
         if (leave) {
            try { co_await self->operations.unsubscribe(source, topic->subject); }
            catch (...) { error = std::current_exception(); }
         }
         auto retired_error = std::exception_ptr{};
         {
            const auto lock = std::scoped_lock{self->mutex};
            std::swap(retired_error, topic->leave_error);
            topic->leave_error = error;
            if (!error) {
               topic->native_dirty = false;
               topic->joined = false;
               handlers.swap(topic->handlers);
               partial = std::move(topic->partial);
            }
         }
         // Failed topics keep their captures and native owner for retry.
         handlers.clear();
         partial.reset();
         if (error && !failure) { failure = error; }
      }
   } catch (...) {
      if (!failure) { failure = std::current_exception(); }
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (!failure) {
         self->topics.swap(retired);
         source = std::move(self->source);
         self->initialized = false;
      }
   }
   retired.clear();
   source.reset();
   {
      const auto lock = std::scoped_lock{self->mutex};
      attempt->error = failure;
      attempt->terminal = !failure;
      attempt->done = true;
   }
   attempt->completed.notify();
   self->changed.notify();
   if (failure) { std::rethrow_exception(failure); }
}

} // namespace forge::plugins::net::p2p::node::detail
