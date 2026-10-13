module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>

#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

module forge.db.object.store;

import forge.asio.gate;
import forge.log.logger;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.object.exceptions;

#include "details/store_impl.hxx"

namespace forge::db::object {

store::store(std::shared_ptr<impl> implementation) : impl_{std::move(implementation)} {}

boost::asio::awaitable<store> store::open(std::shared_ptr<forge::db::core::driver> value) {
   co_return co_await open(std::move(value), config{}, options{});
}

boost::asio::awaitable<store> store::open(std::shared_ptr<forge::db::core::driver> value, options runtime) {
   co_return co_await open(std::move(value), config{}, runtime);
}

boost::asio::awaitable<store> store::open(std::shared_ptr<forge::db::core::driver> value, config settings) {
   co_return co_await open(std::move(value), std::move(settings), options{});
}

boost::asio::awaitable<store> store::open(std::shared_ptr<forge::db::core::driver> value, config settings,
                                          options runtime) {
   if (runtime.id_allocation == id_allocation_policy::transactional && runtime.writes != write_policy::single_writer) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor,
                            "transactional db object id allocation requires single-writer policy");
   }
   auto result = store{std::make_shared<impl>(std::move(value), std::move(settings), runtime)};
   auto ticket = co_await result.impl_->runtime->write_gate->acquire();
   auto active = forge::db::core::transaction{};

   try {
      active = co_await result.impl_->open_write_transaction();
   } catch (const forge::db::core::exceptions::unsupported_operation&) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db object driver does not support writes");
   }

   auto opened_header = forge::db::object::header{};
   auto error = std::exception_ptr{};
   try {
      opened_header = co_await result.impl_->initialize_header(active);
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      auto rolled_back = false;
      try {
         co_await active.rollback();
         rolled_back = true;
      } catch (...) {
      }
      ticket.release();
      if (rolled_back) {
         try {
            std::rethrow_exception(error);
         } catch (const exceptions::invalid_header&) {
            elog("forge.db.object", "object header rejected",
                 ("event", "header.rejected")("reason", "invalid_header"));
         } catch (const exceptions::incompatible_version&) {
            elog("forge.db.object", "object header rejected",
                 ("event", "header.rejected")("reason", "incompatible_version"));
         } catch (...) {
         }
      }
      std::rethrow_exception(error);
   }

   result.impl_->header_value = opened_header;
   ticket.release();
   dlog("forge.db.object", "object store initialized",
        ("event", "store.initialized")("single_writer", runtime.writes == write_policy::single_writer)
        ("transactional_ids", runtime.id_allocation == id_allocation_policy::transactional));
   co_return result;
}

forge::db::object::header store::header() const noexcept {
   return impl_->header_value;
}

std::shared_ptr<forge::db::core::driver> store::driver() const noexcept {
   return impl_->driver;
}

forge::db::core::family store::family() const {
   return impl_->config.family;
}

void store::add_interceptor(std::shared_ptr<interceptor> value) {
   if (value) {
      impl_->interceptors.push_back(std::move(value));
   }
}

void store::add_observer(std::shared_ptr<observer> value) {
   if (value) {
      impl_->observers.push_back(std::move(value));
   }
}

boost::asio::awaitable<transaction> store::begin_transaction() {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto ticket = std::optional<forge::asio::gate::ticket>{};
   if (impl_->settings.writes == write_policy::single_writer) {
      ticket.emplace(co_await impl_->runtime->write_gate->acquire());
   }

   auto active = forge::db::core::transaction{};
   try {
      active = co_await impl_->open_write_transaction();
   } catch (const forge::db::core::exceptions::unsupported_operation&) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db object driver does not support writes");
   }
   auto release = transaction::release_fn{};
   if (ticket.has_value()) {
      auto owned_ticket = std::make_shared<std::optional<forge::asio::gate::ticket>>(std::move(ticket));
      release = [owned_ticket]() mutable { owned_ticket->reset(); };
   }

   auto result = detail::transaction_access::make_owned(
       std::move(active), impl_->config.family,
       [impl = impl_](forge::db::ids::object_id type, std::type_index model) {
          impl->ensure_registered_type(type, model);
       },
       [impl = impl_](forge::db::ids::object_id type, forge::db::core::transaction& active)
           -> boost::asio::awaitable<forge::db::ids::object_id> { co_return co_await impl->allocate_id(type, active); },
       [impl = impl_](transaction::allocation_seal_map seals) -> boost::asio::awaitable<void> {
          co_await impl->seal_allocations(std::move(seals));
          co_return;
       },
       impl_->interceptors, impl_->observers, std::move(release), executor,
       impl_->settings.writes == write_policy::backend,
       impl_->settings.id_allocation == id_allocation_policy::transactional);
   detail::transaction_access::bind_store(result, impl_);
   co_return result;
}

boost::asio::awaitable<snapshot> store::begin_read() {
   auto active = forge::db::core::snapshot{};
   try {
      active = co_await impl_->open_read_snapshot();
   } catch (const forge::db::core::exceptions::unsupported_operation&) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db object driver does not support snapshot reads");
   }
   co_return join(active);
}

snapshot store::join(const forge::db::core::snapshot& active) {
   if (!active.active()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object snapshot is closed");
   }
   if (!active.belongs_to(*impl_->driver)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object snapshot belongs to another driver");
   }
   return snapshot{active, impl_->config.family, [impl = impl_](forge::db::ids::object_id type, std::type_index model) {
                      impl->ensure_registered_type(type, model);
                   }};
}

boost::asio::awaitable<transaction> store::join(forge::db::core::transaction& active) {
   if (active.claims_family(impl_->config.family)) {
      FORGE_THROW_EXCEPTION(forge::db::core::exceptions::participant_conflict,
                            "db object family is already attached to the transaction");
   }

   auto ticket = std::optional<forge::asio::gate::ticket>{};
   if (impl_->settings.writes == write_policy::single_writer) {
      ticket.emplace(co_await impl_->runtime->write_gate->acquire());
   }

   auto release = transaction::release_fn{};
   if (ticket.has_value()) {
      auto owned_ticket = std::make_shared<std::optional<forge::asio::gate::ticket>>(std::move(ticket));
      release = [owned_ticket]() mutable { owned_ticket->reset(); };
   }

   auto result = detail::transaction_access::make_joined(
       active, impl_->config.family,
       [impl = impl_](forge::db::ids::object_id type, std::type_index model) {
          impl->ensure_registered_type(type, model);
       },
       [impl = impl_](forge::db::ids::object_id type, forge::db::core::transaction& active)
           -> boost::asio::awaitable<forge::db::ids::object_id> { co_return co_await impl->allocate_id(type, active); },
       [impl = impl_](transaction::allocation_seal_map seals) -> boost::asio::awaitable<void> {
          co_await impl->seal_allocations(std::move(seals));
          co_return;
       },
       impl_->interceptors, impl_->observers, std::move(release), impl_->settings.writes == write_policy::backend,
       impl_->settings.id_allocation == id_allocation_policy::transactional);
   detail::transaction_access::bind_store(result, impl_);
   co_return result;
}

boost::asio::awaitable<transaction> store::join(transaction& active) {
   if (!detail::transaction_access::belongs_to(active, impl_.get())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object transaction belongs to another store");
   }
   co_return detail::transaction_access::joined(active);
}

void store::register_object_type(forge::db::ids::object_id type, std::type_index model) {
   impl_->register_object_type(type, model);
}

void store::register_system_object_type(forge::db::ids::object_id type, std::type_index model) {
   const auto found = impl_->registered.find(type);
   if (found == impl_->registered.end()) {
      impl_->registered.emplace(type, model);
      return;
   }
   if (found->second != model) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object system type id is already registered");
   }
}

void store::ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const {
   impl_->ensure_registered_type(type, model);
}

} // namespace forge::db::object
