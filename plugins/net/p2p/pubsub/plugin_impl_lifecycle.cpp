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
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> plugin::impl::require_source_locked() const {
   if (stopping) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub admission is closed");
   }
   if (!initialized || !source) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P PubSub plugin is not initialized");
   }
   return source;
}

std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> plugin::impl::begin_operation() {
   const auto lock = std::scoped_lock{mutex};
   auto current = require_source_locked();
   ++active_operations;
   return current;
}

void plugin::impl::finish_operation() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      --active_operations;
   }
   changed.notify();
}

void plugin::impl::finish_topic_operation(const std::shared_ptr<topic_state>& topic,
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

void plugin::impl::ensure_topic_allowed_locked(const forge::net::p2p::pubsub::topic& subject) const {
   const auto contains = [&](const auto& values) {
      return std::ranges::find(values, subject.value) != values.end();
   };
   if (subject.value.empty() || (!settings.allowed_topics.empty() && !contains(settings.allowed_topics)) ||
       contains(settings.denied_topics)) {
      FORGE_THROW_EXCEPTION(exceptions::topic_not_allowed, "P2P PubSub topic is not allowed",
                            forge::exceptions::ctx("topic", subject.value));
   }
}

void plugin::impl::configure(config value) {
   const auto lock = std::scoped_lock{mutex};
   if (initialized || initializing || stopping) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub configuration is already in use");
   }
   settings = std::move(value);
}

void plugin::impl::initialize(std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> value) {
   auto options = forge::net::p2p::pubsub::options{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopping) {
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub admission is closed");
      }
      if (initialized || initializing) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub is already initialized");
      }
      options = core_options_for(settings);
      initializing = true;
      ++active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] {
      {
         const auto lock = std::scoped_lock{mutex};
         initializing = false;
      }
      finish_operation();
   }};
   value->enable(std::move(options));
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopping) {
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P PubSub stopped during initialization");
      }
      source = std::move(value);
      initialized = true;
   }
}

void plugin::impl::request_stop() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      stopping = true;
   }
   // Active retired records also subscribe to this shared stop source.
   partial_stop.request_stop();
   // No allocations or callbacks under mutex. Stopping prevents new topics from entering.
   for (;;) {
      auto topic = std::shared_ptr<topic_state>{};
      {
         const auto lock = std::scoped_lock{mutex};
         for (const auto& [_, current] : topics) {
            if (!current->gate_closed) {
               current->gate_closed = true;
               topic = current;
               break;
            }
         }
      }
      if (!topic) { break; }
      topic->transition.close();
   }
   changed.notify();
}

boost::asio::awaitable<void> plugin::impl::ready_owned(std::shared_ptr<impl> self) {
   (void)self;
   co_return;
}

boost::asio::awaitable<void> plugin::impl::shutdown_owned(std::shared_ptr<impl> self) {
   co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       shutdown_joined(std::move(self)), boost::asio::use_awaitable);
}

boost::asio::awaitable<void> plugin::impl::shutdown_joined(std::shared_ptr<impl> self) {
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   self->request_stop();
   auto leader = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (!self->shutdown_running && !self->shutdown_done) {
         self->shutdown_running = true;
         leader = true;
      }
   }
   if (!leader) {
      for (;;) {
         const auto epoch = self->changed.epoch();
         auto done = false;
         auto error = std::exception_ptr{};
         {
            const auto lock = std::scoped_lock{self->mutex};
            done = self->shutdown_done;
            error = self->shutdown_error;
         }
         if (done) {
            if (error) { std::rethrow_exception(error); }
            co_return;
         }
         (void)co_await self->changed.async_wait(epoch);
      }
   }

   auto failure = std::exception_ptr{};
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
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
            handlers.swap(topic->handlers);
            partial = std::move(topic->partial);
            leave = topic->native_dirty;
         }
         handlers.clear();
         if (partial) { partial->stop.request_stop(); partial.reset(); }
         if (!leave) { continue; }
         auto error = std::exception_ptr{};
         try { co_await source->async_leave_topic(topic->subject); }
         catch (...) { error = std::current_exception(); }
         {
            const auto lock = std::scoped_lock{self->mutex};
            topic->leave_error = error;
            if (!error) { topic->native_dirty = false; topic->joined = false; }
         }
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
   // Dispose callback captures and the source before publishing actual shutdown completion.
   retired.clear();
   source.reset();
   {
      const auto lock = std::scoped_lock{self->mutex};
      self->shutdown_error = failure;
      self->shutdown_done = true;
      self->shutdown_running = false;
   }
   self->changed.notify();
   if (failure) { std::rethrow_exception(failure); }
}

} // namespace forge::plugins::net::p2p::pubsub
