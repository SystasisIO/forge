#pragma once

namespace forge::plugins::swarm::node {

struct capability_context;

class provider_registration final : public forge::swarm::runtime::capabilities::registration {
 public:
   static boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::registration>> open(
      std::shared_ptr<const capability_context>, forge::net::p2p::dht::key,
      std::stop_token, std::chrono::steady_clock::time_point);
   void cancel() noexcept override;
   boost::asio::awaitable<void> async_close() override;
 private:
   static boost::asio::awaitable<int> open_native(provider_registration*, std::shared_ptr<const capability_context>,
      forge::net::p2p::dht::key, std::chrono::steady_clock::time_point);
   forge::net::p2p::provider_registration _native;
};

} // namespace forge::plugins::swarm::node
