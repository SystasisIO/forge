module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability;
import forge.net.p2p.rendezvous;
import forge.net.p2p.scoring;
import forge.plugins.db.store.api;
import forge.raw.raw;

#include "details/p2p_state_schema.hxx"
#include "details/object_peer_state_adapter.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

namespace p2p = forge::net::p2p;
using schema = forge::plugins::net::p2p::node::detail::p2p_state_schema;

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
   FORGE_THROW_EXCEPTION(p2p::exceptions::codec_error, "malformed ObjectDB peer state",
                         forge::exceptions::ctx("reason", message));
}

void require_row(bool condition, std::string_view message) {
   if (!condition) {
      malformed(message);
   }
}

void require_format(std::uint32_t format_version, std::string_view model) {
   if (format_version != schema::format_version) {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::incompatible_version,
                            "peer state row format version is incompatible", forge::exceptions::ctx("model", model),
                            forge::exceptions::ctx("expected", schema::format_version),
                            forge::exceptions::ctx("actual", format_version));
   }
}

void add_bounded_size(std::size_t& total, std::size_t value, std::size_t limit, std::string_view message) {
   require_row(total <= limit && value <= limit - total, message);
   total += value;
}

void validate_peer_row_bounds(const schema::peer_row& value, const p2p::peer_store::options& limits) {
   require_row(value.protocols.size() <= limits.max_protocols_per_peer, "peer protocol count exceeds configured limit");
   require_row(value.endpoints.size() <= limits.max_endpoints_per_peer, "peer endpoint count exceeds configured limit");
   require_row(value.relay_reservations.size() <= limits.max_relay_reservations_per_peer,
               "peer relay reservation count exceeds configured limit");
   auto bytes = std::size_t{};
   const auto add = [&](std::size_t size) {
      add_bounded_size(bytes, size, limits.max_peer_record_bytes, "peer row exceeds configured byte limit");
   };
   add(value.peer.size());
   add(value.protocol_version.size());
   add(value.agent_version.size());
   add(value.public_key.size());
   add(value.signed_peer_record.size());
   if (value.observed_endpoint) {
      add(value.observed_endpoint->size());
   }
   for (const auto& protocol : value.protocols) {
      add(protocol.size());
   }
   for (const auto& endpoint : value.endpoints) {
      add(endpoint.endpoint.size());
      if (endpoint.relay_peer) {
         add(endpoint.relay_peer->size());
      }
   }
   for (const auto& relay : value.relay_reservations) {
      require_row(relay.endpoints.size() <= limits.max_relay_endpoints_per_reservation,
                  "relay endpoint count exceeds configured limit");
      add(relay.relay.size());
      add(relay.voucher.size());
      for (const auto& endpoint : relay.endpoints) {
         add(endpoint.size());
      }
   }
}

void validate_rendezvous_row_bounds(const schema::rendezvous_row& value, const p2p::peer_store::options& limits) {
   require_row(value.endpoints.size() <= limits.max_endpoints_per_peer,
               "Rendezvous endpoint count exceeds configured limit");
   auto bytes = std::size_t{};
   for (const auto size : {value.namespace_name.size(), value.peer.size(), value.signed_peer_record.size()}) {
      add_bounded_size(bytes, size, limits.max_peer_record_bytes, "Rendezvous row exceeds configured byte limit");
   }
   for (const auto& endpoint : value.endpoints) {
      add_bounded_size(bytes, endpoint.size(), limits.max_peer_record_bytes,
                       "Rendezvous row exceeds configured byte limit");
   }
}

[[nodiscard]] std::int64_t time_to_ns(std::chrono::system_clock::time_point value) {
   const auto source = value.time_since_epoch();
   const auto converted = std::chrono::duration_cast<std::chrono::nanoseconds>(source);
   require_row(std::chrono::duration_cast<std::chrono::system_clock::duration>(converted) == source,
               "timestamp is outside the exact nanosecond persisted range");
   require_row(converted.count() >= 0, "timestamp precedes the Unix epoch");
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

[[nodiscard]] std::uint64_t score_priority(double value) {
   require_row(std::isfinite(value), "score is not finite");
   const auto bits = std::bit_cast<std::uint64_t>(value);
   constexpr auto sign = std::uint64_t{1} << 63U;
   return (bits & sign) != 0U ? ~bits : bits ^ sign;
}

[[nodiscard]] p2p::peer_id parse_peer(std::string_view value) {
   auto peer = p2p::peer_id{.value = std::string{value}};
   require_row(p2p::valid_peer_id(peer), "peer ID is invalid");
   return peer;
}

[[nodiscard]] p2p::endpoint parse_endpoint_strict(std::string_view value) {
   auto endpoint = p2p::parse_endpoint(value);
   require_row(endpoint.to_string() == value, "endpoint is not canonical");
   return endpoint;
}

[[nodiscard]] forge::multiformats::multiaddr parse_address_strict(std::string_view value) {
   auto address = forge::multiformats::multiaddr::parse(value);
   require_row(address.to_string() == value, "address is not canonical");
   return address;
}

[[nodiscard]] p2p::path::kind parse_path_kind(std::uint16_t value) {
   switch (value) {
   case 0:
      return p2p::path::kind::direct;
   case 1:
      return p2p::path::kind::hole_punch;
   case 2:
      return p2p::path::kind::relay;
   default:
      malformed("path kind is outside the supported range");
   }
}

[[nodiscard]] p2p::discovery::source parse_discovery_source(std::uint16_t value) {
   switch (value) {
   case 0:
      return p2p::discovery::source::explicit_config;
   case 1:
      return p2p::discovery::source::identify;
   case 2:
      return p2p::discovery::source::dht;
   case 3:
      return p2p::discovery::source::rendezvous;
   default:
      malformed("discovery source is outside the supported range");
   }
}

[[nodiscard]] p2p::reachability::state parse_reachability(std::uint16_t value) {
   switch (value) {
   case 0:
      return p2p::reachability::state::unknown;
   case 1:
      return p2p::reachability::state::publicly_reachable;
   case 2:
      return p2p::reachability::state::private_network;
   case 3:
      return p2p::reachability::state::blocked;
   case 4:
      return p2p::reachability::state::relay_only;
   default:
      malformed("reachability state is outside the supported range");
   }
}

[[nodiscard]] schema::endpoint_fact to_endpoint_fact(const p2p::peer_store::endpoint_record& value) {
   require_row(value.last_latency.count() >= 0, "endpoint latency is negative");
   (void)parse_path_kind(static_cast<std::uint16_t>(value.kind));
   if (value.relay_peer) {
      require_row(p2p::valid_peer_id(*value.relay_peer), "endpoint relay peer ID is invalid");
   }
   (void)score_priority(value.score);
   return schema::endpoint_fact{
       .endpoint = value.address.to_string(),
       .kind = static_cast<std::uint16_t>(value.kind),
       .relay_peer = value.relay_peer ? std::optional<std::string>{value.relay_peer->value} : std::nullopt,
       .successes = value.successes,
       .failures = value.failures,
       .last_latency_ms = value.last_latency.count(),
       .backoff_until_ns = time_to_ns(value.backoff_until),
       .score = value.score,
   };
}

[[nodiscard]] p2p::peer_store::endpoint_record from_endpoint_fact(const schema::endpoint_fact& value) {
   require_row(value.last_latency_ms >= 0, "endpoint latency is negative");
   (void)score_priority(value.score);
   return p2p::peer_store::endpoint_record{
       .address = parse_address_strict(value.endpoint),
       .kind = parse_path_kind(value.kind),
       .relay_peer = value.relay_peer ? std::optional<p2p::peer_id>{parse_peer(*value.relay_peer)} : std::nullopt,
       .successes = value.successes,
       .failures = value.failures,
       .last_latency = std::chrono::milliseconds{value.last_latency_ms},
       .backoff_until = time_from_ns(value.backoff_until_ns),
       .score = value.score,
   };
}

[[nodiscard]] schema::relay_fact to_relay_fact(const p2p::peer_store::relay_record& value) {
   require_row(p2p::valid_peer_id(value.relay), "relay peer ID is invalid");
   require_row(value.last_latency.count() >= 0, "relay latency is negative");
   (void)score_priority(value.score);
   auto row = schema::relay_fact{
       .relay = value.relay.value,
       .reservation_id = value.reservation_id,
       .expires_at_ns = time_to_ns(value.expires_at),
       .voucher = value.voucher,
       .successes = value.successes,
       .failures = value.failures,
       .last_latency_ms = value.last_latency.count(),
       .score = value.score,
   };
   row.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      row.endpoints.push_back(endpoint.to_string());
   }
   return row;
}

[[nodiscard]] p2p::peer_store::relay_record from_relay_fact(const schema::relay_fact& value) {
   require_row(value.last_latency_ms >= 0, "relay latency is negative");
   (void)score_priority(value.score);
   auto record = p2p::peer_store::relay_record{
       .relay = parse_peer(value.relay),
       .reservation_id = value.reservation_id,
       .expires_at = time_from_ns(value.expires_at_ns),
       .voucher = value.voucher,
       .successes = value.successes,
       .failures = value.failures,
       .last_latency = std::chrono::milliseconds{value.last_latency_ms},
       .score = value.score,
   };
   record.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      record.endpoints.push_back(parse_endpoint_strict(endpoint));
   }
   return record;
}

[[nodiscard]] schema::peer_row to_peer_row(const p2p::peer_store::record& value) {
   require_row(p2p::valid_peer_id(value.peer), "peer ID is invalid");
   require_row(value.last_latency.count() >= 0, "peer latency is negative");
   (void)parse_discovery_source(static_cast<std::uint16_t>(value.discovered_by));
   (void)parse_reachability(static_cast<std::uint16_t>(value.reachability));
   auto row = schema::peer_row{
       .peer = value.peer.value,
       .capabilities = value.capabilities.bits,
       .discovered_by = static_cast<std::uint16_t>(value.discovered_by),
       .protocol_version = value.protocol_version,
       .agent_version = value.agent_version,
       .public_key = value.public_key,
       .signed_peer_record = value.signed_peer_record,
       .reachability = static_cast<std::uint16_t>(value.reachability),
       .observed_endpoint =
           value.observed_endpoint ? std::optional<std::string>{value.observed_endpoint->to_string()} : std::nullopt,
       .reachability_expires_at_ns = time_to_ns(value.reachability_expires_at),
       .discovered_at_ns = time_to_ns(value.discovered_at),
       .discovery_expires_at_ns = time_to_ns(value.discovery_expires_at),
       .discovery_backoff_until_ns = time_to_ns(value.discovery_backoff_until),
       .successes = value.successes,
       .failures = value.failures,
       .last_latency_ms = value.last_latency.count(),
       .score = value.score,
       .hydration_priority = score_priority(value.score),
   };
   row.protocols.reserve(value.protocols.size());
   for (const auto& protocol : value.protocols) {
      require_row(!protocol.value.empty() && protocol.value.front() == '/', "protocol ID is invalid");
      row.protocols.push_back(protocol.value);
   }
   row.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      row.endpoints.push_back(to_endpoint_fact(endpoint));
   }
   row.relay_reservations.reserve(value.relay_reservations.size());
   for (const auto& relay : value.relay_reservations) {
      row.relay_reservations.push_back(to_relay_fact(relay));
   }
   return row;
}

[[nodiscard]] p2p::peer_store::record from_peer_row(const schema::peer_row& value) {
   require_row(value.last_latency_ms >= 0, "peer latency is negative");
   require_row(value.hydration_priority == score_priority(value.score), "peer hydration priority is inconsistent");
   auto record = p2p::peer_store::record{
       .peer = parse_peer(value.peer),
       .capabilities = p2p::capability_set{.bits = value.capabilities},
       .discovered_by = parse_discovery_source(value.discovered_by),
       .protocol_version = value.protocol_version,
       .agent_version = value.agent_version,
       .public_key = value.public_key,
       .signed_peer_record = value.signed_peer_record,
       .reachability = parse_reachability(value.reachability),
       .observed_endpoint = value.observed_endpoint
                                ? std::optional<p2p::endpoint>{parse_endpoint_strict(*value.observed_endpoint)}
                                : std::nullopt,
       .reachability_expires_at = time_from_ns(value.reachability_expires_at_ns),
       .discovered_at = time_from_ns(value.discovered_at_ns),
       .discovery_expires_at = time_from_ns(value.discovery_expires_at_ns),
       .discovery_backoff_until = time_from_ns(value.discovery_backoff_until_ns),
       .successes = value.successes,
       .failures = value.failures,
       .last_latency = std::chrono::milliseconds{value.last_latency_ms},
       .score = value.score,
   };
   record.protocols.reserve(value.protocols.size());
   for (const auto& protocol : value.protocols) {
      require_row(!protocol.empty() && protocol.front() == '/', "protocol ID is invalid");
      record.protocols.push_back(p2p::protocol_id{.value = protocol});
   }
   record.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      record.endpoints.push_back(from_endpoint_fact(endpoint));
   }
   record.relay_reservations.reserve(value.relay_reservations.size());
   for (const auto& relay : value.relay_reservations) {
      record.relay_reservations.push_back(from_relay_fact(relay));
   }
   return record;
}

[[nodiscard]] schema::rendezvous_row to_rendezvous_row(const p2p::rendezvous::registration& value) {
   require_row(!value.namespace_name.empty() && value.namespace_name.size() <= 255, "Rendezvous namespace is invalid");
   require_row(p2p::valid_peer_id(value.peer), "Rendezvous peer ID is invalid");
   require_row(value.ttl.count() >= 0, "Rendezvous TTL is negative");
   require_row(value.sequence > 0, "Rendezvous sequence is zero");
   auto row = schema::rendezvous_row{
       .namespace_name = value.namespace_name,
       .peer = value.peer.value,
       .signed_peer_record = value.signed_peer_record,
       .ttl_seconds = value.ttl.count(),
       .expires_at_ns = time_to_ns(value.expires_at),
       .sequence = value.sequence,
   };
   row.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      row.endpoints.push_back(endpoint.to_string());
   }
   return row;
}

[[nodiscard]] p2p::rendezvous::registration from_rendezvous_row(const schema::rendezvous_row& value) {
   require_row(!value.namespace_name.empty() && value.namespace_name.size() <= 255, "Rendezvous namespace is invalid");
   require_row(value.ttl_seconds >= 0, "Rendezvous TTL is negative");
   require_row(value.sequence > 0, "Rendezvous sequence is zero");
   auto registration = p2p::rendezvous::registration{
       .namespace_name = value.namespace_name,
       .peer = parse_peer(value.peer),
       .signed_peer_record = value.signed_peer_record,
       .ttl = std::chrono::seconds{value.ttl_seconds},
       .expires_at = time_from_ns(value.expires_at_ns),
       .sequence = value.sequence,
   };
   registration.endpoints.reserve(value.endpoints.size());
   for (const auto& endpoint : value.endpoints) {
      registration.endpoints.push_back(parse_address_strict(endpoint));
   }
   return registration;
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

template <typename Object, typename Tag>
boost::asio::awaitable<std::vector<typename Object::value_type>>
expired_rows(forge::db::object::transaction& objects, std::int64_t first_expiry_ns, std::int64_t now_ns,
             std::uint32_t limit) {
   if (now_ns < first_expiry_ns || limit == 0) {
      co_return std::vector<typename Object::value_type>{};
   }
   auto index = objects.template index<Object, Tag>();
   auto query = now_ns == std::numeric_limits<std::int64_t>::max() ? index.lower_bound(first_expiry_ns)
                                                                   : index.range(first_expiry_ns, now_ns + 1);
   auto page = co_await query.page(forge::db::core::page_request{.limit = limit});
   co_return std::move(page.items);
}

template <typename Object, typename Tag>
boost::asio::awaitable<bool> has_expired_rows(forge::db::object::transaction& objects, std::int64_t first_expiry_ns,
                                              std::int64_t now_ns) {
   auto rows = co_await expired_rows<Object, Tag>(objects, first_expiry_ns, now_ns, 1);
   co_return !rows.empty();
}

[[nodiscard]] forge::db::core::page_request hydration_page_request(const p2p::peer_store::hydration_request& request) {
   if (request.limit == 0 || request.limit > forge::db::core::max_page_limit) {
      FORGE_THROW_EXCEPTION(p2p::exceptions::invalid_options, "peer state hydration limit is invalid",
                            forge::exceptions::ctx("limit", request.limit),
                            forge::exceptions::ctx("max", forge::db::core::max_page_limit));
   }
   if (request.cursor && request.cursor->empty()) {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::invalid_cursor, "peer state hydration cursor is empty");
   }
   return forge::db::core::page_request{
       .after = request.cursor ? std::optional<forge::db::core::cursor>{forge::db::core::cursor{
                                     .boundary = forge::db::core::record_key{*request.cursor}}}
                               : std::nullopt,
       .limit = static_cast<std::uint32_t>(request.limit),
   };
}

void set_cursor(p2p::peer_store::hydration_page& out, const std::optional<forge::db::core::cursor>& next) {
   if (next) {
      out.cursor = next->boundary.bytes();
   } else {
      out.cursor.reset();
   }
}

} // namespace

object_peer_state_adapter::object_peer_state_adapter(forge::plugins::db::store::api* db,
                                                     forge::plugins::db::store::store_handle store,
                                                     p2p::peer_store::options limits)
    : db_{db}, store_{std::move(store)}, limits_{std::move(limits)} {}

boost::asio::awaitable<std::shared_ptr<object_peer_state_adapter>>
object_peer_state_adapter::async_open(forge::plugins::db::store::api* db, forge::plugins::db::store::store_handle store,
                                      p2p::peer_store::options limits, bool reset_incompatible_cache) {
   co_await schema::async_prepare(db, store, reset_incompatible_cache);

   co_return std::shared_ptr<object_peer_state_adapter>{
       new object_peer_state_adapter{db, std::move(store), std::move(limits)}};
}

void object_peer_state_adapter::ensure_open() const {
   if (closed_.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(p2p::exceptions::closed, "ObjectDB peer state adapter is closed");
   }
}

boost::asio::awaitable<p2p::peer_store::hydration_page>
object_peer_state_adapter::async_hydrate(p2p::peer_store::hydration_request request) {
   ensure_open();
   const auto page_request = hydration_page_request(request);
   auto snapshot = co_await store_.begin_read();
   auto objects = snapshot.objects();
   auto out = p2p::peer_store::hydration_page{};
   const auto state = co_await objects.get(schema::schema_state_id);
   require_format(state.format_version, "schema_state");
   out.rendezvous_sequence_high_watermark = state.rendezvous_sequence;

   switch (request.kind) {
   case p2p::peer_store::hydration_kind::peers: {
      out.peers.reserve(request.limit);
      auto next = page_request.after;
      while (out.peers.size() < request.limit) {
         auto page = co_await objects.index<schema::peer_object, schema::by_peer_hydration>()
                         .lower_bound(std::numeric_limits<std::uint64_t>::max())
                         .page({.after = next, .limit = 1});
         if (page.items.empty()) {
            out.cursor.reset();
            break;
         }
         validate_peer_row_bounds(page.items.front(), limits_);
         out.peers.push_back(from_peer_row(page.items.front()));
         set_cursor(out, page.next);
         if (!page.next) {
            break;
         }
         next = page.next;
      }
      break;
   }
   case p2p::peer_store::hydration_kind::rendezvous: {
      out.rendezvous_registrations.reserve(request.limit);
      auto next = page_request.after;
      while (out.rendezvous_registrations.size() < request.limit) {
         auto page = co_await objects.index<schema::rendezvous_object, schema::by_rendezvous_sequence>()
                         .lower_bound(std::numeric_limits<std::uint64_t>::max())
                         .page({.after = next, .limit = 1});
         if (page.items.empty()) {
            out.cursor.reset();
            break;
         }
         validate_rendezvous_row_bounds(page.items.front(), limits_);
         out.rendezvous_registrations.push_back(from_rendezvous_row(page.items.front()));
         set_cursor(out, page.next);
         if (!page.next) {
            break;
         }
         next = page.next;
      }
      break;
   }
   }
   co_return out;
}

boost::asio::awaitable<p2p::peer_store::apply_result>
object_peer_state_adapter::async_apply(p2p::peer_store::mutation_batch batch) {
   ensure_open();
   auto transaction = co_await store_.begin_transaction();
   auto objects = co_await store_.objects().join(transaction);
   const auto state = co_await objects.get(schema::schema_state_id);
   require_format(state.format_version, "schema_state");

   for (const auto& value : batch.peer_upserts) {
      auto row = to_peer_row(value);
      const auto peer = row.peer;
      co_await upsert_row<schema::peer_object, schema::by_peer_id>(objects, std::move(row), peer);
   }
   for (const auto& value : batch.peer_removals) {
      require_row(p2p::valid_peer_id(value), "peer removal ID is invalid");
      auto existing = co_await objects.index<schema::peer_object, schema::by_peer_id>().find(value.value);
      if (existing) {
         co_await objects.erase(existing->id);
      }
   }
   auto high_watermark = std::max(state.rendezvous_sequence, batch.rendezvous_sequence_high_watermark);
   for (const auto& value : batch.rendezvous_upserts) {
      auto row = to_rendezvous_row(value);
      const auto namespace_name = row.namespace_name;
      const auto peer = row.peer;
      high_watermark = std::max(high_watermark, row.sequence);
      co_await upsert_row<schema::rendezvous_object, schema::by_rendezvous_namespace_peer>(objects, std::move(row),
                                                                                           namespace_name, peer);
   }
   for (const auto& value : batch.rendezvous_removals) {
      require_row(!value.namespace_name.empty() && value.namespace_name.size() <= 255,
                  "Rendezvous removal namespace is invalid");
      require_row(p2p::valid_peer_id(value.peer), "Rendezvous removal peer ID is invalid");
      auto existing = co_await objects.index<schema::rendezvous_object, schema::by_rendezvous_namespace_peer>().find(
          value.namespace_name, value.peer.value);
      if (existing) {
         co_await objects.erase(existing->id);
      }
   }
   if (high_watermark != state.rendezvous_sequence) {
      co_await objects.modify(schema::schema_state_id, [high_watermark](schema::schema_state& current) {
         current.rendezvous_sequence = high_watermark;
      });
   }
   co_await transaction.commit();
   if (batch.durable) {
      try {
         co_await db_->flush(store_.name(), true);
      } catch (...) {
         co_return p2p::peer_store::apply_result{
             .durability_confirmed = false,
             .durability_failure = current_exception_message(),
         };
      }
   }
   co_return p2p::peer_store::apply_result{};
}

boost::asio::awaitable<p2p::peer_store::prune_result>
object_peer_state_adapter::async_prune_expired(std::chrono::system_clock::time_point now, std::size_t limit) {
   ensure_open();
   if (limit == 0) {
      FORGE_THROW_EXCEPTION(p2p::exceptions::invalid_options, "peer state prune limit must be positive");
   }
   const auto now_ns = time_to_ns(now);
   auto remaining = static_cast<std::uint32_t>(std::min<std::size_t>(limit, forge::db::core::max_page_limit));
   auto result = p2p::peer_store::prune_result{};

   auto transaction = co_await store_.begin_transaction();
   auto objects = co_await store_.objects().join(transaction);
   const auto state = co_await objects.get(schema::schema_state_id);
   require_format(state.format_version, "schema_state");

   const auto first_kind = next_prune_kind_.fetch_add(1, std::memory_order_relaxed) % 2U;
   for (auto offset = std::uint8_t{0}; offset < 2U && remaining > 0; ++offset) {
      switch ((first_kind + offset) % 2U) {
      case 0: {
         auto rows = co_await expired_rows<schema::peer_object, schema::by_peer_expiry>(objects, 1, now_ns, remaining);
         for (const auto& row : rows) {
            result.peers.push_back(parse_peer(row.peer));
            co_await objects.erase(row.id);
         }
         remaining -= static_cast<std::uint32_t>(rows.size());
         break;
      }
      case 1: {
         auto rows = co_await expired_rows<schema::rendezvous_object, schema::by_rendezvous_expiry>(objects, 0, now_ns,
                                                                                                    remaining);
         for (const auto& row : rows) {
            result.rendezvous_registrations.push_back(from_rendezvous_row(row));
            co_await objects.erase(row.id);
         }
         remaining -= static_cast<std::uint32_t>(rows.size());
         break;
      }
      default:
         std::unreachable();
      }
   }

   result.may_have_more =
       co_await has_expired_rows<schema::peer_object, schema::by_peer_expiry>(objects, 1, now_ns) ||
       co_await has_expired_rows<schema::rendezvous_object, schema::by_rendezvous_expiry>(objects, 0, now_ns);
   co_await transaction.commit();
   co_return result;
}

boost::asio::awaitable<void> object_peer_state_adapter::async_flush() {
   ensure_open();
   co_await db_->flush(store_.name(), true);
}

boost::asio::awaitable<void> object_peer_state_adapter::async_close() {
   if (closed_.exchange(true, std::memory_order_acq_rel)) {
      co_return;
   }
   db_ = nullptr;
   store_ = {};
   co_return;
}

} // namespace forge::plugins::net::p2p::node
