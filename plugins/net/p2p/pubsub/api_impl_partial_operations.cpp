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
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

boost::asio::awaitable<void> plugin::api_impl::advertise_partial(forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
   return group_owned(impl_, std::move(token), std::move(group), true);
}

boost::asio::awaitable<void> plugin::api_impl::forget_partial(forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
   return group_owned(impl_, std::move(token), std::move(group), false);
}

boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> plugin::api_impl::partial_peers(forge::net::p2p::pubsub::partial_topic token) {
   return peers_owned(impl_, std::move(token));
}

boost::asio::awaitable<void> plugin::api_impl::send_partial(forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer, forge::net::p2p::pubsub::partial_message value, std::stop_token stop) {
   return send_owned(impl_, std::move(token), std::move(peer), std::move(value), stop);
}

boost::asio::awaitable<void> plugin::api_impl::group_owned(std::shared_ptr<plugin::impl> self,
    forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group, bool advertise) {
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   auto topic = std::shared_ptr<topic_state>{};
   auto record = std::shared_ptr<partial_record>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      record = self->require_partial_locked(token);
      topic = self->topics.at(token.subject().value);
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_partial_operation(topic, record); }};
   auto ticket = co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       topic->transition.acquire(), boost::asio::use_awaitable);
   {
      const auto lock = std::scoped_lock{self->mutex};
      static_cast<void>(self->require_source_locked());
      if (self->require_partial_locked(token) != record) {
         FORGE_THROW_EXCEPTION(exceptions::subscription_not_found, "Partial owner changed while waiting");
      }
   }

   if (advertise) { co_await source->async_advertise_partial(std::move(token), std::move(group)); }
   else { co_await source->async_forget_partial(std::move(token), std::move(group)); }
}

boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> plugin::api_impl::peers_owned(
    std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::partial_topic token) {
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   auto topic = std::shared_ptr<topic_state>{};
   auto record = std::shared_ptr<partial_record>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      record = self->require_partial_locked(token);
      topic = self->topics.at(token.subject().value);
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_partial_operation(topic, record); }};
   auto ticket = co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
       topic->transition.acquire(), boost::asio::use_awaitable);
   {
      const auto lock = std::scoped_lock{self->mutex};
      static_cast<void>(self->require_source_locked());
      if (self->require_partial_locked(token) != record) {
         FORGE_THROW_EXCEPTION(exceptions::subscription_not_found, "Partial owner changed while waiting");
      }
   }

   co_return co_await source->async_partial_peers(std::move(token));
}

boost::asio::awaitable<void> plugin::api_impl::send_owned(std::shared_ptr<plugin::impl> self,
    forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer,
    forge::net::p2p::pubsub::partial_message value, std::stop_token caller) {
   auto source = std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source>{};
   auto topic = std::shared_ptr<topic_state>{};
   auto record = std::shared_ptr<partial_record>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      source = self->require_source_locked();
      record = self->require_partial_locked(token);
      topic = self->topics.at(token.subject().value);
      ++topic->participants;
      ++self->active_operations;
   }
   auto completed = boost::scope::scope_exit{[&] { self->finish_partial_operation(topic, record); }};
   // Admission above owns this send, not the topic transition. A concurrent downgrade must be
   // able to signal record.stop while native I/O is blocked; native rechecks the exact token.
   auto stop = std::stop_source{};
   const auto signal = [stop]() mutable noexcept { stop.request_stop(); };
   const auto caller_stop = std::stop_callback{caller, signal};
   const auto record_stop = std::stop_callback{record->stop.get_token(), signal};
   const auto plugin_stop = std::stop_callback{self->partial_stop.get_token(), signal};
   if (stop.stop_requested()) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "Partial send canceled before native admission");
   }
   co_await source->async_send_partial(std::move(token), std::move(peer), std::move(value), stop.get_token());
}

} // namespace forge::plugins::net::p2p::pubsub
