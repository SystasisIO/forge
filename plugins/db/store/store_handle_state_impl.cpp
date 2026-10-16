module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <memory>
#include <mutex>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

module forge.plugins.db.store.plugin;

import forge.db.blob.store;
import forge.db.core.driver;
import forge.db.object.store;
import forge.db.revision.store;
import forge.asio.compute;
import forge.plugins.db.store.exceptions;

#include "details/plugin_impl.hxx"
#include "details/store_handle_state_impl.hxx"

namespace forge::plugins::db::store {

std::shared_ptr<plugin::impl> plugin::store_handle_state_impl::activation_owner() const {
   const auto lock = std::scoped_lock{activation_mutex_};
   auto owner = activation_owner_ ? activation_owner_ : owner_.lock();
   if (!owner)
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopped");
   return owner;
}

boost::asio::awaitable<void> plugin::store_handle_state_impl::close_for_activation() const {
   auto owner = activation_owner();
   {
      const auto lock = std::scoped_lock{activation_mutex_};
      activation_owner_ = owner;
   }
   co_await owner->close_for_activation(name_);
}
void plugin::store_handle_state_impl::claim_activation() const {
   activation_owner()->claim_activation(name_);
}
boost::asio::awaitable<void> plugin::store_handle_state_impl::activate_closed(std::filesystem::path destination) const {
   auto owner = activation_owner();
   co_await owner->activate_closed(name_, std::move(destination));
}
boost::asio::awaitable<void> plugin::store_handle_state_impl::confirm_activation() const {
   auto owner = activation_owner();
   co_await owner->activate_closed(name_, std::nullopt);
}
activation_status plugin::store_handle_state_impl::closed_state() const {
   return activation_owner()->closed_state(name_);
}
bool plugin::store_handle_state_impl::activation_inflight() const noexcept {
   const auto lock = std::scoped_lock{activation_mutex_};
   return activation_owner_ && activation_owner_->activation_inflight(name_);
}

plugin::store_handle_state_impl::store_handle_state_impl(std::weak_ptr<impl> owner, std::string name)
   : owner_{std::move(owner)}, name_{std::move(name)} {}

std::string plugin::store_handle_state_impl::name() const {
   return name_;
}

std::shared_ptr<forge::db::core::driver> plugin::store_handle_state_impl::require_driver() const {
   const auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopped");
   }
   return owner->require_started_store(name_).driver;
}

std::shared_ptr<forge::db::object::store> plugin::store_handle_state_impl::require_objects() const {
   const auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopped");
   }
   auto opened = owner->require_setup_store(name_);
   if (!opened.objects) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable_layer, "db store object layer is not configured",
                            forge::exceptions::ctx("store", name_));
   }
   return opened.objects;
}

std::shared_ptr<forge::db::blob::store> plugin::store_handle_state_impl::require_blobs() const {
   const auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopped");
   }
   auto opened = owner->require_started_store(name_);
   if (!opened.blobs) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable_layer, "db store blob layer is not configured",
                            forge::exceptions::ctx("store", name_));
   }
   return opened.blobs;
}

std::shared_ptr<forge::db::revision::store> plugin::store_handle_state_impl::require_revisions() const {
   const auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopped");
   }
   auto opened = owner->require_started_store(name_);
   if (!opened.revisions) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable_layer, "db store revision layer is not configured",
                            forge::exceptions::ctx("store", name_));
   }
   return opened.revisions;
}

boost::asio::awaitable<transaction> plugin::store_handle_state_impl::begin_transaction() const {
   const auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopped");
   }
   auto opened = owner->require_started_store(name_);
   if (opened.objects) {
      co_return transaction{co_await opened.objects->begin_transaction(), name_};
   }
   co_return transaction{co_await opened.driver->begin_transaction(), name_};
}

} // namespace forge::plugins::db::store
