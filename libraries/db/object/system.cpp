module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.db.object.system;

import forge.db.object.object;
import forge.db.object.header;
import forge.db.object.exceptions;
import forge.raw.raw;

#include "details/record_key.hxx"

namespace forge::db::object::system {

std::shared_ptr<forge::db::core::driver> access::driver(const store& value) {
   return value.driver();
}

forge::db::core::family access::family(const store& value) {
   return value.family();
}

bool access::joined(const store& value,
                    const forge::db::core::transaction& active) {
   const auto object_family = value.family();
   auto participant = std::string{"forge.db.object:"};
   participant.append(std::to_string(object_family.name.size()));
   participant.push_back(':');
   participant.append(object_family.name);
   return active.has_participant(participant);
}

boost::asio::awaitable<void> access::require_pristine(store& value, transaction& active) {
   static_cast<void>(co_await inspect_pristine(value, active, false));
}

boost::asio::awaitable<std::vector<std::byte>> access::inspect_pristine_revision(store& value, transaction& active) {
   auto encoded = co_await inspect_pristine(value, active, true);
   co_return std::move(*encoded);
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
access::inspect_pristine(store& value, transaction& active, bool with_revision) {
   // Exact impl identity and active alias; NOT joined(store, CoreTx), which
   // intentionally tests only the participant name for other legacy callers.
   auto joined = co_await value.join(active);
   auto& core = joined.db_transaction();
   const auto owner = value.driver();
   if (!core.active()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db object pristine transaction is closed");
   }
   if (!core.belongs_to(*owner)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object pristine transaction belongs to another driver");
   }
   if (!core.capabilities().writes || value.runtime_options().writes != write_policy::single_writer) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db object pristine check requires a single writer");
   }
   const auto header_key = detail::record_key::object(forge::db::ids::to_object_id(forge::db::object::header_id));
   const auto revision_key = detail::record_key::object(
       forge::db::ids::object_id{.space = system_space, .type = type_id::revision_state, .instance = 0U});
   const auto expected = with_revision ? 2U : 1U;
   auto records = co_await core.scan_page(value.family(), forge::db::core::record_range{.has_end = false},
                                          forge::db::core::page_request{.limit = expected + 1U});
   if (records.entries.size() != expected || records.next) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object family is not pristine");
   }
   const auto canonical_header = forge::raw::pack(value.header());
   auto header_found = false;
   auto payload = std::optional<std::vector<std::byte>>{};
   for (auto& record : records.entries) {
      if (record.key == header_key && !header_found) {
         if (record.value.size() != canonical_header.size() ||
             !std::equal(
                 record.value.begin(), record.value.end(), canonical_header.begin(),
                 [](std::byte actual, std::uint8_t expected) { return actual == static_cast<std::byte>(expected); })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_header, "db object pristine header is not canonical");
         }
         header_found = true;
      } else if (with_revision && !payload && record.key == revision_key) {
         payload = std::move(record.value);
      } else {
         FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object family contains a non-pristine record");
      }
   }
   if (!header_found || (with_revision && !payload)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object pristine infrastructure is missing");
   }
   co_return payload;
}

forge::db::core::record_key access::record_key(forge::db::ids::object_id id) {
   return detail::record_key::object(id);
}

} // namespace forge::db::object::system
