module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.db.authenticated.transaction;

import forge.db.authenticated.exceptions;
import forge.db.authenticated.tree_engine;
import forge.db.core.participant;

#include "details/transaction_participant_impl.hxx"

namespace forge::db::authenticated::detail {

transaction_participant_impl::transaction_participant_impl(forge::db::core::family family, std::string domain,
                                                           digest namespace_hash)
    : name_{"forge.db.authenticated:" + std::move(domain)}, family_{std::move(family)},
      namespace_hash_{namespace_hash} {}

std::string_view transaction_participant_impl::name() const noexcept {
   return name_;
}

forge::db::core::mutation_policy transaction_participant_impl::classify(const forge::db::core::family& family,
                                                                        const forge::db::core::record_key& key,
                                                                        forge::db::core::mutation_kind) const noexcept {
   if (restore_phase_ != restore_phase::ordinary && family.name == family_.name) {
      return forge::db::core::mutation_policy::forbidden_when_captured;
   }
   if (family.name == family_.name && !key.empty() && key.bytes().front() == std::byte{9}) {
      return forge::db::core::mutation_policy::excluded;
   }
   return forge::db::core::mutation_policy::inherit;
}

std::optional<forge::db::core::record_address>
transaction_participant_impl::make_retention_guard(const forge::db::core::record_mutation& mutation,
                                                   std::span<const std::byte> token) const {
   if (mutation.column_family.name != family_.name ||
       mutation.key.bytes() != detail::latest_key(namespace_hash_).bytes() || !mutation.before || token.empty()) {
      return std::nullopt;
   }
   const auto previous = detail::decode_root(*mutation.before);
   return forge::db::core::record_address{
       .column_family = family_,
       .key = detail::retention_guard_key(namespace_hash_, previous.version, token),
   };
}

boost::asio::awaitable<void> transaction_participant_impl::prepare_savepoint(forge::db::core::savepoint_id_t id) {
   require_ordinary();
   pending_savepoint_ = savepoint_frame{.id = id, .staged = staged_};
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

boost::asio::awaitable<void> transaction_participant_impl::rollback_to_savepoint(forge::db::core::savepoint_id_t id,
                                                                                 forge::db::core::participant_access&) {
   require_ordinary();
   if (!savepoints_.empty() && savepoints_.back().id == id) {
      staged_ = std::move(savepoints_.back().staged);
      savepoints_.pop_back();
   }
   co_return;
}

boost::asio::awaitable<void> transaction_participant_impl::release_savepoint(forge::db::core::savepoint_id_t id,
                                                                             forge::db::core::participant_access&) {
   require_ordinary();
   if (!savepoints_.empty() && savepoints_.back().id == id) {
      savepoints_.pop_back();
   }
   co_return;
}

boost::asio::awaitable<void> transaction_participant_impl::prepare_commit(forge::db::core::participant_access&) {
   if (restore_phase_ != restore_phase::ordinary) {
      if (restore_phase_ != restore_phase::ready) {
         FORGE_THROW_EXCEPTION(exceptions::transaction_not_staged, "authenticated checkpoint is incomplete or failed");
      }
      co_return;
   }
   if (!staged_) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_not_staged, "authenticated transaction has no staged version");
   }
   co_return;
}

void transaction_participant_impl::set_staged(staged_version value) {
   require_ordinary();
   if (staged_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_version, "authenticated transaction is already staged");
   }
   staged_ = std::move(value);
}

std::optional<staged_version> transaction_participant_impl::staged() const {
   return staged_;
}

void transaction_participant_impl::enable_restore(root expected,
                                                  std::shared_ptr<forge::db::core::driver> owner) noexcept {
   restore_anchor_ = expected;
   restore_owner_ = std::move(owner);
   restore_phase_ = restore_phase::pending;
}

void transaction_participant_impl::require_ordinary() {
   if (restore_phase_ != restore_phase::ordinary) {
      fail_restore();
      FORGE_THROW_EXCEPTION(exceptions::invalid_version,
                            "ordinary staging and savepoints cannot modify a checkpoint restore");
   }
}

void transaction_participant_impl::start_restore() {
   if (restore_phase_ != restore_phase::pending) {
      fail_restore();
      FORGE_THROW_EXCEPTION(exceptions::invalid_version, "authenticated checkpoint restore is not pending");
   }
   restore_phase_ = restore_phase::loading;
}

void transaction_participant_impl::fail_restore() noexcept {
   if (restore_phase_ != restore_phase::ordinary) {
      restore_phase_ = restore_phase::failed;
   }
}

bool transaction_participant_impl::finish_restore() noexcept {
   if (restore_phase_ != restore_phase::loading) {
      return false;
   }
   restore_phase_ = restore_phase::ready;
   return true;
}

std::optional<root> transaction_participant_impl::restored() const noexcept {
   return restore_phase_ == restore_phase::ready ? std::optional<root>{restore_anchor_} : std::nullopt;
}

} // namespace forge::db::authenticated::detail
