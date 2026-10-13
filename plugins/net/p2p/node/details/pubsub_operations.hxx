#pragma once

#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace forge::plugins::net::p2p::node::detail {

// Private mutation boundary of the registration service, not an API provider.
// Every operation receives the native owner retained by its admitted caller.
struct pubsub_operations {
   using owner = std::shared_ptr<forge::net::p2p::node>;

   pubsub_operations();
   void validate() const;

   std::function<boost::asio::awaitable<forge::net::p2p::pubsub::message>(owner,
       forge::net::p2p::pubsub::topic, std::vector<std::uint8_t>, forge::net::p2p::pubsub::publish_options)> publish;
   std::function<boost::asio::awaitable<forge::net::p2p::pubsub::subscription>(owner,
       forge::net::p2p::pubsub::topic, forge::net::p2p::pubsub::handler)> subscribe;
   std::function<boost::asio::awaitable<void>(owner, forge::net::p2p::pubsub::topic)> unsubscribe;
   std::function<boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic>(owner,
       forge::net::p2p::pubsub::topic, forge::net::p2p::pubsub::handler,
       forge::net::p2p::pubsub::partial_options)> enable_partial;
   std::function<boost::asio::awaitable<void>(owner, forge::net::p2p::pubsub::partial_topic)> disable_partial;
   std::function<boost::asio::awaitable<void>(owner, forge::net::p2p::pubsub::partial_topic,
       std::vector<std::uint8_t>)> advertise_partial;
   std::function<boost::asio::awaitable<void>(owner, forge::net::p2p::pubsub::partial_topic,
       std::vector<std::uint8_t>)> forget_partial;
   std::function<boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>>(owner,
       forge::net::p2p::pubsub::partial_topic)> partial_peers;
   std::function<boost::asio::awaitable<void>(owner, forge::net::p2p::pubsub::partial_topic,
       forge::net::p2p::peer_id, forge::net::p2p::pubsub::partial_message, std::stop_token)> send_partial;
};

} // namespace forge::plugins::net::p2p::node::detail
