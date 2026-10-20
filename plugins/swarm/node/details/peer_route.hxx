#pragma once

namespace forge::plugins::swarm::node {

struct capability_context;

class peer_route final : public forge::swarm::runtime::capabilities::route {
 public:
   peer_route(std::shared_ptr<const capability_context>, forge::net::p2p::peer_id,
      std::vector<forge::multiformats::multiaddr>);
   std::string_view key() const noexcept override;
   boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> async_connect(
      std::stop_token, std::chrono::steady_clock::time_point) const override;
 private:
   struct context {
      std::shared_ptr<const capability_context> backend;
      forge::net::p2p::peer_id peer;
      std::vector<forge::multiformats::multiaddr> addresses;
      std::string key;
   };
   static boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> connect_owned(
      std::shared_ptr<const context>, std::stop_token, std::chrono::steady_clock::time_point);
   std::shared_ptr<const context> _context;
};

} // namespace forge::plugins::swarm::node
