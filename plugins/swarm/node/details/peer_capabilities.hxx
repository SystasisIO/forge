#pragma once

namespace forge::plugins::swarm::node {

struct capability_context;

class peer_capabilities final : public forge::swarm::runtime::capabilities {
 public:
   explicit peer_capabilities(std::shared_ptr<const capability_context>);
   std::unique_ptr<route> make_route(forge::net::p2p::peer_id, std::vector<forge::multiformats::multiaddr>) const;
   boost::asio::awaitable<std::vector<std::unique_ptr<route>>> async_lookup(
      forge::swarm::core::swarm_id, std::string, std::size_t, std::stop_token,
      std::chrono::steady_clock::time_point) override;
   boost::asio::awaitable<std::unique_ptr<registration>> async_register(
      forge::swarm::core::swarm_id, std::string, std::stop_token, std::chrono::steady_clock::time_point) override;
 private:
   static boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>> lookup_native(
      std::shared_ptr<const capability_context>, forge::net::p2p::dht::key, std::size_t,
      std::chrono::steady_clock::time_point);
   static boost::asio::awaitable<std::vector<std::unique_ptr<route>>> lookup_owned(
      std::shared_ptr<const capability_context>, forge::swarm::core::swarm_id, std::string,
      std::size_t, std::stop_token, std::chrono::steady_clock::time_point);
   std::shared_ptr<const capability_context> _context;
};

} // namespace forge::plugins::swarm::node
