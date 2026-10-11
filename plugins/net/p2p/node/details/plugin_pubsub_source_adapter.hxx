#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::pubsub_source_adapter final : public pubsub_source {
 public:
   explicit pubsub_source_adapter(std::shared_ptr<plugin::impl> impl);
   void enable(forge::net::p2p::pubsub::options options) override;
   forge::net::p2p::peer_id local_peer() const override;
   boost::asio::awaitable<forge::net::p2p::pubsub::message> async_publish_message(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, forge::net::p2p::pubsub::publish_options options) override;
   boost::asio::awaitable<forge::net::p2p::pubsub::subscription> async_join_topic(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback) override;
   boost::asio::awaitable<void> async_leave_topic(forge::net::p2p::pubsub::topic subject) override;
   boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> async_enable_partial(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback, forge::net::p2p::pubsub::partial_options options) override;
   boost::asio::awaitable<void> async_disable_partial(forge::net::p2p::pubsub::partial_topic token) override;
   boost::asio::awaitable<void> async_advertise_partial(forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) override;
   boost::asio::awaitable<void> async_forget_partial(forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) override;
   boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> async_partial_peers(forge::net::p2p::pubsub::partial_topic token) override;
   boost::asio::awaitable<void> async_send_partial(forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer, forge::net::p2p::pubsub::partial_message value, std::stop_token stop) override;
   forge::net::p2p::pubsub::snapshot snapshot() const override;

 private:
   static boost::asio::awaitable<forge::net::p2p::pubsub::message> publish_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, forge::net::p2p::pubsub::publish_options options);
   static boost::asio::awaitable<forge::net::p2p::pubsub::subscription> join_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback);
   static boost::asio::awaitable<void> leave_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject);
   static boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> enable_partial_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback, forge::net::p2p::pubsub::partial_options options);
   static boost::asio::awaitable<void> disable_partial_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token);
   static boost::asio::awaitable<void> advertise_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group);
   static boost::asio::awaitable<void> forget_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group);
   static boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> peers_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token);
   static boost::asio::awaitable<void> send_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer, forge::net::p2p::pubsub::partial_message value, std::stop_token stop);
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
