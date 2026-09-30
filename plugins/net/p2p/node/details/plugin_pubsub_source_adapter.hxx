#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::pubsub_source_adapter final : public pubsub_source {
 public:
   explicit pubsub_source_adapter(std::shared_ptr<plugin::impl> impl);

   void enable(forge::net::p2p::pubsub::options options) override;
   forge::net::p2p::peer_id local_peer() const override;
   boost::asio::awaitable<forge::net::p2p::pubsub::message>
   async_publish_message(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data,
                         forge::net::p2p::pubsub::publish_options options) override;
   boost::asio::awaitable<forge::net::p2p::pubsub::subscription>
   async_join_topic(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler handler) override;
   boost::asio::awaitable<void> async_leave_topic(forge::net::p2p::pubsub::topic subject) override;
   forge::net::p2p::pubsub::snapshot snapshot() const override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
