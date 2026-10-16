#pragma once

#include <boost/asio/awaitable.hpp>

namespace forge::db::object {

namespace detail {
class transaction_participant_impl;
}

struct transaction::impl {
   impl(forge::db::core::transaction&& active_value, forge::db::core::family family_value,
        transaction::ensure_registered_fn ensure, transaction::allocate_id_fn allocate,
        transaction::seal_allocations_fn seal, std::vector<std::shared_ptr<interceptor>> interceptors_value,
        std::vector<std::shared_ptr<observer>> observers_value, transaction::release_fn release,
        bool reuse_rolled_back_ids = false);

   impl(forge::db::core::transaction& active_value, forge::db::core::family family_value,
        transaction::ensure_registered_fn ensure, transaction::allocate_id_fn allocate,
        transaction::seal_allocations_fn seal, std::vector<std::shared_ptr<interceptor>> interceptors_value,
        std::vector<std::shared_ptr<observer>> observers_value, transaction::release_fn release,
        bool reuse_rolled_back_ids = false);

   std::optional<forge::db::core::transaction> owned;
   forge::db::core::transaction* active = nullptr;
   forge::db::core::family family;
   transaction::ensure_registered_fn ensure_registered;
   transaction::allocate_id_fn allocate_id;
   detail::transaction_access::tracked_allocate_id_fn allocate_tracked;
   std::shared_ptr<detail::transaction_participant_impl> participant;
   std::vector<std::shared_ptr<interceptor>> interceptors;
   std::shared_ptr<const void> store_identity;
   bool backend_writes = false;
   void remember_allocation(forge::db::ids::object_id type, std::uint64_t next_instance);
};

} // namespace forge::db::object
