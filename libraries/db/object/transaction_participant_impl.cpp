module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.db.object.transaction;

#include "details/transaction_impl.hxx"
#include "details/transaction_participant_impl.hxx"
#include "details/record_key.hxx"

namespace forge::db::object::detail {

namespace {

std::string participant_name(const forge::db::core::family& family) {
   auto result = std::string{"forge.db.object:"};
   result.append(std::to_string(family.name.size()));
   result.push_back(':');
   result.append(family.name);
   return result;
}

bool same_changes(const change_set& left, const change_set& right) {
   return std::equal(left.mutations.begin(), left.mutations.end(), right.mutations.begin(), right.mutations.end(),
                     [](const object_mutation& lhs, const object_mutation& rhs) {
                        return lhs.kind == rhs.kind && lhs.id == rhs.id && lhs.before == rhs.before &&
                               lhs.after == rhs.after;
                     });
}

} // namespace

transaction_participant_impl::transaction_participant_impl(forge::db::core::family family,
                                                           transaction::seal_allocations_fn seal,
                                                           std::vector<std::shared_ptr<observer>> observers,
                                                           transaction::release_fn release, bool reuse_rolled_back_ids)
    : name_{participant_name(family)}, family_{std::move(family)},
      prewrite_locks_{
          forge::db::core::record_lock_claim{.column_family = family_, .key = record_key::ranked_coordinator()}},
      seal_allocations_{std::move(seal)}, observers_{std::move(observers)}, release_{std::move(release)},
      reuse_rolled_back_ids_{reuse_rolled_back_ids} {}

transaction_participant_impl::~transaction_participant_impl() {
   release_writer();
}

std::string_view transaction_participant_impl::name() const noexcept {
   return name_;
}

std::span<const forge::db::core::family> transaction_participant_impl::exclusive_families() const noexcept {
   return {std::addressof(family_), 1};
}

std::span<const forge::db::core::record_lock_claim> transaction_participant_impl::prewrite_locks() const noexcept {
   if (!backend_writes_) {
      return {};
   }
   return prewrite_locks_;
}

forge::db::core::mutation_policy transaction_participant_impl::classify(const forge::db::core::family& family,
                                                                        const forge::db::core::record_key& key,
                                                                        forge::db::core::mutation_kind) const noexcept {
   if (family.name != family_.name || key.empty()) {
      return forge::db::core::mutation_policy::inherit;
   }

   const auto kind = std::to_integer<std::uint8_t>(key.bytes().front());
   if (kind == static_cast<std::uint8_t>(record_key::entry_kind::system_record)) {
      return forge::db::core::mutation_policy::excluded;
   }
   if (kind == static_cast<std::uint8_t>(record_key::entry_kind::sequence_record)) {
      return reuse_rolled_back_ids_ ? forge::db::core::mutation_policy::reversible
                                    : forge::db::core::mutation_policy::excluded;
   }
   return forge::db::core::mutation_policy::reversible;
}

boost::asio::awaitable<void> transaction_participant_impl::prepare_savepoint(forge::db::core::savepoint_id_t id) {
   pending_savepoint_ = savepoint_frame{.id = id, .mutation_count = changes_.mutations.size()};
   co_return;
}

void transaction_participant_impl::publish_savepoint(forge::db::core::savepoint_id_t id) noexcept {
   if (pending_savepoint_ && pending_savepoint_->id == id) {
      savepoints_.push_back(*pending_savepoint_);
   }
   pending_savepoint_.reset();
}

void transaction_participant_impl::discard_savepoint(forge::db::core::savepoint_id_t id) noexcept {
   if (pending_savepoint_ && pending_savepoint_->id == id) {
      pending_savepoint_.reset();
   }
}

boost::asio::awaitable<void>
transaction_participant_impl::rollback_to_savepoint(forge::db::core::savepoint_id_t id,
                                                    forge::db::core::participant_access& access) {
   if (savepoints_.empty() || savepoints_.back().id != id) {
      co_return;
   }

   changes_.mutations.resize(savepoints_.back().mutation_count);
   savepoints_.pop_back();

   if (!reuse_rolled_back_ids_) {
      // Native rollback also restores sequence records. Re-publish every consumed
      // high-watermark so generated IDs are never reusable inside the outer transaction.
      for (const auto& [type, next_instance] : allocation_seals_) {
         auto bytes = std::vector<std::byte>{};
         bytes.reserve(sizeof(next_instance));
         record_key::append_be64(bytes, next_instance);
         co_await access.put(family_, record_key::sequence(type), std::move(bytes));
      }
   }
}

boost::asio::awaitable<void> transaction_participant_impl::release_savepoint(forge::db::core::savepoint_id_t id,
                                                                             forge::db::core::participant_access&) {
   if (!savepoints_.empty() && savepoints_.back().id == id) {
      savepoints_.pop_back();
   }
   co_return;
}

boost::asio::awaitable<void> transaction_participant_impl::prepare_commit(forge::db::core::participant_access&) {
   if (observed_change_set_ && !same_changes(*observed_change_set_, changes_)) {
      FORGE_THROW_EXCEPTION(exceptions::stale_precommit_projection,
                            "db object precommit projection changed after an observer accepted it");
   }
   co_return;
}

void transaction_participant_impl::remember_allocation(forge::db::ids::object_id type, std::uint64_t next_instance) {
   if (reuse_rolled_back_ids_) {
      return;
   }
   type.instance = 0;
   auto& existing = allocation_seals_[type];
   existing = std::max(existing, next_instance);
}

void transaction_participant_impl::use_backend_writes(bool value) noexcept {
   backend_writes_ = value;
}

change_set& transaction_participant_impl::changes() noexcept {
   return changes_;
}

const change_set& transaction_participant_impl::changes() const noexcept {
   return changes_;
}

bool transaction_participant_impl::finalized() const noexcept {
   return finalized_;
}

std::size_t transaction_participant_impl::committed_mutation_count() const noexcept {
   return committed_mutation_count_;
}

bool transaction_participant_impl::add_precommit_observer(std::shared_ptr<precommit_observer> value) {
   if (finalized_ || preparing_commit_) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object transaction is closed");
   }
   if (!value) {
      return false;
   }
   const auto install_hook = precommit_observers_.empty();
   precommit_observers_.push_back(std::move(value));
   return install_hook;
}

boost::asio::awaitable<void> transaction_participant_impl::run_precommit_observers() {
   preparing_commit_ = true;
   const auto observers = precommit_observers_;
   for (const auto& hook : observers) {
      co_await hook->before_commit(changes_);
   }
   observed_change_set_ = changes_;
}

void transaction_participant_impl::release_writer() noexcept {
   if (release_) {
      release_();
      release_ = {};
   }
}

boost::asio::awaitable<void> transaction_participant_impl::after_rollback() {
   finalized_ = true;
   changes_.mutations.clear();
   observed_change_set_.reset();
   savepoints_.clear();
   pending_savepoint_.reset();

   auto seals = std::move(allocation_seals_);
   allocation_seals_.clear();
   try {
      if (seal_allocations_ && !seals.empty()) {
         co_await seal_allocations_(std::move(seals));
      }
   } catch (...) {
      release_writer();
      throw;
   }
   release_writer();
}

boost::asio::awaitable<void> transaction_participant_impl::after_commit() {
   finalized_ = true;
   auto committed_changes = std::move(changes_);
   committed_mutation_count_ = committed_changes.mutations.size();
   observed_change_set_.reset();
   allocation_seals_.clear();
   savepoints_.clear();
   pending_savepoint_.reset();
   release_writer();

   if (!committed_changes.empty()) {
      for (const auto& hook : observers_) {
         co_await hook->after_commit(committed_changes);
      }
   }
}

} // namespace forge::db::object::detail
