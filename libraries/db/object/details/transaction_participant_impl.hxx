#pragma once

#include <boost/asio/awaitable.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::db::object::detail {

class transaction_participant_impl final : public forge::db::core::transaction_participant {
 public:
   transaction_participant_impl(forge::db::core::family family, transaction::seal_allocations_fn seal,
                                std::vector<std::shared_ptr<observer>> observers, transaction::release_fn release,
                                bool reuse_rolled_back_ids);
   ~transaction_participant_impl() override;

   [[nodiscard]] std::string_view name() const noexcept override;
   [[nodiscard]] std::span<const forge::db::core::family> exclusive_families() const noexcept override;
   [[nodiscard]] std::span<const forge::db::core::record_lock_claim> prewrite_locks() const noexcept override;
   [[nodiscard]] forge::db::core::mutation_policy classify(const forge::db::core::family& family,
                                                           const forge::db::core::record_key& key,
                                                           forge::db::core::mutation_kind kind) const noexcept override;

   boost::asio::awaitable<void> prepare_savepoint(forge::db::core::savepoint_id_t id) override;
   void publish_savepoint(forge::db::core::savepoint_id_t id) noexcept override;
   void discard_savepoint(forge::db::core::savepoint_id_t id) noexcept override;
   boost::asio::awaitable<void> rollback_to_savepoint(forge::db::core::savepoint_id_t id,
                                                      forge::db::core::participant_access& access) override;
   boost::asio::awaitable<void> release_savepoint(forge::db::core::savepoint_id_t id,
                                                  forge::db::core::participant_access& access) override;
   boost::asio::awaitable<void> prepare_commit(forge::db::core::participant_access& access) override;

   void remember_allocation(forge::db::ids::object_id type, std::uint64_t next_instance);
   void use_backend_writes(bool value) noexcept;
   [[nodiscard]] change_set& changes() noexcept;
   [[nodiscard]] const change_set& changes() const noexcept;
   [[nodiscard]] bool finalized() const noexcept;
   [[nodiscard]] std::size_t committed_mutation_count() const noexcept;
   [[nodiscard]] bool add_precommit_observer(std::shared_ptr<precommit_observer> value);
   boost::asio::awaitable<void> run_precommit_observers();

   boost::asio::awaitable<void> after_rollback();
   boost::asio::awaitable<void> after_commit();

 private:
   struct savepoint_frame {
      forge::db::core::savepoint_id_t id = 0;
      std::size_t mutation_count = 0;
   };

   void release_writer() noexcept;

   std::string name_;
   forge::db::core::family family_;
   std::array<forge::db::core::record_lock_claim, 1> prewrite_locks_;
   transaction::seal_allocations_fn seal_allocations_;
   transaction::allocation_seal_map allocation_seals_;
   std::vector<std::shared_ptr<observer>> observers_;
   std::vector<std::shared_ptr<precommit_observer>> precommit_observers_;
   std::optional<change_set> observed_change_set_;
   transaction::release_fn release_;
   change_set changes_;
   std::optional<savepoint_frame> pending_savepoint_;
   std::vector<savepoint_frame> savepoints_;
   bool finalized_ = false;
   bool preparing_commit_ = false;
   bool backend_writes_ = false;
   bool reuse_rolled_back_ids_ = false;
   std::size_t committed_mutation_count_ = 0;
};

} // namespace forge::db::object::detail
