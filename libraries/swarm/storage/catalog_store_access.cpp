module;

#include <boost/asio/awaitable.hpp>
#include <utility>

module forge.swarm.storage.catalog;

import forge.db.core.driver;

#include "details/catalog_store_access.hxx"

namespace forge::swarm::storage::detail {

catalog_store_access::catalog_store_access(forge::db::object::store objects) : _objects{std::move(objects)} {}
forge::db::object::store::options catalog_store_access::runtime_options() const { return _objects.runtime_options(); }
boost::asio::awaitable<forge::db::object::snapshot> catalog_store_access::begin_read() { return _objects.begin_read(); }
boost::asio::awaitable<forge::db::object::transaction> catalog_store_access::begin_transaction() {
   return _objects.begin_transaction();
}
boost::asio::awaitable<void> catalog_store_access::flush() { return _objects.driver()->async_flush(true); }

} // namespace forge::swarm::storage::detail
