module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include "details/snapshot_access.hxx"

module forge.db.blob.store;

import forge.db.blob.exceptions;
import forge.log.logger;
import forge.db.core.exceptions;

#include "details/store_impl.hxx"

namespace forge::db::blob {

store::impl::impl(std::shared_ptr<forge::db::core::driver> driver_value, store::config config_value)
    : driver{std::move(driver_value)}, config{std::move(config_value)} {
   if (!driver) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "db blob driver is null");
   }
   if (config.data_family.name.empty() || config.refs_family.name.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "db blob family names must not be empty");
   }
}

store::store(std::shared_ptr<forge::db::core::driver> driver)
    : store(std::move(driver), config{}) {}

store::store(std::shared_ptr<forge::db::core::driver> driver, config settings)
    : impl_{std::make_shared<impl>(std::move(driver), std::move(settings))} {
   dlog("forge.db.blob", "blob store initialized", ("event", "store.initialized"));
}

boost::asio::awaitable<transaction> store::begin_transaction() {
   auto active = co_await impl_->driver->begin_transaction();
   auto result = transaction{
      std::move(active),
      impl_->config.data_family,
      impl_->config.refs_family};
   detail::transaction_access::bind_store(result, impl_);
   co_return result;
}

boost::asio::awaitable<snapshot> store::begin_read() {
   auto active = forge::db::core::snapshot{};
   try {
      active = co_await impl_->driver->begin_read();
   } catch (const forge::db::core::exceptions::unsupported_operation&) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation,
                            "db blob driver does not support snapshot reads");
   }
   co_return join(active);
}

snapshot store::join(const forge::db::core::snapshot& active) {
   if (!active.active()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed,
                            "db blob snapshot is closed");
   }
   if (!active.belongs_to(*impl_->driver)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor,
                            "db blob snapshot belongs to another driver");
   }
   return detail::snapshot_access::make<snapshot>(
      active,
      impl_->config.data_family,
      impl_->config.refs_family);
}

transaction store::join(forge::db::core::transaction& active) {
   auto result = transaction{
      active,
      impl_->config.data_family,
      impl_->config.refs_family};
   detail::transaction_access::bind_store(result, impl_);
   return result;
}

transaction store::join(transaction& active) {
   if (!detail::transaction_access::belongs_to(active, impl_.get())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor,
                            "db blob transaction belongs to another store");
   }
   return detail::transaction_access::joined(active);
}

boost::asio::awaitable<ref<digest>> store::put(std::vector<std::byte> payload) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   auto value = ref<digest>{};
   try {
      value = co_await active.put(std::move(payload));
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
   dlog("forge.db.blob", "blob committed", ("event", "blob.committed"));
   co_return value;
}

boost::asio::awaitable<collect_result> store::collect_unreferenced(collect_options options) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   auto result = collect_result{};
   try {
      result = co_await active.collect_unreferenced(options);
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
   dlog("forge.db.blob", "unreferenced blob collection committed",
        ("event", "collection.committed")("removed", result.removed)("limit", options.limit));
   co_return result;
}

} // namespace forge::db::blob
