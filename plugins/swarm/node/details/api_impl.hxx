#pragma once

namespace forge::plugins::swarm::node {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<impl>);
   forge::app::lifecycle_snapshot status() const override;
   boost::asio::awaitable<forge::swarm::runtime::sharing> async_share(
      forge::swarm::core::descriptor, std::shared_ptr<forge::swarm::core::store>, std::string) override;
   boost::asio::awaitable<forge::swarm::runtime::download> async_download(
      forge::swarm::core::descriptor, std::shared_ptr<forge::swarm::core::store>, std::string,
      std::vector<std::unique_ptr<forge::swarm::runtime::capabilities::route>>,
      std::optional<std::chrono::steady_clock::time_point>) override;
   std::unique_ptr<forge::swarm::runtime::capabilities::route> make_route(
      forge::net::p2p::peer_id, std::vector<forge::multiformats::multiaddr>) const override;
   boost::asio::awaitable<forge::swarm::db::content> async_create(forge::swarm::core::descriptor,
      std::string, forge::swarm::storage::catalog::job_settings, std::stop_token) override;
   boost::asio::awaitable<std::optional<forge::swarm::db::content>> async_find(
      forge::swarm::core::swarm_id, std::stop_token) override;
   boost::asio::awaitable<void> async_update(forge::swarm::core::swarm_id,
      forge::swarm::storage::catalog::job_settings, std::stop_token) override;
   boost::asio::awaitable<void> async_erase(forge::swarm::core::swarm_id, std::stop_token) override;
   boost::asio::awaitable<forge::db::object::object_page<forge::swarm::db::content>> async_by_state(
      forge::swarm::db::job_state, forge::db::core::page_request, std::stop_token) override;
   boost::asio::awaitable<forge::db::object::object_page<forge::swarm::db::content>> async_expiring_before(
      std::int64_t, forge::db::core::page_request, std::stop_token) override;
   boost::asio::awaitable<std::shared_ptr<forge::swarm::core::checkpoint_store>> async_checkpoint_store(
      forge::swarm::core::swarm_id, std::string, std::stop_token) override;
 private:
   std::shared_ptr<impl> _owner;
};

} // namespace forge::plugins::swarm::node
