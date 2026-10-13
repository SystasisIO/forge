module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/system_executor.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

module forge.db.object.transaction;

import forge.log.logger;

import forge.db.core.exceptions;
import forge.db.object.exceptions;

#include "details/transaction_impl.hxx"
#include "details/transaction_participant_impl.hxx"

namespace forge::db::object {

transaction::impl::impl(forge::db::core::transaction&& active_value, forge::db::core::family family_value,
                        transaction::ensure_registered_fn ensure, transaction::allocate_id_fn allocate,
                        transaction::seal_allocations_fn seal,
                        std::vector<std::shared_ptr<interceptor>> interceptors_value,
                        std::vector<std::shared_ptr<observer>> observers_value, transaction::release_fn release,
                        bool reuse_rolled_back_ids)
    : owned{std::move(active_value)}, active{&*owned}, family{std::move(family_value)},
      ensure_registered{std::move(ensure)}, allocate_id{std::move(allocate)},
      participant{std::make_shared<detail::transaction_participant_impl>(
          family, std::move(seal), std::move(observers_value), std::move(release), reuse_rolled_back_ids)},
      interceptors{std::move(interceptors_value)} {}

transaction::impl::impl(forge::db::core::transaction& active_value, forge::db::core::family family_value,
                        transaction::ensure_registered_fn ensure, transaction::allocate_id_fn allocate,
                        transaction::seal_allocations_fn seal,
                        std::vector<std::shared_ptr<interceptor>> interceptors_value,
                        std::vector<std::shared_ptr<observer>> observers_value, transaction::release_fn release,
                        bool reuse_rolled_back_ids)
    : active{&active_value}, family{std::move(family_value)}, ensure_registered{std::move(ensure)},
      allocate_id{std::move(allocate)},
      participant{std::make_shared<detail::transaction_participant_impl>(
          family, std::move(seal), std::move(observers_value), std::move(release), reuse_rolled_back_ids)},
      interceptors{std::move(interceptors_value)} {}

void transaction::impl::remember_allocation(forge::db::ids::object_id type, std::uint64_t next_instance) {
   participant->remember_allocation(type, next_instance);
}

transaction::transaction(forge::db::core::transaction&& active, forge::db::core::family family,
                         ensure_registered_fn ensure, allocate_id_fn allocate,
                         std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release)
    : transaction(std::move(active), std::move(family), std::move(ensure), std::move(allocate), seal_allocations_fn{},
                  std::move(interceptors), std::move(observers), std::move(release), boost::asio::system_executor{}) {}

transaction::transaction(forge::db::core::transaction&& active, forge::db::core::family family,
                         ensure_registered_fn ensure, std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release)
    : transaction(std::move(active), std::move(family), std::move(ensure), allocate_id_fn{}, seal_allocations_fn{},
                  std::move(interceptors), std::move(observers), std::move(release), boost::asio::system_executor{}) {}

transaction::transaction(forge::db::core::transaction&& active, forge::db::core::family family,
                         ensure_registered_fn ensure, allocate_id_fn allocate, seal_allocations_fn seal,
                         std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release,
                         boost::asio::any_io_executor cleanup_executor)
    : transaction(std::move(active), std::move(family), std::move(ensure), std::move(allocate), std::move(seal),
                  std::move(interceptors), std::move(observers), std::move(release), std::move(cleanup_executor), false,
                  false) {}

transaction::transaction(forge::db::core::transaction&& active, forge::db::core::family family,
                         ensure_registered_fn ensure, allocate_id_fn allocate, seal_allocations_fn seal,
                         std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release,
                         boost::asio::any_io_executor, bool backend_writes, bool reuse_rolled_back_ids)
    : impl_{std::make_shared<impl>(std::move(active), std::move(family), std::move(ensure), std::move(allocate),
                                   std::move(seal), std::move(interceptors), std::move(observers), std::move(release),
                                   reuse_rolled_back_ids)} {
   owns_commit_ = true;
   impl_->backend_writes = backend_writes;
   impl_->participant->use_backend_writes(backend_writes);
   db_transaction().attach_participant(impl_->participant);
   auto participant = impl_->participant;
   db_transaction().after_commit([participant]() mutable -> boost::asio::awaitable<void> {
      co_await participant->after_commit();
      co_return;
   });
   db_transaction().after_rollback([participant]() mutable -> boost::asio::awaitable<void> {
      co_await participant->after_rollback();
      co_return;
   });
}

transaction::transaction(forge::db::core::transaction&& active, forge::db::core::family family,
                         ensure_registered_fn ensure, std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release,
                         boost::asio::any_io_executor cleanup_executor)
    : transaction(std::move(active), std::move(family), std::move(ensure), allocate_id_fn{}, seal_allocations_fn{},
                  std::move(interceptors), std::move(observers), std::move(release), std::move(cleanup_executor)) {}

transaction::transaction(forge::db::core::transaction& active, forge::db::core::family family,
                         ensure_registered_fn ensure, allocate_id_fn allocate,
                         std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers)
    : transaction(active, std::move(family), std::move(ensure), std::move(allocate), seal_allocations_fn{},
                  std::move(interceptors), std::move(observers)) {}

transaction::transaction(std::shared_ptr<impl> implementation) : impl_{std::move(implementation)} {}

transaction detail::transaction_access::make_owned(
    forge::db::core::transaction&& active, forge::db::core::family family, transaction::ensure_registered_fn ensure,
    transaction::allocate_id_fn allocate, transaction::seal_allocations_fn seal,
    std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
    transaction::release_fn release, boost::asio::any_io_executor cleanup_executor, bool backend_writes,
    bool reuse_rolled_back_ids) {
   return transaction{std::move(active),    std::move(family),    std::move(ensure),
                      std::move(allocate),  std::move(seal),      std::move(interceptors),
                      std::move(observers), std::move(release),   std::move(cleanup_executor),
                      backend_writes,       reuse_rolled_back_ids};
}

transaction detail::transaction_access::make_joined(
    forge::db::core::transaction& active, forge::db::core::family family, transaction::ensure_registered_fn ensure,
    transaction::allocate_id_fn allocate, transaction::seal_allocations_fn seal,
    std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
    transaction::release_fn release, bool backend_writes, bool reuse_rolled_back_ids) {
   return transaction{active,          std::move(family),       std::move(ensure),    std::move(allocate),
                      std::move(seal), std::move(interceptors), std::move(observers), std::move(release),
                      backend_writes,  reuse_rolled_back_ids};
}

void detail::transaction_access::bind_store(transaction& active, std::shared_ptr<const void> identity) {
   if (active.impl_) {
      active.impl_->store_identity = std::move(identity);
   }
}

bool detail::transaction_access::belongs_to(const transaction& active, const void* identity) noexcept {
   return active.impl_ && active.impl_->store_identity.get() == identity;
}

transaction detail::transaction_access::joined(transaction& active) {
   (void)active.active_transaction();
   return transaction{active.impl_};
}

transaction::transaction(forge::db::core::transaction& active, forge::db::core::family family,
                         ensure_registered_fn ensure, allocate_id_fn allocate, seal_allocations_fn seal,
                         std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release)
    : transaction(active, std::move(family), std::move(ensure), std::move(allocate), std::move(seal),
                  std::move(interceptors), std::move(observers), std::move(release), false, false) {}

transaction::transaction(forge::db::core::transaction& active, forge::db::core::family family,
                         ensure_registered_fn ensure, allocate_id_fn allocate, seal_allocations_fn seal,
                         std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers, release_fn release, bool backend_writes,
                         bool reuse_rolled_back_ids)
    : impl_{std::make_shared<impl>(active, std::move(family), std::move(ensure), std::move(allocate), std::move(seal),
                                   std::move(interceptors), std::move(observers), std::move(release),
                                   reuse_rolled_back_ids)} {
   impl_->backend_writes = backend_writes;
   impl_->participant->use_backend_writes(backend_writes);
   db_transaction().attach_participant(impl_->participant);
   auto participant = impl_->participant;
   db_transaction().after_commit([participant]() mutable -> boost::asio::awaitable<void> {
      co_await participant->after_commit();
      co_return;
   });
   db_transaction().after_rollback([participant]() mutable -> boost::asio::awaitable<void> {
      co_await participant->after_rollback();
      co_return;
   });
}

transaction::transaction(forge::db::core::transaction& active, forge::db::core::family family,
                         ensure_registered_fn ensure, std::vector<std::shared_ptr<interceptor>> interceptors,
                         std::vector<std::shared_ptr<observer>> observers)
    : transaction(active, std::move(family), std::move(ensure), allocate_id_fn{}, seal_allocations_fn{},
                  std::move(interceptors), std::move(observers)) {}

forge::db::core::transaction& transaction::db_transaction() const {
   return active_transaction();
}

change_set transaction::projected_changes() const {
   static_cast<void>(active_transaction());
   return impl_->participant->changes();
}

void transaction::add_precommit_observer(std::shared_ptr<precommit_observer> value) {
   if (!impl_ || impl_->participant->finalized()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   if (impl_->participant->add_precommit_observer(std::move(value))) {
      auto participant = impl_->participant;
      active_transaction().before_commit([participant = std::move(participant)]() -> boost::asio::awaitable<void> {
         co_await participant->run_precommit_observers();
      });
   }
}

forge::db::core::transaction& transaction::active_transaction() const {
   if (!impl_ || !impl_->active || !impl_->active->active() || impl_->participant->finalized()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   return *impl_->active;
}

change_set& transaction::changes() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   return impl_->participant->changes();
}

void transaction::ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const {
   if (!impl_ || !impl_->ensure_registered) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   impl_->ensure_registered(type, model);
}

boost::asio::awaitable<void> transaction::before_mutation(const object_mutation& mutation) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   for (const auto& hook : impl_->interceptors) {
      co_await hook->before_mutation(mutation);
   }
   co_return;
}

boost::asio::awaitable<forge::db::ids::object_id> transaction::allocate_id(forge::db::ids::object_id type) const {
   if (!impl_ || !impl_->allocate_id) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db object transaction cannot allocate ids");
   }
   auto allocated = co_await impl_->allocate_id(type, active_transaction());
   impl_->remember_allocation(type, allocated.instance + 1U);
   co_return allocated;
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
transaction::get_record(forge::db::core::record_key key) const {
   co_return co_await active_transaction().get(impl_->family, std::move(key));
}

boost::asio::awaitable<void> transaction::put_record(forge::db::core::record_key key,
                                                     std::vector<std::byte> value) const {
   co_await active_transaction().put(impl_->family, std::move(key), std::move(value));
}

boost::asio::awaitable<void> transaction::erase_record(forge::db::core::record_key key) const {
   co_await active_transaction().erase(impl_->family, std::move(key));
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
transaction::lock_record(forge::db::core::record_key key) const {
   if (!impl_ || !impl_->active) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   if (!impl_->backend_writes) {
      co_return co_await impl_->active->get(impl_->family, std::move(key));
   }
   if (!impl_->active->capabilities().record_locks) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "ranked db object indexes require backend record locks");
   }
   co_return co_await impl_->active->get_for_update(impl_->family, std::move(key));
}

boost::asio::awaitable<forge::db::core::record_page>
transaction::scan_records(forge::db::core::record_range range, forge::db::core::page_request request) const {
   co_return co_await active_transaction().scan_page(impl_->family, std::move(range), std::move(request));
}

boost::asio::awaitable<void> transaction::commit() {
   if (!impl_ || impl_->participant->finalized()) {
      co_return;
   }
   if (!owns_commit_) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "joined db object transaction does not own commit");
   }
   co_await active_transaction().commit();
   dlog("forge.db.object", "object changes committed",
        ("event", "changes.committed")("mutations", impl_->participant->committed_mutation_count()));
   co_return;
}

boost::asio::awaitable<void> transaction::rollback() {
   if (!impl_ || impl_->participant->finalized()) {
      co_return;
   }
   if (!owns_commit_) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "joined db object transaction does not own rollback");
   }
   co_await active_transaction().rollback();
   co_return;
}

} // namespace forge::db::object
