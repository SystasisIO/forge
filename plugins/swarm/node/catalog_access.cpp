module;

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <utility>

module forge.plugins.swarm.node.plugin;

import forge.plugins.db.store.api;
import forge.swarm.storage.catalog;
#include "details/catalog_access.hxx"

namespace forge::plugins::swarm::node {

catalog_access::catalog_access(forge::plugins::db::store::object_handle objects,
   std::shared_ptr<forge::plugins::db::store::api> database)
   : _objects{std::move(objects)}, _database{std::move(database)} {}
forge::db::object::store::options catalog_access::runtime_options() const { return _objects.runtime_options(); }
boost::asio::awaitable<forge::db::object::snapshot> catalog_access::begin_read() { return _objects.begin_read(); }
boost::asio::awaitable<forge::db::object::transaction> catalog_access::begin_transaction() {
   return _objects.begin_transaction();
}
boost::asio::awaitable<void> catalog_access::flush() { return _database->flush(_objects.name(), true); }

} // namespace forge::plugins::swarm::node
