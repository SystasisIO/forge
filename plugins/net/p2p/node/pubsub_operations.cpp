module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.exceptions;

import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.pubsub_exceptions;

#include "details/pubsub_operations.hxx"

namespace forge::plugins::net::p2p::node::detail {

pubsub_operations::pubsub_operations()
    : publish{[](owner node, forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> bytes,
                 forge::net::p2p::pubsub::publish_options options) {
         return node->async_publish(std::move(subject), std::move(bytes), options);
      }},
      subscribe{[](owner node, forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback) {
         return node->async_subscribe(std::move(subject), std::move(callback));
      }},
      unsubscribe{[](owner node, forge::net::p2p::pubsub::topic subject) {
         return node->async_unsubscribe(std::move(subject));
      }},
      enable_partial{[](owner node, forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler fallback,
                        forge::net::p2p::pubsub::partial_options options) {
         return node->async_subscribe(std::move(subject), std::move(fallback), std::move(options));
      }},
      disable_partial{[](owner node, forge::net::p2p::pubsub::partial_topic token) {
         return node->async_disable_partial(std::move(token));
      }},
      advertise_partial{[](owner node, forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
         return node->async_advertise_partial(std::move(token), std::move(group));
      }},
      forget_partial{[](owner node, forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
         return node->async_forget_partial(std::move(token), std::move(group));
      }},
      partial_peers{[](owner node, forge::net::p2p::pubsub::partial_topic token) {
         return node->async_partial_peers(std::move(token));
      }},
      send_partial{[](owner node, forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer,
                      forge::net::p2p::pubsub::partial_message value, std::stop_token stop) {
         return node->async_send_partial(std::move(token), std::move(peer), std::move(value), stop);
      }} {}

void pubsub_operations::validate() const {
   if (!publish || !subscribe || !unsubscribe || !enable_partial || !disable_partial || !advertise_partial ||
       !forget_partial || !partial_peers || !send_partial) {
      FORGE_THROW_EXCEPTION(pubsub_exceptions::invalid_config, "P2P PubSub native operations are incomplete");
   }
}

} // namespace forge::plugins::net::p2p::node::detail
