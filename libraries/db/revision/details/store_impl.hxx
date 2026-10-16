#pragma once

#include <memory>

namespace forge::db::revision {

struct store::impl {
   impl(std::shared_ptr<forge::db::core::driver> driver_value,
        forge::db::object::store objects_value);

   boost::asio::awaitable<void> initialize(bool create_missing);
   boost::asio::awaitable<void> require_pristine(forge::db::object::transaction& active);
   void require_joinable(const forge::db::core::transaction& active) const;
   void require_control(const forge::db::core::transaction& active) const;
   boost::asio::awaitable<revision_id_t> join(forge::db::core::transaction& active);
   boost::asio::awaitable<void>
   revert(forge::db::core::transaction& active, revision_id_t expected_head);
   boost::asio::awaitable<prune_result>
   prune_through(forge::db::core::transaction& active,
                 revision_id_t inclusive_boundary,
                 prune_options options);

   std::shared_ptr<forge::db::core::driver> driver;
   forge::db::object::store objects;
   forge::db::core::family family;
};

} // namespace forge::db::revision
