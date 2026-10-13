module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <memory>
#include <string>
#include <utility>

module forge.plugins.db.store.plugin;

import forge.db.blob.store;
import forge.db.object.hooks;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.db.revision.store;
import forge.db.core.driver;
import forge.plugins.db.store.exceptions;
import forge.plugins.db.store.types;

#include "details/api_impl.hxx"
#include "details/plugin_impl.hxx"
#include "details/store_handle_state_impl.hxx"

namespace forge::plugins::db::store {

plugin::api_impl::api_impl(std::shared_ptr<impl> owner) : owner_{std::move(owner)} {}

boost::asio::awaitable<void>
plugin::api_impl::add_store(std::string name,
                            std::shared_ptr<forge::db::core::driver> driver,
                            store_options options) {
   owner_->add_store(std::move(name), std::move(driver), options);
   co_return;
}

boost::asio::awaitable<store_handle> plugin::api_impl::store(std::string name) {
   const auto generation = owner_->lifecycle.snapshot().generation;
   if (!owner_->lifecycle.try_admit(generation, [&] { (void)owner_->require_store(name); })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store handle admission is closed");
   }
   co_return store_handle{std::make_shared<store_handle_state_impl>(owner_, std::move(name))};
}

boost::asio::awaitable<void> plugin::api_impl::flush(std::string name, bool sync) {
   co_await owner_->require_started_store(name).driver->async_flush(sync);
}

boost::asio::awaitable<void> plugin::api_impl::flush_all(bool sync) {
   for (const auto& item : owner_->current_status().stores) {
      co_await owner_->require_started_store(item.name).driver->async_flush(sync);
   }
}

boost::asio::awaitable<::forge::plugins::db::store::status> plugin::api_impl::status() {
   co_return owner_->current_status();
}

} // namespace forge::plugins::db::store
