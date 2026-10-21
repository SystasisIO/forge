module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

module forge.swarm.storage.catalog;

import forge.db.core.driver;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.snapshot;
import forge.db.object.transaction;
import forge.asio.notification;

#include "details/catalog_impl.hxx"
#include "details/catalog_store_access.hxx"

namespace forge::swarm::storage {

catalog::catalog(std::shared_ptr<impl> value) : _impl(std::move(value)) {}
catalog::~catalog() = default;

void catalog::register_objects(forge::db::object::store& objects) {
   register_objects<forge::db::object::store>(objects);
}

boost::asio::awaitable<std::shared_ptr<catalog>> catalog::async_open(
   forge::db::object::store objects, options config, std::stop_token token) {
   return async_open(std::make_shared<detail::catalog_store_access>(std::move(objects)), std::move(config), token);
}

boost::asio::awaitable<std::shared_ptr<catalog>> catalog::async_open(
   std::shared_ptr<access> objects, options config, std::stop_token token) {
   auto self = std::make_shared<impl>(std::move(objects), std::move(config));
   return impl::dispatch<std::shared_ptr<catalog>>(impl::admission{self}, token,
      [](std::shared_ptr<impl> owner, std::stop_token stop) { return impl::open(std::move(owner), stop); });
}

boost::asio::awaitable<void> catalog::async_close() {
   auto self = _impl;
   // One atomic word linearizes close against the existing eager admission CAS.
   self->inflight.fetch_or(impl::closed_bit, std::memory_order_acq_rel);
   return boost::asio::co_spawn(self->config.database, impl::close(self),
      boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
}

boost::asio::awaitable<db::content> catalog::async_create(
   core::descriptor manifest, std::string locator, job_settings settings, std::stop_token token) {
   return impl::dispatch<db::content>(impl::admission{_impl}, token,
      [manifest = std::move(manifest), locator = std::move(locator), settings](
         std::shared_ptr<impl> self, std::stop_token stop) mutable {
         return impl::create(std::move(self), std::move(manifest), std::move(locator), settings, stop);
      });
}

boost::asio::awaitable<std::optional<db::content>> catalog::async_find(core::swarm_id id, std::stop_token token) {
   return impl::dispatch<std::optional<db::content>>(impl::admission{_impl}, token,
      [id](std::shared_ptr<impl> self, std::stop_token stop) { return impl::find(std::move(self), id, stop); });
}

boost::asio::awaitable<void> catalog::async_update(core::swarm_id id, job_settings settings, std::stop_token token) {
   return impl::dispatch<void>(impl::admission{_impl}, token,
      [id, settings](std::shared_ptr<impl> self, std::stop_token stop) {
         return impl::update(std::move(self), id, settings, stop);
      });
}

boost::asio::awaitable<void> catalog::async_erase(core::swarm_id id, std::stop_token token) {
   return impl::dispatch<void>(impl::admission{_impl}, token,
      [id](std::shared_ptr<impl> self, std::stop_token stop) { return impl::erase(std::move(self), id, stop); });
}

boost::asio::awaitable<forge::db::object::object_page<db::content>> catalog::async_by_state(
   db::job_state state, forge::db::core::page_request page, std::stop_token token) {
   return impl::dispatch<forge::db::object::object_page<db::content>>(impl::admission{_impl}, token,
      [state, page = std::move(page)](std::shared_ptr<impl> self, std::stop_token stop) mutable {
         return impl::by_state(std::move(self), state, std::move(page), stop);
      });
}

boost::asio::awaitable<forge::db::object::object_page<db::content>> catalog::async_expiring_before(
   std::int64_t exclusive, forge::db::core::page_request page, std::stop_token token) {
   return impl::dispatch<forge::db::object::object_page<db::content>>(impl::admission{_impl}, token,
      [exclusive, page = std::move(page)](std::shared_ptr<impl> self, std::stop_token stop) mutable {
         return impl::expiring_before(std::move(self), exclusive, std::move(page), stop);
      });
}

boost::asio::awaitable<std::shared_ptr<core::checkpoint_store>> catalog::async_checkpoint_store(
   core::swarm_id id, std::string locator, std::stop_token token) {
   return impl::dispatch<std::shared_ptr<core::checkpoint_store>>(impl::admission{_impl}, token,
      [id, locator = std::move(locator)](std::shared_ptr<impl> self, std::stop_token stop) mutable {
         return impl::bind_checkpoint(std::move(self), id, std::move(locator), stop);
      });
}

} // namespace forge::swarm::storage
