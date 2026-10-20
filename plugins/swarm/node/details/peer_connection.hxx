#pragma once

namespace forge::plugins::swarm::node {

struct capability_context;

class peer_connection final : public forge::swarm::runtime::capabilities::connection {
 public:
   static boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> open(
      std::shared_ptr<const capability_context>, forge::net::p2p::peer_id,
      std::vector<forge::multiformats::multiaddr>, std::stop_token, std::chrono::steady_clock::time_point);
   forge::api::core::handle<forge::swarm::api::peer> remote() const override;
   forge::api::auth::authenticated_caller remote_identity() const override;
   void cancel() noexcept override;
   boost::asio::awaitable<void> async_close() override;
 private:
   static boost::asio::awaitable<int> open_native(peer_connection*, std::shared_ptr<const capability_context>,
      forge::net::p2p::peer_id, std::vector<forge::multiformats::multiaddr>, std::chrono::steady_clock::time_point);
   forge::api::transport::connection _native;
   forge::api::core::handle<forge::swarm::api::peer> _remote;
   forge::api::auth::authenticated_caller _identity;
};

} // namespace forge::plugins::swarm::node
