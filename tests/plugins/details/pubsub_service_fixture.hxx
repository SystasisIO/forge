#pragma once

namespace forge::plugins::net::p2p::node::test {

// Exercises the production registration owner through its private native-operation boundary.
// It is not installed and is never registered as an application API provider.
class pubsub_service_fixture {
 public:
   pubsub_service_fixture(forge::asio::runtime& runtime, detail::pubsub_operations operations,
                          pubsub_config settings);
   [[nodiscard]] std::shared_ptr<pubsub_api> interface();
   [[nodiscard]] const std::shared_ptr<forge::net::p2p::node>& native() const noexcept;
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();

 private:
   std::shared_ptr<forge::net::p2p::node> native_;
   std::shared_ptr<detail::pubsub_service> owner_;
};

}
