module;

#include <boost/asio/awaitable.hpp>

export module forge.package.db.registration.catalog;

import forge.db.object.registration;
import forge.plugins.db.store.api;

export namespace package_registration {

forge::db::object::registration metadata();
forge::db::object::registration conflicting_metadata();
forge::db::object::registration legacy_metadata();
void register_legacy(const forge::plugins::db::store::object_handle& objects);
boost::asio::awaitable<void> create_and_read(forge::plugins::db::store::store_handle store);

} // namespace package_registration
