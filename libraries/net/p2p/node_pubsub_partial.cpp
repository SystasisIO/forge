module;

#include <boost/compat/move_only_function.hpp>

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

module forge.net.p2p.node;
import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/owner_cancellation.hxx"

namespace forge::net::p2p {

boost::asio::awaitable<pubsub::partial_topic> node::async_subscribe(pubsub::topic subject,
    pubsub::handler full_handler, pubsub::partial_options options) {
   co_return (co_await impl_->subscribe_pubsub(std::move(subject), std::move(full_handler), std::move(options))).second;
}

boost::asio::awaitable<void> node::async_unsubscribe(pubsub::partial_topic registration) {
   const auto subject = registration.subject();
   co_await impl_->unsubscribe_pubsub(subject, std::move(registration));
}

boost::asio::awaitable<void> node::async_advertise_partial(pubsub::partial_topic registration,
                                                        std::vector<std::uint8_t> group_id) {
   const auto self = impl_;
   auto operation = self->lifecycle.track();
   if (!operation.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "Partial advertisement owner closed"); }
   const auto current = self->pubsub_value.partial.require(registration);
   if (!current->options.gossip) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Partial advertisement requires a gossip callback");
   }
   self->pubsub_value.partial.advertise(registration, std::move(group_id), self->options.limits.pubsub.limits);
   co_return;
}

boost::asio::awaitable<void> node::async_forget_partial(pubsub::partial_topic registration,
                                                     std::vector<std::uint8_t> group_id) {
   const auto self = impl_;
   auto operation = self->lifecycle.track();
   if (!operation.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "Partial advertisement owner closed"); }
   self->pubsub_value.partial.forget(registration, group_id);
   co_return;
}

boost::asio::awaitable<std::vector<peer_id>> node::async_partial_peers(pubsub::partial_topic registration) {
   const auto self = impl_;
   auto operation = self->lifecycle.track();
   if (!operation.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "Partial peer snapshot owner closed"); }
   const auto lock = std::scoped_lock{self->mutex};
   co_return self->partial_peers_locked(registration, false);
}

boost::asio::awaitable<void> node::async_send_partial(pubsub::partial_topic registration, peer_id peer,
                                                   pubsub::partial_message value, std::stop_token stop) {
   const auto self = impl_;
   auto operation = self->lifecycle.track();
   if (!operation.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "Partial send owner closed"); }
   const auto current = self->pubsub_value.partial.require(registration);
   if (value.subject && *value.subject != registration.subject()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Partial topic conflicts with its registration");
   }
   if (!value.group_id) { FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Partial send requires a group ID"); }
   value.subject = registration.subject();
   auto bridge = std::make_shared<detail::worker_stop_bridge>();
   const auto request_stop = [bridge]() noexcept { bridge->request_stop(); };
   const auto caller_stop = std::stop_callback{stop, request_stop};
   const auto registration_stop = std::stop_callback{current->stop.get_token(), request_stop};
   auto written = false;
   co_await boost::asio::co_spawn(operation.executor(), detail::async_run_with_owner_cancellation(bridge,
       [self, registration = std::move(registration), peer = std::move(peer), value = std::move(value), bridge, &written]
       (boost::asio::cancellation_slot slot) mutable -> boost::asio::awaitable<void> {
          if (bridge->stop_requested()) { FORGE_THROW_EXCEPTION(exceptions::canceled, "Partial send canceled before admission"); }
          auto generation = std::optional<std::uint64_t>{};
          const auto sent = co_await self->send_pubsub_rpc(peer, pubsub::rpc{.partial = std::move(value)}, generation,
              {}, true, {}, registration, bridge, slot);
          if (!sent) { FORGE_THROW_EXCEPTION(exceptions::closed, "Partial send registration or stream retired"); }
          written = true;
       }, detail::worker_stop_bridge_options{.lifecycle_stop = operation.stop_source()}), boost::asio::use_awaitable);
   // The bridge joins work, but an observed stop can skip it or consume its cancellation exception.
   if (!written) { FORGE_THROW_EXCEPTION(exceptions::canceled, "Partial send stopped without a successful native write"); }
}

} // namespace forge::net::p2p
