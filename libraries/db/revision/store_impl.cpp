module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

module forge.db.revision.store;

import forge.db.core.exceptions;
import forge.db.core.record;
import forge.db.ids.object_id;
import forge.db.object.system;
import forge.db.revision.exceptions;
import forge.raw.raw;

#include "details/codec.hxx"
#include "details/store_impl.hxx"
#include "details/transaction_impl.hxx"

namespace forge::db::revision {

namespace {

bool same_owner(const std::shared_ptr<forge::db::core::driver>& left,
                const std::shared_ptr<forge::db::core::driver>& right) noexcept {
   const auto less = std::owner_less<std::shared_ptr<forge::db::core::driver>>{};
   return !less(left, right) && !less(right, left);
}

void validate_state(const state& value) {
   if (value.id != state_id || value.format_version != state::current_format_version ||
       value.next_revision == 0 || value.next_delta == 0 ||
       (value.head && *value.head >= value.next_revision) ||
       value.oldest_retained == 0 || value.oldest_retained > value.next_revision ||
       value.prune_baseline >= value.oldest_retained) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision state is inconsistent");
   }
}

boost::asio::awaitable<state>
read_state(forge::db::core::transaction& active, const forge::db::core::family& family) {
   const auto encoded = co_await active.get(family, detail::state_key());
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision state record is missing");
   }
   auto value = detail::decode<state>(*encoded, "state");
   validate_state(value);
   co_return value;
}

boost::asio::awaitable<state>
lock_state(forge::db::core::transaction& active, const forge::db::core::family& family) {
   if (!active.capabilities().record_locks) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation,
                            "db revision requires backend record locks");
   }
   const auto encoded = co_await active.get_for_update(family, detail::state_key());
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision state record is missing");
   }
   auto value = detail::decode<state>(*encoded, "state");
   validate_state(value);
   co_return value;
}

boost::asio::awaitable<entry>
read_entry(forge::db::core::transaction& active,
           const forge::db::core::family& family,
           revision_id_t id) {
   const auto encoded = co_await active.get(family, detail::entry_key(id));
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision entry is missing",
                            forge::exceptions::ctx("revision", id));
   }
   auto value = detail::decode<entry>(*encoded, "entry");
   if (value.id.instance != id ||
       (value.parent && *value.parent >= value.id.instance)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision entry id is inconsistent");
   }
   co_return value;
}

boost::asio::awaitable<std::optional<entry>>
read_next_entry(forge::db::core::transaction& active,
                const forge::db::core::family& family,
                const entry& current) {
   const auto current_key = detail::entry_key(current.id.instance);
   const auto page = co_await active.scan_page(
      family,
      forge::db::core::record_range{
         .begin = current_key,
         .end = detail::entry_key(std::numeric_limits<revision_id_t>::max()),
      },
      forge::db::core::page_request{
         .after = forge::db::core::cursor{.boundary = current_key},
         .limit = 1U,
      });
   if (page.entries.empty()) {
      co_return std::nullopt;
   }

   auto value = detail::decode<entry>(page.entries.front().value, "entry");
   if (page.entries.front().key != detail::entry_key(value.id.instance) ||
       value.id.instance <= current.id.instance ||
       value.parent != std::optional<revision_id_t>{current.id.instance}) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state,
                            "db revision retained chain is inconsistent");
   }
   co_return value;
}

boost::asio::awaitable<std::vector<delta>>
read_deltas(forge::db::core::transaction& active,
            const forge::db::core::family& family,
            const entry& revision_entry) {
   auto result = std::vector<delta>{};
   if (revision_entry.delta_count == 0) {
      if (revision_entry.first_delta != 0) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "empty db revision has a delta range");
      }
      co_return result;
   }
   if (revision_entry.first_delta == 0 ||
       revision_entry.delta_count > std::numeric_limits<std::uint64_t>::max() - revision_entry.first_delta) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision delta range is invalid");
   }

   for (auto ordinal = std::uint64_t{0}; ordinal < revision_entry.delta_count; ++ordinal) {
      const auto id = revision_entry.first_delta + ordinal;
      const auto encoded = co_await active.get(family, detail::delta_key(id));
      if (!encoded) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision delta is missing",
                               forge::exceptions::ctx("delta", id));
      }
      auto value = detail::decode<delta>(*encoded, "delta");
      if (value.id.instance != id || value.revision != revision_entry.id.instance ||
          value.ordinal != ordinal || value.family.empty() || value.key.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision delta is inconsistent");
      }
      if (value.retention_guard &&
          (value.retention_guard->family.empty() || value.retention_guard->key.empty())) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision retention guard is inconsistent");
      }
      result.push_back(std::move(value));
   }
   co_return result;
}

void require_control_transaction(const forge::db::core::transaction& active) {
   if (active.has_participant("forge.db.revision")) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation,
                            "db revision control operation cannot run inside a revision scope");
   }
}

} // namespace

store::impl::impl(std::shared_ptr<forge::db::core::driver> driver_value,
                  forge::db::object::store objects_value)
    : driver{std::move(driver_value)},
      objects{std::move(objects_value)},
      family{forge::db::object::system::access::family(objects)} {
   if (!driver || !same_owner(driver, forge::db::object::system::access::driver(objects))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store,
                            "db revision and db object stores must share one driver");
   }
}

boost::asio::awaitable<void> store::impl::require_pristine(forge::db::object::transaction& active) {
   auto encoded = co_await forge::db::object::system::access::inspect_pristine_revision(objects, active);
   const auto current = detail::decode<state>(encoded, "state");
   validate_state(current);
   auto initial = state{};
   initial.id = state_id;
   if (current != initial || encoded != detail::encode(initial)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state, "db revision state is not pristine");
   }
}

boost::asio::awaitable<void> store::impl::initialize(bool create_missing) {
   forge::db::object::system::access::register_object<state_index>(objects);
   forge::db::object::system::access::register_object<entry_index>(objects);
   forge::db::object::system::access::register_object<delta_index>(objects);

   auto active = co_await driver->begin_transaction();
   if (!create_missing) {
      auto error = std::exception_ptr{};
      try {
         if (!active.capabilities().record_locks) {
            FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db revision requires backend record locks");
         }
         static_cast<void>(co_await read_state(active, family));
      } catch (...) {
         error = std::current_exception();
      }
      // No journal/Object participant or mutation was admitted. Rollback is
      // nevertheless mandatory and must succeed before publishing this store.
      // Core closes/retires its SAME session even when rollback reports failure.
      co_await active.rollback();
      if (error) {
         std::rethrow_exception(error);
      }
      co_return;
   }
   auto error = std::exception_ptr{};
   try {
      auto object_scope = co_await objects.join(active);
      static_cast<void>(object_scope);
      if (!active.capabilities().record_locks) {
         FORGE_THROW_EXCEPTION(exceptions::unsupported_operation,
                               "db revision requires backend record locks");
      }
      const auto existing = co_await active.get_for_update(family, detail::state_key());
      if (!existing) {
         auto initial = state{};
         initial.id = state_id;
         co_await active.put(family, detail::state_key(), detail::encode(initial));
      } else {
         validate_state(detail::decode<state>(*existing, "state"));
      }
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      try {
         co_await active.rollback();
      } catch (...) {
      }
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<revision_id_t>
store::impl::join(forge::db::core::transaction& active) {
   require_joinable(active);
   auto current = co_await read_state(active, family);
   auto participant = std::make_shared<detail::transaction_impl>(family, std::move(current));
   active.attach_participant(participant);
   try {
      participant->reset_state(co_await lock_state(active, family));
   } catch (...) {
      participant->invalidate();
      throw;
   }
   co_return participant->id();
}

void store::impl::require_joinable(const forge::db::core::transaction& active) const {
   if (active.has_participant("forge.db.revision")) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation,
                            "db transaction already has a revision scope");
   }
}

void store::impl::require_control(const forge::db::core::transaction& active) const {
   require_control_transaction(active);
}

boost::asio::awaitable<void>
store::impl::revert(forge::db::core::transaction& active, revision_id_t expected_head) {
   require_control_transaction(active);
   auto current = co_await lock_state(active, family);
   if (!current.head || *current.head != expected_head) {
      FORGE_THROW_EXCEPTION(exceptions::stale_head, "db revision head does not match expected value",
                            forge::exceptions::ctx("expected", expected_head));
   }
   if (expected_head <= current.prune_baseline) {
      FORGE_THROW_EXCEPTION(exceptions::revision_pruned,
                            "db revision head no longer has retained undo data",
                            forge::exceptions::ctx("revision", expected_head));
   }

   const auto revision_entry = co_await read_entry(active, family, expected_head);
   const auto deltas = co_await read_deltas(active, family, revision_entry);
   if (revision_entry.parent) {
      if (*revision_entry.parent < current.prune_baseline) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_state,
                               "db revision parent is older than the prune baseline");
      }
      if (*revision_entry.parent > current.prune_baseline) {
         const auto parent = co_await read_entry(active, family, *revision_entry.parent);
         const auto child = co_await read_next_entry(active, family, parent);
         if (!child || child->id.instance != expected_head) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_state,
                                  "db revision parent chain is inconsistent");
         }
      }
   } else if (current.prune_baseline != 0) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state,
                            "db revision parent is missing after the prune baseline");
   }

   for (auto iterator = deltas.rbegin(); iterator != deltas.rend(); ++iterator) {
      auto target_family = forge::db::core::family{iterator->family};
      auto target_key = forge::db::core::record_key{iterator->key};
      if (iterator->before) {
         co_await active.put(std::move(target_family), std::move(target_key), *iterator->before);
      } else {
         co_await active.erase(std::move(target_family), std::move(target_key));
      }
   }
   for (const auto& value : deltas) {
      if (value.retention_guard) {
         co_await active.erase(
            forge::db::core::family{value.retention_guard->family},
            forge::db::core::record_key{value.retention_guard->key});
      }
      co_await active.erase(family, detail::delta_key(value.id.instance));
   }
   co_await active.erase(family, detail::entry_key(expected_head));

   current.head = revision_entry.parent;
   if (current.oldest_retained == expected_head) {
      current.oldest_retained = current.next_revision;
   }
   co_await active.put(family, detail::state_key(), detail::encode(current));
}

boost::asio::awaitable<prune_result>
store::impl::prune_through(forge::db::core::transaction& active,
                          revision_id_t inclusive_boundary,
                          prune_options options) {
   require_control_transaction(active);
   if (options.max_revisions == 0 || options.max_deltas == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_prune, "db revision prune limits must be positive");
   }
   auto current = co_await lock_state(active, family);
   if (!current.head || inclusive_boundary >= *current.head) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_prune,
                            "db revision prune boundary must be older than the active head");
   }
   if (inclusive_boundary <= current.prune_baseline) {
      co_return prune_result{.complete = true};
   }
   const auto boundary_record = co_await active.get(family, detail::entry_key(inclusive_boundary));
   if (!boundary_record) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_prune,
                            "db revision prune boundary is not retained");
   }
   const auto boundary_entry = detail::decode<entry>(*boundary_record, "entry");
   if (boundary_entry.id.instance != inclusive_boundary ||
       (boundary_entry.parent && *boundary_entry.parent >= inclusive_boundary)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_state,
                            "db revision prune boundary is inconsistent");
   }

   struct planned_revision {
      entry metadata;
      std::vector<delta> deltas;
      revision_id_t next = 0;
   };
   auto planned = std::vector<planned_revision>{};
   auto cursor = current.oldest_retained;
   auto delta_count = std::uint64_t{0};
   while (cursor <= inclusive_boundary && planned.size() < options.max_revisions) {
      auto metadata = co_await read_entry(active, family, cursor);
      if (metadata.delta_count > options.max_deltas - delta_count) {
         if (planned.empty()) {
            FORGE_THROW_EXCEPTION(exceptions::prune_limit_too_small,
                                  "db revision delta limit cannot hold the first complete revision");
         }
         break;
      }
      auto deltas = co_await read_deltas(active, family, metadata);
      const auto next = co_await read_next_entry(active, family, metadata);
      const auto next_id = next ? next->id.instance : current.next_revision;
      if (metadata.id.instance != *current.head && !next) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_state,
                               "db revision retained chain ends before head");
      }
      delta_count += metadata.delta_count;
      planned.push_back(planned_revision{
         .metadata = std::move(metadata),
         .deltas = std::move(deltas),
         .next = next_id,
      });
      cursor = next_id;
   }

   auto result = prune_result{};
   for (const auto& revision : planned) {
      for (const auto& value : revision.deltas) {
         if (value.retention_guard) {
            co_await active.erase(
               forge::db::core::family{value.retention_guard->family},
               forge::db::core::record_key{value.retention_guard->key});
         }
         co_await active.erase(family, detail::delta_key(value.id.instance));
         ++result.deltas_pruned;
      }
      co_await active.erase(family, detail::entry_key(revision.metadata.id.instance));
      result.last_pruned = revision.metadata.id.instance;
      ++result.revisions_pruned;
   }

   if (result.last_pruned) {
      current.prune_baseline = *result.last_pruned;
      current.oldest_retained = planned.back().next;
      co_await active.put(family, detail::state_key(), detail::encode(current));
   }
   result.complete = current.oldest_retained > inclusive_boundary;
   co_return result;
}

} // namespace forge::db::revision
