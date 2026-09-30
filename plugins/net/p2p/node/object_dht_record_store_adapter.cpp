module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.db.core.record;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.snapshot;
import forge.db.object.transaction;
import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.db.store.api;

#include "details/object_dht_record_store_adapter.hxx"
#include "details/p2p_state_schema.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

[[nodiscard]] std::string current_exception_message() {
   try {
      throw;
   } catch (const std::exception& error) {
      return error.what();
   } catch (...) {
      return "unknown persistence failure";
   }
}

[[noreturn]] void malformed(std::string_view message) {
   FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::codec_error, "malformed ObjectDB DHT record state",
                         forge::exceptions::ctx("reason", message));
}

void require_row(bool condition, std::string_view message) {
   if (!condition) {
      malformed(message);
   }
}

void require_format(std::uint32_t format_version) {
   if (format_version != detail::p2p_state_schema::format_version) {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::incompatible_version,
                            "P2P state row format version is incompatible",
                            forge::exceptions::ctx("expected", detail::p2p_state_schema::format_version),
                            forge::exceptions::ctx("actual", format_version));
   }
}

void validate_profile(const forge::net::p2p::protocol_id& profile, std::size_t max_record_bytes) {
   if (max_record_bytes == 0 || profile.value.empty() || profile.value.front() != '/' ||
       profile.value.size() > max_record_bytes) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options,
                            "ObjectDB DHT persistence profile or record bound is invalid");
   }
}

void add_bounded_size(std::size_t& total, std::size_t value, std::size_t limit, std::string_view message) {
   require_row(total <= limit && value <= limit - total, message);
   total += value;
}

[[nodiscard]] std::string binary_text(const std::vector<std::uint8_t>& value) {
   if (value.empty()) {
      return {};
   }
   return std::string{reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::vector<std::uint8_t> binary_bytes(std::string_view value) {
   if (value.empty()) {
      return {};
   }
   const auto* first = reinterpret_cast<const std::uint8_t*>(value.data());
   return std::vector<std::uint8_t>{first, first + value.size()};
}

[[nodiscard]] std::int64_t time_to_ns(std::chrono::system_clock::time_point value) {
   const auto source = value.time_since_epoch();
   const auto converted = std::chrono::duration_cast<std::chrono::nanoseconds>(source);
   require_row(std::chrono::duration_cast<std::chrono::system_clock::duration>(converted) == source,
               "timestamp is outside the exact nanosecond persisted range");
   require_row(converted.count() >= 0, "timestamp precedes the Unix epoch");
   require_row(converted.count() < std::numeric_limits<std::int64_t>::max(),
               "timestamp uses the reserved maximum persisted value");
   return converted.count();
}

[[nodiscard]] std::chrono::system_clock::time_point time_from_ns(std::int64_t value) {
   require_row(value >= 0, "timestamp is negative");
   const auto source = std::chrono::nanoseconds{value};
   const auto converted = std::chrono::duration_cast<std::chrono::system_clock::duration>(source);
   require_row(std::chrono::duration_cast<std::chrono::nanoseconds>(converted) == source,
               "timestamp cannot be represented by the host clock");
   return std::chrono::system_clock::time_point{converted};
}

[[nodiscard]] forge::net::p2p::peer_id parse_peer(std::string_view value) {
   auto peer = forge::net::p2p::peer_id{.value = std::string{value}};
   require_row(forge::net::p2p::valid_peer_id(peer), "provider Peer ID is invalid");
   return peer;
}

[[nodiscard]] forge::multiformats::multiaddr parse_address_strict(std::string_view value) {
   auto address = forge::multiformats::multiaddr::parse(value);
   require_row(address.to_string() == value, "provider address is not canonical");
   return address;
}

void validate_value_row_bounds(const detail::p2p_state_schema::dht_value_row& value, std::string_view profile,
                               std::size_t max_record_bytes) {
   require_row(value.profile == profile, "DHT value row belongs to another profile");
   require_row(!value.key.empty(), "DHT value key is empty");
   require_row(value.ttl_seconds >= 0, "DHT value TTL is negative");
   require_row(value.expires_at_ns > 0, "DHT value expiry is missing");
   require_row(value.expires_at_ns < std::numeric_limits<std::int64_t>::max(),
               "DHT value expiry uses the reserved maximum value");
   if (value.publisher) {
      (void)parse_peer(*value.publisher);
   }
   auto bytes = std::size_t{};
   for (const auto size : {value.key.size(), value.value.size(), value.time_received.size(),
                           value.publisher ? value.publisher->size() : std::size_t{}}) {
      add_bounded_size(bytes, size, max_record_bytes, "DHT value row exceeds configured byte limit");
   }
}

void validate_provider_row_bounds(const detail::p2p_state_schema::dht_provider_row& value, std::string_view profile,
                                  std::size_t max_record_bytes) {
   require_row(value.profile == profile, "DHT provider row belongs to another profile");
   require_row(!value.key.empty(), "DHT provider key is empty");
   (void)parse_peer(value.peer);
   require_row(value.provider_expires_at_ns > 0, "DHT provider expiry is missing");
   require_row(value.provider_expires_at_ns < std::numeric_limits<std::int64_t>::max(),
               "DHT provider expiry uses the reserved maximum value");
   require_row(value.addresses_expires_at_ns >= 0, "DHT provider address expiry is negative");
   require_row(value.addresses_expires_at_ns < std::numeric_limits<std::int64_t>::max(),
               "DHT provider address expiry uses the reserved maximum value");
   require_row(value.endpoints.empty() || value.addresses_expires_at_ns > 0, "DHT provider addresses have no expiry");
   require_row(value.endpoints.size() <= max_record_bytes, "DHT provider endpoint count exceeds configured byte limit");
   auto bytes = std::size_t{};
   add_bounded_size(bytes, value.key.size(), max_record_bytes, "DHT provider row exceeds configured byte limit");
   add_bounded_size(bytes, value.peer.size(), max_record_bytes, "DHT provider row exceeds configured byte limit");
   for (const auto& endpoint : value.endpoints) {
      add_bounded_size(bytes, endpoint.size(), max_record_bytes, "DHT provider row exceeds configured byte limit");
      (void)parse_address_strict(endpoint);
   }
}

[[nodiscard]] detail::p2p_state_schema::dht_value_row
to_value_row(const forge::net::p2p::dht::record_store::value_record& value, std::string_view profile,
             std::size_t max_record_bytes) {
   auto row = detail::p2p_state_schema::dht_value_row{
       .profile = std::string{profile},
       .key = binary_text(value.record.key_value.bytes),
       .value = value.record.value,
       .time_received = value.record.time_received,
       .publisher = value.record.publisher ? std::optional<std::string>{value.record.publisher->value} : std::nullopt,
       .ttl_seconds = value.record.ttl.count(),
       .expires_at_ns = time_to_ns(value.expires_at),
   };
   validate_value_row_bounds(row, profile, max_record_bytes);
   return row;
}

[[nodiscard]] forge::net::p2p::dht::record_store::value_record
from_value_row(const detail::p2p_state_schema::dht_value_row& value, std::string_view profile,
               std::size_t max_record_bytes) {
   validate_value_row_bounds(value, profile, max_record_bytes);
   return forge::net::p2p::dht::record_store::value_record{
       .record =
           forge::net::p2p::dht::record{
               .key_value = forge::net::p2p::dht::key{.bytes = binary_bytes(value.key)},
               .value = value.value,
               .time_received = value.time_received,
               .publisher = value.publisher ? std::optional<forge::net::p2p::peer_id>{parse_peer(*value.publisher)}
                                            : std::nullopt,
               .ttl = std::chrono::seconds{value.ttl_seconds},
           },
       .expires_at = time_from_ns(value.expires_at_ns),
   };
}

[[nodiscard]] detail::p2p_state_schema::dht_provider_row
to_provider_row(const forge::net::p2p::dht::record_store::provider_record& value, std::string_view profile,
                std::size_t max_record_bytes) {
   auto row = detail::p2p_state_schema::dht_provider_row{
       .profile = std::string{profile},
       .key = binary_text(value.key.bytes),
       .peer = value.provider.value,
       .provider_expires_at_ns = time_to_ns(value.provider_expires_at),
       .addresses_expires_at_ns = time_to_ns(value.addresses_expires_at),
       .local_owned = value.local_owned,
   };
   row.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      row.endpoints.push_back(endpoint.to_string());
   }
   validate_provider_row_bounds(row, profile, max_record_bytes);
   return row;
}

[[nodiscard]] forge::net::p2p::dht::record_store::provider_record
from_provider_row(const detail::p2p_state_schema::dht_provider_row& value, std::string_view profile,
                  std::size_t max_record_bytes) {
   validate_provider_row_bounds(value, profile, max_record_bytes);
   auto record = forge::net::p2p::dht::record_store::provider_record{
       .key = forge::net::p2p::dht::key{.bytes = binary_bytes(value.key)},
       .provider = parse_peer(value.peer),
       .provider_expires_at = time_from_ns(value.provider_expires_at_ns),
       .addresses_expires_at = time_from_ns(value.addresses_expires_at_ns),
       .local_owned = value.local_owned,
   };
   record.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      record.endpoints.push_back(parse_address_strict(endpoint));
   }
   return record;
}

template <typename Object, typename Tag, typename Row, typename... Keys>
boost::asio::awaitable<void> upsert_row(forge::db::object::transaction& objects, Row row, const Keys&... keys) {
   auto existing = co_await objects.template index<Object, Tag>().find(keys...);
   if (existing) {
      row.id = existing->id;
      co_await objects.replace(std::move(row));
      co_return;
   }
   co_await objects.template create<Row>([row = std::move(row)](Row& created) mutable {
      const auto id = created.id;
      created = std::move(row);
      created.id = id;
   });
}

[[nodiscard]] forge::db::core::page_request
hydration_page_request(const forge::net::p2p::dht::record_store::hydration_request& request) {
   if (request.limit == 0 || request.limit > forge::db::core::max_page_limit) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options, "DHT hydration limit is invalid",
                            forge::exceptions::ctx("limit", request.limit),
                            forge::exceptions::ctx("max", forge::db::core::max_page_limit));
   }
   if (request.cursor && request.cursor->empty()) {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::invalid_cursor, "DHT hydration cursor is empty");
   }
   return forge::db::core::page_request{
       .after = request.cursor ? std::optional<forge::db::core::cursor>{forge::db::core::cursor{
                                     .boundary = forge::db::core::record_key{*request.cursor}}}
                               : std::nullopt,
       .limit = static_cast<std::uint32_t>(request.limit),
   };
}

void set_cursor(forge::net::p2p::dht::record_store::hydration_page& out,
                const std::optional<forge::db::core::cursor>& next) {
   if (next) {
      out.cursor = next->boundary.bytes();
   } else {
      out.cursor.reset();
   }
}

[[nodiscard]] std::int64_t exclusive_expiry_end(std::int64_t now_ns) {
   return now_ns + 1;
}

boost::asio::awaitable<std::vector<detail::p2p_state_schema::dht_value_row>>
expired_value_rows(forge::db::object::transaction& objects, std::string_view profile, std::int64_t now_ns,
                   std::uint32_t limit) {
   if (now_ns < 1 || limit == 0) {
      co_return std::vector<detail::p2p_state_schema::dht_value_row>{};
   }
   auto page = co_await objects
                   .index<detail::p2p_state_schema::dht_value_object, detail::p2p_state_schema::by_dht_value_expiry>()
                   .range(std::tuple{std::string{profile}, std::int64_t{1}},
                          std::tuple{std::string{profile}, exclusive_expiry_end(now_ns)})
                   .page(forge::db::core::page_request{.limit = limit});
   co_return std::move(page.items);
}

boost::asio::awaitable<std::vector<detail::p2p_state_schema::dht_provider_row>>
expired_provider_rows(forge::db::object::transaction& objects, std::string_view profile, std::int64_t now_ns,
                      std::uint32_t limit) {
   if (now_ns < 1 || limit == 0) {
      co_return std::vector<detail::p2p_state_schema::dht_provider_row>{};
   }
   auto page =
       co_await objects
           .index<detail::p2p_state_schema::dht_provider_object, detail::p2p_state_schema::by_dht_provider_expiry>()
           .range(std::tuple{std::string{profile}, std::int64_t{1}},
                  std::tuple{std::string{profile}, exclusive_expiry_end(now_ns)})
           .page(forge::db::core::page_request{.limit = limit});
   co_return std::move(page.items);
}

boost::asio::awaitable<std::vector<detail::p2p_state_schema::dht_provider_row>>
expired_provider_address_rows(forge::db::object::transaction& objects, std::string_view profile, std::int64_t now_ns,
                              std::uint32_t limit) {
   if (now_ns < 1 || limit == 0) {
      co_return std::vector<detail::p2p_state_schema::dht_provider_row>{};
   }
   auto page = co_await objects
                   .index<detail::p2p_state_schema::dht_provider_object,
                          detail::p2p_state_schema::by_dht_provider_addresses_expiry>()
                   .range(std::tuple{std::string{profile}, std::int64_t{1}},
                          std::tuple{std::string{profile}, exclusive_expiry_end(now_ns)})
                   .page(forge::db::core::page_request{.limit = limit});
   co_return std::move(page.items);
}

boost::asio::awaitable<bool> has_expired_rows(forge::db::object::transaction& objects, std::string_view profile,
                                              std::int64_t now_ns) {
   co_return !(co_await expired_value_rows(objects, profile, now_ns, 1)).empty() ||
       !(co_await expired_provider_rows(objects, profile, now_ns, 1)).empty() ||
       !(co_await expired_provider_address_rows(objects, profile, now_ns, 1)).empty();
}

} // namespace

object_dht_record_store_adapter::object_dht_record_store_adapter(forge::plugins::db::store::api* db,
                                                                 forge::plugins::db::store::store_handle store,
                                                                 forge::net::p2p::protocol_id profile,
                                                                 std::size_t max_record_bytes)
    : db_{db}, store_{std::move(store)}, profile_{std::move(profile)}, max_record_bytes_{max_record_bytes} {}

boost::asio::awaitable<std::shared_ptr<object_dht_record_store_adapter>> object_dht_record_store_adapter::async_open(
    forge::plugins::db::store::api* db, forge::plugins::db::store::store_handle store,
    forge::net::p2p::protocol_id profile, forge::net::p2p::dht::record_store::options limits) {
   if (!db || !store) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options,
                            "ObjectDB DHT persistence requires a named DB Store handle");
   }
   validate_profile(profile, limits.max_record_bytes);

   auto snapshot = co_await store.begin_read();
   auto objects = snapshot.objects();
   auto state = co_await objects.find(detail::p2p_state_schema::schema_state_id);
   if (!state) {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::incompatible_version, "P2P state schema marker is missing",
                            forge::exceptions::ctx("store", store.name()));
   }
   require_format(state->format_version);

   co_return std::shared_ptr<object_dht_record_store_adapter>{
       new object_dht_record_store_adapter{db, std::move(store), std::move(profile), limits.max_record_bytes}};
}

void object_dht_record_store_adapter::ensure_open() const {
   if (closed_.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::closed, "ObjectDB DHT record store adapter is closed");
   }
}

boost::asio::awaitable<forge::net::p2p::dht::record_store::hydration_page>
object_dht_record_store_adapter::async_hydrate(forge::net::p2p::dht::record_store::hydration_request request) {
   ensure_open();
   const auto page_request = hydration_page_request(request);
   auto snapshot = co_await store_.begin_read();
   auto objects = snapshot.objects();
   const auto state = co_await objects.get(detail::p2p_state_schema::schema_state_id);
   require_format(state.format_version);
   auto out = forge::net::p2p::dht::record_store::hydration_page{};

   switch (request.kind) {
   case forge::net::p2p::dht::record_store::hydration_kind::values: {
      auto page =
          co_await objects
              .index<detail::p2p_state_schema::dht_value_object, detail::p2p_state_schema::by_dht_value_profile_key>()
              .equal_range(profile_.value)
              .page(page_request);
      out.values.reserve(page.items.size());
      for (const auto& row : page.items) {
         out.values.push_back(from_value_row(row, profile_.value, max_record_bytes_));
      }
      set_cursor(out, page.next);
      break;
   }
   case forge::net::p2p::dht::record_store::hydration_kind::providers: {
      auto page = co_await objects
                      .index<detail::p2p_state_schema::dht_provider_object,
                             detail::p2p_state_schema::by_dht_provider_profile_key_peer>()
                      .equal_range(profile_.value)
                      .page(page_request);
      out.providers.reserve(page.items.size());
      for (const auto& row : page.items) {
         out.providers.push_back(from_provider_row(row, profile_.value, max_record_bytes_));
      }
      set_cursor(out, page.next);
      break;
   }
   }
   co_return out;
}

boost::asio::awaitable<forge::net::p2p::dht::record_store::apply_result>
object_dht_record_store_adapter::async_apply(forge::net::p2p::dht::record_store::mutation_batch batch) {
   ensure_open();

   auto value_rows = std::vector<detail::p2p_state_schema::dht_value_row>{};
   value_rows.reserve(batch.value_upserts.size());
   for (const auto& value : batch.value_upserts) {
      value_rows.push_back(to_value_row(value, profile_.value, max_record_bytes_));
   }
   auto value_removals = std::vector<std::string>{};
   value_removals.reserve(batch.value_removals.size());
   for (const auto& value : batch.value_removals) {
      require_row(!value.bytes.empty() && value.bytes.size() <= max_record_bytes_, "DHT value removal key is invalid");
      value_removals.push_back(binary_text(value.bytes));
   }
   auto provider_rows = std::vector<detail::p2p_state_schema::dht_provider_row>{};
   provider_rows.reserve(batch.provider_upserts.size());
   for (const auto& value : batch.provider_upserts) {
      provider_rows.push_back(to_provider_row(value, profile_.value, max_record_bytes_));
   }
   auto provider_removals = std::vector<std::pair<std::string, std::string>>{};
   provider_removals.reserve(batch.provider_removals.size());
   for (const auto& value : batch.provider_removals) {
      require_row(!value.key.bytes.empty() && value.key.bytes.size() <= max_record_bytes_,
                  "DHT provider removal key is invalid");
      require_row(forge::net::p2p::valid_peer_id(value.provider), "DHT provider removal Peer ID is invalid");
      provider_removals.emplace_back(binary_text(value.key.bytes), value.provider.value);
   }

   auto transaction = co_await store_.begin_transaction();
   auto objects = co_await store_.objects().join(transaction);
   const auto state = co_await objects.get(detail::p2p_state_schema::schema_state_id);
   require_format(state.format_version);

   for (const auto& key : value_removals) {
      auto existing =
          co_await objects
              .index<detail::p2p_state_schema::dht_value_object, detail::p2p_state_schema::by_dht_value_profile_key>()
              .find(profile_.value, key);
      if (existing) {
         co_await objects.erase(existing->id);
      }
   }
   for (const auto& key : provider_removals) {
      auto existing = co_await objects
                          .index<detail::p2p_state_schema::dht_provider_object,
                                 detail::p2p_state_schema::by_dht_provider_profile_key_peer>()
                          .find(profile_.value, key.first, key.second);
      if (existing) {
         co_await objects.erase(existing->id);
      }
   }
   for (auto& row : value_rows) {
      const auto key = row.key;
      co_await upsert_row<detail::p2p_state_schema::dht_value_object,
                          detail::p2p_state_schema::by_dht_value_profile_key>(objects, std::move(row), profile_.value,
                                                                              key);
   }
   for (auto& row : provider_rows) {
      const auto key = row.key;
      const auto peer = row.peer;
      co_await upsert_row<detail::p2p_state_schema::dht_provider_object,
                          detail::p2p_state_schema::by_dht_provider_profile_key_peer>(objects, std::move(row),
                                                                                      profile_.value, key, peer);
   }
   co_await transaction.commit();

   try {
      co_await db_->flush(store_.name(), true);
   } catch (...) {
      co_return forge::net::p2p::dht::record_store::apply_result{
          .durability_confirmed = false,
          .durability_failure = current_exception_message(),
      };
   }
   co_return forge::net::p2p::dht::record_store::apply_result{};
}

boost::asio::awaitable<forge::net::p2p::dht::record_store::prune_result>
object_dht_record_store_adapter::async_prune_expired(std::chrono::system_clock::time_point now, std::size_t limit) {
   ensure_open();
   if (limit == 0) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options, "DHT prune limit must be positive");
   }
   const auto now_ns = time_to_ns(now);
   const auto bounded_limit = static_cast<std::uint32_t>(std::min<std::size_t>(limit, forge::db::core::max_page_limit));

   auto transaction = co_await store_.begin_transaction();
   auto objects = co_await store_.objects().join(transaction);
   const auto state = co_await objects.get(detail::p2p_state_schema::schema_state_id);
   require_format(state.format_version);
   auto values = co_await expired_value_rows(objects, profile_.value, now_ns, bounded_limit);
   auto providers = co_await expired_provider_rows(objects, profile_.value, now_ns, bounded_limit);
   auto addresses = co_await expired_provider_address_rows(objects, profile_.value, now_ns, bounded_limit);

   for (const auto& row : values) {
      validate_value_row_bounds(row, profile_.value, max_record_bytes_);
   }
   for (const auto& row : providers) {
      validate_provider_row_bounds(row, profile_.value, max_record_bytes_);
   }
   for (const auto& row : addresses) {
      validate_provider_row_bounds(row, profile_.value, max_record_bytes_);
   }

   auto result = forge::net::p2p::dht::record_store::prune_result{};
   auto value_index = std::size_t{};
   auto provider_index = std::size_t{};
   auto address_index = std::size_t{};
   auto changed = false;
   while (result.values.size() + result.providers.size() + result.provider_address_updates.size() < bounded_limit) {
      if (value_index == values.size() && provider_index == providers.size() && address_index == addresses.size()) {
         break;
      }
      const auto value_expiry =
          value_index < values.size() ? values[value_index].expires_at_ns : std::numeric_limits<std::int64_t>::max();
      const auto provider_expiry = provider_index < providers.size() ? providers[provider_index].provider_expires_at_ns
                                                                     : std::numeric_limits<std::int64_t>::max();
      const auto address_expiry = address_index < addresses.size() ? addresses[address_index].addresses_expires_at_ns
                                                                   : std::numeric_limits<std::int64_t>::max();
      const auto next_expiry = std::min({value_expiry, provider_expiry, address_expiry});
      if (next_expiry > now_ns) {
         break;
      }

      if (value_index < values.size() && value_expiry == next_expiry) {
         const auto& row = values[value_index++];
         auto existing = co_await objects
                             .index<detail::p2p_state_schema::dht_value_object,
                                    detail::p2p_state_schema::by_dht_value_profile_key>()
                             .find(profile_.value, row.key);
         if (existing && existing->expires_at_ns == row.expires_at_ns) {
            result.values.push_back(forge::net::p2p::dht::key{.bytes = binary_bytes(row.key)});
            co_await objects.erase(existing->id);
            changed = true;
         }
         continue;
      }

      if (provider_index < providers.size() && provider_expiry == next_expiry) {
         const auto& row = providers[provider_index++];
         auto existing = co_await objects
                             .index<detail::p2p_state_schema::dht_provider_object,
                                    detail::p2p_state_schema::by_dht_provider_profile_key_peer>()
                             .find(profile_.value, row.key, row.peer);
         if (existing && existing->provider_expires_at_ns == row.provider_expires_at_ns) {
            result.providers.push_back(forge::net::p2p::dht::record_store::provider_key{
                .key = forge::net::p2p::dht::key{.bytes = binary_bytes(row.key)},
                .provider = parse_peer(row.peer),
            });
            co_await objects.erase(existing->id);
            changed = true;
         }
         continue;
      }

      const auto& row = addresses[address_index++];
      auto existing = co_await objects
                          .index<detail::p2p_state_schema::dht_provider_object,
                                 detail::p2p_state_schema::by_dht_provider_profile_key_peer>()
                          .find(profile_.value, row.key, row.peer);
      if (existing && existing->addresses_expires_at_ns == row.addresses_expires_at_ns) {
         if (existing->provider_expires_at_ns <= now_ns) {
            result.providers.push_back(forge::net::p2p::dht::record_store::provider_key{
                .key = forge::net::p2p::dht::key{.bytes = binary_bytes(row.key)},
                .provider = parse_peer(row.peer),
            });
            co_await objects.erase(existing->id);
         } else {
            existing->endpoints.clear();
            existing->addresses_expires_at_ns = 0;
            auto update = from_provider_row(*existing, profile_.value, max_record_bytes_);
            co_await objects.replace(std::move(*existing));
            result.provider_address_updates.push_back(std::move(update));
         }
         changed = true;
      }
   }

   result.may_have_more = co_await has_expired_rows(objects, profile_.value, now_ns);
   co_await transaction.commit();
   if (!changed) {
      co_return result;
   }
   try {
      co_await db_->flush(store_.name(), true);
   } catch (...) {
      result.durability = forge::net::p2p::dht::record_store::apply_result{
          .durability_confirmed = false,
          .durability_failure = current_exception_message(),
      };
   }
   co_return result;
}

boost::asio::awaitable<void> object_dht_record_store_adapter::async_flush() {
   ensure_open();
   co_await db_->flush(store_.name(), true);
}

boost::asio::awaitable<void> object_dht_record_store_adapter::async_close() {
   if (closed_.exchange(true, std::memory_order_acq_rel)) {
      co_return;
   }
   db_ = nullptr;
   store_ = {};
   co_return;
}

} // namespace forge::plugins::net::p2p::node
