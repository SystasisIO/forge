module;

#include <forge/api/core/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

export module forge.plugins.swarm.node.api;

export import forge.swarm.runtime.engine;
export import forge.swarm.storage.catalog;
export import forge.app.plugin_lifecycle;
export import forge.net.p2p.identity;
export import forge.multiformats.multiaddr;
import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.dispatcher;
import forge.api.core.error_projection;
import forge.api.core.exceptions;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;

export namespace forge::plugins::swarm::node {

class api : public forge::api::core::contract<api> {
 public:
   virtual ~api() = default;
   [[nodiscard]] virtual forge::app::lifecycle_snapshot status() const = 0;
   virtual boost::asio::awaitable<forge::swarm::runtime::sharing> async_share(
      forge::swarm::core::descriptor, std::shared_ptr<forge::swarm::core::store>, std::string realm = {}) = 0;
   virtual boost::asio::awaitable<forge::swarm::runtime::download> async_download(
      forge::swarm::core::descriptor, std::shared_ptr<forge::swarm::core::store>, std::string realm = {},
      std::vector<std::unique_ptr<forge::swarm::runtime::capabilities::route>> routes = {},
      std::optional<std::chrono::steady_clock::time_point> total_deadline = std::nullopt) = 0;
   [[nodiscard]] virtual std::unique_ptr<forge::swarm::runtime::capabilities::route> make_route(
      forge::net::p2p::peer_id, std::vector<forge::multiformats::multiaddr>) const = 0;
   virtual boost::asio::awaitable<forge::swarm::db::content> async_create(
      forge::swarm::core::descriptor, std::string locator, forge::swarm::storage::catalog::job_settings,
      std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<std::optional<forge::swarm::db::content>> async_find(
      forge::swarm::core::swarm_id, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<void> async_update(
      forge::swarm::core::swarm_id, forge::swarm::storage::catalog::job_settings, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<void> async_erase(forge::swarm::core::swarm_id, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<forge::db::object::object_page<forge::swarm::db::content>> async_by_state(
      forge::swarm::db::job_state, forge::db::core::page_request = {.limit = 16}, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<forge::db::object::object_page<forge::swarm::db::content>> async_expiring_before(
      std::int64_t, forge::db::core::page_request = {.limit = 16}, std::stop_token = {}) = 0;
   virtual boost::asio::awaitable<std::shared_ptr<forge::swarm::core::checkpoint_store>> async_checkpoint_store(
      forge::swarm::core::swarm_id, std::string locator, std::stop_token = {}) = 0;
};

} // namespace forge::plugins::swarm::node

FORGE_EXPORT_API(::forge::plugins::swarm::node::api, FORGE_API_CONTRACT("forge.plugins.swarm.node", 1, 0))
