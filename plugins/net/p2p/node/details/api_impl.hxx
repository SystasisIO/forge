#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);

   forge::net::p2p::peer_id local_peer() const override;
   std::optional<forge::net::p2p::endpoint> local_endpoint() const override;
   std::vector<forge::net::p2p::endpoint> local_endpoints() const override;
   info network_info() const override;
   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol) override;
   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
               forge::api::transport::options options) override;
   void publish_protocol(forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) override;
   boost::asio::awaitable<forge::api::transport::connection>
   open_api_connection(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol, remote_options options) override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
