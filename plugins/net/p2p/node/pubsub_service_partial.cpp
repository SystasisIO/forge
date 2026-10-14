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

#include "details/pubsub_service.hxx"

namespace forge::plugins::net::p2p::node::detail {

void pubsub_service::finish_partial_operation(const std::shared_ptr<topic_state>& topic,
                                            std::shared_ptr<partial_record>& record) noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      --topic->participants;
      if (topic->participants == 0 && topic->handlers.empty() && !topic->partial && !topic->native_dirty) {
         const auto found = topics.find(topic->subject.value);
         if (found != topics.end() && found->second == topic) { topics.erase(found); }
      }
   }
   record.reset();
   finish_operation();
}

std::shared_ptr<partial_record> pubsub_service::require_partial_locked(
    const forge::net::p2p::pubsub::partial_topic& token, bool cleanup) const {
   const auto found = topics.find(token.subject().value);
   const auto record = found == topics.end() ? nullptr : found->second->partial;
   if (!record || record->token != token || record->removing ||
       (!(record->committed && record->admission) && !(cleanup && record->cleanup_pending))) {
      FORGE_THROW_EXCEPTION(exceptions::subscription_not_found, "P2P Partial registration is not current");
   }
   return record;
}

forge::net::p2p::pubsub::handler pubsub_service::full_dispatch(std::weak_ptr<pubsub_service> self) {
   return [self = std::move(self)](forge::net::p2p::pubsub::event event) {
      return handle_event_owned(self.lock(), std::move(event));
   };
}

std::shared_ptr<partial_record> pubsub_service::admit_partial_event_locked(const std::weak_ptr<partial_record>& expected,
                                                                       const forge::net::p2p::pubsub::partial_topic& token) {
   if (stopping || active_handlers >= settings.max_active_handlers) { return {}; }
   const auto found = topics.find(token.subject().value);
   if (found == topics.end()) { return {}; }
   const auto& record = found->second->partial;
   if (!record || !record->committed || !record->admission || record->token != token ||
       expected.owner_before(record) || record.owner_before(expected)) { return {}; }
   ++active_events;
   ++active_handlers;
   return record;
}

boost::asio::awaitable<void> pubsub_service::restore_full_owned(std::shared_ptr<pubsub_service> self,
    std::shared_ptr<topic_state> topic, std::shared_ptr<forge::net::p2p::node> source) {
   // Necessarily joined compensation child; its cancellation policy never escapes to the caller.
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   auto ordinary = false;
   {
      const auto lock = std::scoped_lock{self->mutex};
      for (const auto& [_, handler] : topic->handlers) { ordinary = ordinary || handler->committed; }
   }
   if (ordinary) { static_cast<void>(co_await self->operations.subscribe(source, topic->subject, full_dispatch(self))); }
   else { co_await self->operations.unsubscribe(source, topic->subject); }
   auto retired_join_error = std::exception_ptr{};
   auto retired_leave_error = std::exception_ptr{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      topic->joined = ordinary;
      topic->native_dirty = ordinary;
      std::swap(retired_join_error, topic->join_error);
      std::swap(retired_leave_error, topic->leave_error);
   }
}

boost::asio::awaitable<void> pubsub_service::receive_partial_owned(std::weak_ptr<pubsub_service> owner,
    std::weak_ptr<partial_record> expected, forge::net::p2p::pubsub::partial_event event, std::stop_token native) {
   const auto self = owner.lock();
   if (!self) { co_return; }
   auto record = std::shared_ptr<partial_record>{};
   const auto generation = self->lifecycle.snapshot().generation;
   if (!self->lifecycle.try_admit(generation, forge::app::lifecycle_phase::started, [&] {
      const auto lock = std::scoped_lock{self->mutex};
      record = self->admit_partial_event_locked(expected, event.registration);
   })) { co_return; }
   if (!record) { co_return; }
   auto completed = boost::scope::scope_exit{[&] {
      record.reset(); // Last user captures retire outside mutex, before publishing completion.
      {
         const auto lock = std::scoped_lock{self->mutex};
         --self->active_handlers;
         --self->active_events;
      }
      self->changed.notify();
   }};
   auto stop = std::stop_source{};
   const auto signal = [stop]() mutable noexcept { stop.request_stop(); };
   const auto native_stop = std::stop_callback{native, signal};
   const auto record_stop = std::stop_callback{record->stop.get_token(), signal};
   const auto plugin_stop = std::stop_callback{self->partial_stop.get_token(), signal};
   try { co_await record->callbacks.receive(std::move(event), stop.get_token()); }
   catch (...) { self->record_handler_failure(); throw; }
}

boost::asio::awaitable<void> pubsub_service::gossip_partial_owned(std::weak_ptr<pubsub_service> owner,
    std::weak_ptr<partial_record> expected, forge::net::p2p::pubsub::partial_gossip_event event, std::stop_token native) {
   const auto self = owner.lock();
   if (!self) { co_return; }
   auto record = std::shared_ptr<partial_record>{};
   const auto generation = self->lifecycle.snapshot().generation;
   if (!self->lifecycle.try_admit(generation, forge::app::lifecycle_phase::started, [&] {
      const auto lock = std::scoped_lock{self->mutex};
      record = self->admit_partial_event_locked(expected, event.registration);
   })) { co_return; }
   if (!record) { co_return; }
   auto completed = boost::scope::scope_exit{[&] {
      record.reset(); // Last user captures retire outside mutex, before publishing completion.
      {
         const auto lock = std::scoped_lock{self->mutex};
         --self->active_handlers;
         --self->active_events;
      }
      self->changed.notify();
   }};
   auto stop = std::stop_source{};
   const auto signal = [stop]() mutable noexcept { stop.request_stop(); };
   const auto native_stop = std::stop_callback{native, signal};
   const auto record_stop = std::stop_callback{record->stop.get_token(), signal};
   const auto plugin_stop = std::stop_callback{self->partial_stop.get_token(), signal};
   try { co_await record->callbacks.gossip(std::move(event), stop.get_token()); }
   catch (...) { self->record_handler_failure(); throw; }
}

} // namespace forge::plugins::net::p2p::node::detail
