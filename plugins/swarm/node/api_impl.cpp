module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.api;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.db.store.api;
import forge.api.p2p.publication;
import forge.net.p2p.protocol;
import forge.net.p2p.dht;
#include "details/plugin_impl.hxx"
#include "details/peer_capabilities.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::swarm::node {

plugin::api_impl::api_impl(std::shared_ptr<impl> owner) : _owner{std::move(owner)} {}
forge::app::lifecycle_snapshot plugin::api_impl::status() const { return _owner->lifecycle.snapshot(); }
boost::asio::awaitable<forge::swarm::runtime::sharing> plugin::api_impl::async_share(
   forge::swarm::core::descriptor descriptor, std::shared_ptr<forge::swarm::core::store> store, std::string realm) {
   return _owner->require_engine()->async_share(std::move(descriptor), std::move(store), std::move(realm));
}
boost::asio::awaitable<forge::swarm::runtime::download> plugin::api_impl::async_download(
   forge::swarm::core::descriptor descriptor, std::shared_ptr<forge::swarm::core::store> store, std::string realm,
   std::vector<std::unique_ptr<forge::swarm::runtime::capabilities::route>> routes,
   std::optional<std::chrono::steady_clock::time_point> deadline) {
   return _owner->require_engine()->async_download(std::move(descriptor), std::move(store),
      std::move(realm), std::move(routes), deadline);
}
std::unique_ptr<forge::swarm::runtime::capabilities::route> plugin::api_impl::make_route(
   forge::net::p2p::peer_id peer, std::vector<forge::multiformats::multiaddr> addresses) const {
   return _owner->require_capabilities()->make_route(std::move(peer), std::move(addresses));
}
boost::asio::awaitable<forge::swarm::db::content> plugin::api_impl::async_create(
   forge::swarm::core::descriptor descriptor, std::string locator,
   forge::swarm::storage::catalog::job_settings settings, std::stop_token stop) {
   return _owner->require_catalog()->async_create(std::move(descriptor), std::move(locator), settings, stop);
}
boost::asio::awaitable<std::optional<forge::swarm::db::content>> plugin::api_impl::async_find(
   forge::swarm::core::swarm_id id, std::stop_token stop) { return _owner->require_catalog()->async_find(id, stop); }
boost::asio::awaitable<void> plugin::api_impl::async_update(forge::swarm::core::swarm_id id,
   forge::swarm::storage::catalog::job_settings settings, std::stop_token stop) {
   return _owner->require_catalog()->async_update(id, settings, stop);
}
boost::asio::awaitable<void> plugin::api_impl::async_erase(forge::swarm::core::swarm_id id, std::stop_token stop) {
   return _owner->require_catalog()->async_erase(id, stop);
}
boost::asio::awaitable<forge::db::object::object_page<forge::swarm::db::content>> plugin::api_impl::async_by_state(
   forge::swarm::db::job_state state, forge::db::core::page_request page, std::stop_token stop) {
   return _owner->require_catalog()->async_by_state(state, std::move(page), stop);
}
boost::asio::awaitable<forge::db::object::object_page<forge::swarm::db::content>> plugin::api_impl::async_expiring_before(
   std::int64_t exclusive, forge::db::core::page_request page, std::stop_token stop) {
   return _owner->require_catalog()->async_expiring_before(exclusive, std::move(page), stop);
}
boost::asio::awaitable<std::shared_ptr<forge::swarm::core::checkpoint_store>> plugin::api_impl::async_checkpoint_store(
   forge::swarm::core::swarm_id id, std::string locator, std::stop_token stop) {
   return _owner->require_catalog()->async_checkpoint_store(id, std::move(locator), stop);
}

} // namespace forge::plugins::swarm::node
