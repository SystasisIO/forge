#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);

   forge::net::p2p::peer_id local_peer() const override;
   std::optional<forge::net::p2p::endpoint> local_endpoint() const override;
   std::vector<forge::net::p2p::endpoint> local_endpoints() const override;
   info network_info() const override;
   boost::asio::awaitable<forge::net::p2p::node::session_info>
   connect(forge::net::p2p::endpoint endpoint, forge::net::p2p::node::connect_options options) override;
   boost::asio::awaitable<forge::net::p2p::node::session_info>
   connect(forge::multiformats::multiaddr address, forge::net::p2p::node::connect_options options) override;
   boost::asio::awaitable<std::chrono::milliseconds>
   ping(forge::net::p2p::peer_id peer, forge::net::p2p::node::open_options options) override;
   bool is_peer_protected(const forge::net::p2p::peer_id& peer) const override;
   peer_contribution protect_peer(forge::net::p2p::peer_id peer) override;
   peer_contribution tag_peer(forge::net::p2p::peer_id peer, std::int64_t value) override;
   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol) override;
   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
               forge::api::transport::options options) override;
   boost::asio::awaitable<forge::api::transport::connection>
   open_api_connection(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol, remote_options options) override;

 private:
   static boost::asio::awaitable<forge::net::p2p::node::session_info> connect_owned(
       std::shared_ptr<plugin::impl> owner, forge::net::p2p::endpoint endpoint,
       forge::net::p2p::node::connect_options options);
   static boost::asio::awaitable<forge::net::p2p::node::session_info> connect_owned(
       std::shared_ptr<plugin::impl> owner, forge::multiformats::multiaddr address,
       forge::net::p2p::node::connect_options options);
   static boost::asio::awaitable<std::chrono::milliseconds> ping_owned(
       std::shared_ptr<plugin::impl> owner, forge::net::p2p::peer_id peer,
       forge::net::p2p::node::open_options options);
   static boost::asio::awaitable<forge::api::transport::connection> open_owned(
       std::shared_ptr<plugin::impl> owner, forge::net::p2p::peer_id peer,
       forge::net::p2p::protocol_id protocol, remote_options options);
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
