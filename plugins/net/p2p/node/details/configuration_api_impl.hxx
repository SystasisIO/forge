#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::configuration_api_impl final : public configuration_api {
 public:
   explicit configuration_api_impl(std::shared_ptr<plugin::impl> state);
   void set_connection_gater(std::shared_ptr<forge::net::p2p::connection_gater> gater) override;
   void set_pubsub_options(forge::net::p2p::pubsub::options options) override;
   void set_dht_value_policies(forge::net::p2p::protocol_id profile,
                              std::vector<forge::net::p2p::dht::value_policy> policies) override;

 private:
   std::shared_ptr<plugin::impl> state_;
};

} // namespace forge::plugins::net::p2p::node
