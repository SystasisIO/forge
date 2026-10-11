#pragma once

namespace forge::plugins::net::p2p::pubsub {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);

   boost::asio::awaitable<message> publish(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data,
                                           publish_options options) override;
   boost::asio::awaitable<subscription> subscribe(forge::net::p2p::pubsub::topic subject, handler callback,
                                                  subscribe_options options) override;
   boost::asio::awaitable<void> unsubscribe(subscription value) override;
   boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> enable_partial(
       forge::net::p2p::pubsub::topic subject, handler full_fallback,
       forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options) override;
   boost::asio::awaitable<void> disable_partial(forge::net::p2p::pubsub::partial_topic token) override;
   boost::asio::awaitable<void> advertise_partial(forge::net::p2p::pubsub::partial_topic token,
                                                 std::vector<std::uint8_t> group) override;
   boost::asio::awaitable<void> forget_partial(forge::net::p2p::pubsub::partial_topic token,
                                              std::vector<std::uint8_t> group) override;
   boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>>
   partial_peers(forge::net::p2p::pubsub::partial_topic token) override;
   boost::asio::awaitable<void> send_partial(forge::net::p2p::pubsub::partial_topic token,
       forge::net::p2p::peer_id peer, forge::net::p2p::pubsub::partial_message value, std::stop_token stop) override;
   [[nodiscard]] std::vector<subscription> subscriptions() const override;
   [[nodiscard]] ::forge::plugins::net::p2p::pubsub::snapshot snapshot() const override;

 private:
   static boost::asio::awaitable<message> publish_owned(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, publish_options options);
   static boost::asio::awaitable<subscription> subscribe_owned(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::topic subject, handler callback, subscribe_options options);
   static boost::asio::awaitable<void> unsubscribe_owned(std::shared_ptr<plugin::impl> self, subscription value);
   static boost::asio::awaitable<subscription> subscribe_transition(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::topic subject, handler callback, subscribe_options options);
   static boost::asio::awaitable<void> unsubscribe_transition(std::shared_ptr<plugin::impl> self, subscription value);
   static boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> enable_partial_owned(
       std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject, handler fallback,
       forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options);
   static boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> enable_partial_transition(
       std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::topic subject, handler fallback,
       forge::net::p2p::pubsub::partial_options options, subscribe_options fallback_options);
   static boost::asio::awaitable<void> disable_partial_owned(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::partial_topic token);
   static boost::asio::awaitable<void> disable_partial_transition(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::partial_topic token);
   static boost::asio::awaitable<void> group_owned(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group, bool advertise);
   static boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> peers_owned(
       std::shared_ptr<plugin::impl> self, forge::net::p2p::pubsub::partial_topic token);
   static boost::asio::awaitable<void> send_owned(std::shared_ptr<plugin::impl> self,
       forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer,
       forge::net::p2p::pubsub::partial_message value, std::stop_token stop);
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::pubsub
