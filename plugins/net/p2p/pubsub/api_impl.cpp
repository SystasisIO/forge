module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

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

boost::asio::awaitable<message>
plugin::api_impl::publish(forge::net::p2p::pubsub::topic subject,
                          std::vector<std::uint8_t> data,
                          publish_options options) {
   auto& source = impl_->require_source();
   impl_->ensure_topic_allowed(subject);
   if (data.size() > impl_->settings.max_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::message_too_large, "P2P PubSub message exceeds configured limit",
                          forge::exceptions::ctx("topic", subject.value));
   }
   auto published = co_await source.async_publish_message(
      std::move(subject), std::move(data),
      forge::net::p2p::pubsub::publish_options{.sign = options.sign.value_or(impl_->settings.sign_publishes)});
   {
      auto lock = std::scoped_lock{impl_->mutex};
      ++impl_->messages_published;
   }
   co_return project_message(source.local_peer(), published);
}

boost::asio::awaitable<subscription>
plugin::api_impl::subscribe(forge::net::p2p::pubsub::topic subject,
                            handler callback,
                            subscribe_options options) {
   auto& source = impl_->require_source();
   impl_->ensure_topic_allowed(subject);
   if (!callback) {
      FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub subscription requires handler");
   }

   auto value = subscription{};
   auto join_leader = false;
   auto waiter = std::shared_ptr<join_waiter>{};
   const auto executor = co_await boost::asio::this_coro::executor;
   {
      auto lock = std::scoped_lock{impl_->mutex};
      const auto new_topic = !impl_->topics.contains(subject.value);
      if (new_topic && impl_->topics.size() >= impl_->settings.max_topics) {
         FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub topic limit reached",
                             forge::exceptions::ctx("topic", subject.value));
      }
      auto& state = impl_->topics[subject.value];
      if (state.handlers.size() >= impl_->settings.max_handlers_per_topic) {
         FORGE_THROW_EXCEPTION(exceptions::handler_limit, "P2P PubSub handler limit reached",
                             forge::exceptions::ctx("topic", subject.value));
      }
      value = subscription{.id = impl_->next_subscription++, .subject = subject};
      auto deadline = options.handler_deadline;
      if (deadline.count() <= 0) {
         deadline = to_ms(impl_->settings.handler_deadline_ms);
      }
      state.handlers.emplace(value.id, handler_record{
                                          .id = value.id,
                                          .subject = subject,
                                          .callback = std::move(callback),
                                          .deadline = deadline,
                                       });
      if (!state.joined) {
         if (state.joining) {
            waiter = std::make_shared<join_waiter>(executor);
            state.waiters.push_back(waiter);
         } else {
            state.joining = true;
            join_leader = true;
         }
      }
   }

   if (waiter) {
      while (!waiter->ready) {
         auto error = boost::system::error_code{};
         co_await waiter->timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      if (waiter->error) {
         std::rethrow_exception(waiter->error);
      }
      co_return value;
   }

   if (join_leader) {
      auto self = impl_;
      try {
         (void)co_await source.async_join_topic(
            subject, [self](forge::net::p2p::pubsub::event event) mutable
                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
               co_return co_await self->handle_event(std::move(event));
            });
         auto waiters = std::vector<std::shared_ptr<join_waiter>>{};
         {
            auto lock = std::scoped_lock{impl_->mutex};
            if (auto found = impl_->topics.find(value.subject.value); found != impl_->topics.end()) {
               found->second.joined = true;
               found->second.joining = false;
               waiters = std::move(found->second.waiters);
            }
         }
         for (auto& pending : waiters) {
            pending->complete();
         }
      } catch (...) {
         auto failure = std::current_exception();
         auto waiters = std::vector<std::shared_ptr<join_waiter>>{};
         {
            auto lock = std::scoped_lock{impl_->mutex};
            if (auto found = impl_->topics.find(value.subject.value); found != impl_->topics.end()) {
               waiters = std::move(found->second.waiters);
               impl_->topics.erase(found);
            }
         }
         for (auto& pending : waiters) {
            pending->complete(failure);
         }
         throw;
      }
   }
   co_return value;
}

boost::asio::awaitable<void> plugin::api_impl::unsubscribe(subscription value) {
   auto& source = impl_->require_source();
   auto last_for_topic = false;
   {
      auto lock = std::scoped_lock{impl_->mutex};
      auto found = impl_->topics.find(value.subject.value);
      if (found == impl_->topics.end() || !found->second.handlers.erase(value.id)) {
         FORGE_THROW_EXCEPTION(exceptions::subscription_not_found, "P2P PubSub subscription was not found",
                             forge::exceptions::ctx("topic", value.subject.value));
      }
      last_for_topic = found->second.handlers.empty();
      if (last_for_topic) {
         impl_->topics.erase(found);
      }
   }
   if (last_for_topic) {
      co_await source.async_leave_topic(std::move(value.subject));
   }
}

std::vector<subscription> plugin::api_impl::subscriptions() const {
   (void)impl_->require_source();
   auto lock = std::scoped_lock{impl_->mutex};
   auto out = std::vector<subscription>{};
   for (const auto& [_, topic] : impl_->topics) {
      for (const auto& [id, handler] : topic.handlers) {
         out.push_back(subscription{.id = id, .subject = handler.subject});
      }
   }
   return out;
}

::forge::plugins::net::p2p::pubsub::snapshot plugin::api_impl::snapshot() const {
   auto& source = impl_->require_source();
   auto lock = std::scoped_lock{impl_->mutex};
   auto subscriptions = std::size_t{};
   for (const auto& [_, topic] : impl_->topics) {
      subscriptions += topic.handlers.size();
   }
   return ::forge::plugins::net::p2p::pubsub::snapshot{
      .topics = impl_->topics.size(),
      .subscriptions = subscriptions,
      .active_handlers = impl_->active_handlers,
      .messages_published = impl_->messages_published,
      .messages_delivered = impl_->messages_delivered,
      .messages_accepted = impl_->messages_accepted,
      .messages_rejected = impl_->messages_rejected,
      .messages_ignored = impl_->messages_ignored,
      .messages_retried = impl_->messages_retried,
      .messages_dropped = impl_->messages_dropped,
      .handler_failures = impl_->handler_failures,
      .core = source.snapshot(),
   };
}

} // namespace forge::plugins::net::p2p::pubsub
