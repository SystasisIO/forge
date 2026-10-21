module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

module forge.net.p2p.resource_manager;

import forge.log.logger;

namespace forge::net::p2p::detail {
void fail_next_service_bind_prepare_for_test() noexcept;
void fail_next_session_establish_prepare_for_test() noexcept;
void fail_next_dial_bind_prepare_for_test() noexcept;
void fail_next_malformed_record_prepare_for_test() noexcept;
void fail_next_lifecycle_reserve_prepare_for_test() noexcept;
void fail_next_session_reserve_prepare_for_test() noexcept;
void fail_next_dial_reserve_prepare_for_test() noexcept;
void fail_next_stream_reserve_prepare_for_test() noexcept;
void fail_next_relay_reserve_prepare_for_test() noexcept;
}

namespace forge::net::p2p {
namespace {

[[nodiscard]] peer_id test_peer(std::string value) {
   return peer_id{.value = std::move(value)};
}

[[nodiscard]] protocol_id test_protocol(std::string value) {
   return protocol_id{.value = std::move(value)};
}

class memory_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      {
         const auto lock = std::scoped_lock{_mutex};
         _records.push_back(record);
      }
      if (after_record)
         after_record();
   }

   std::vector<forge::log_record> snapshot() const {
      const auto lock = std::scoped_lock{_mutex};
      return _records;
   }

   std::function<void()> after_record;

 private:
   mutable std::mutex _mutex;
   std::vector<forge::log_record> _records;
};

struct memory_log_capture {
   memory_log_capture() : route{forge::logger::get("forge.net.p2p.resources")}, previous{route.get_log_level()} {
      route.set_log_level(forge::log_level::warn);
      route.add_sink(captured);
   }
   ~memory_log_capture() {
      route.remove_sink(captured);
      route.set_log_level(previous);
   }

   forge::logger route;
   forge::log_level previous;
   std::shared_ptr<memory_capture_sink> captured = std::make_shared<memory_capture_sink>();
};

std::string field_value(const forge::log_record& record, std::string_view key) {
   for (const auto& field : record.fields)
      if (field.key == key)
         return field.value;
   return {};
}

[[nodiscard]] bool bind_accepted(resource_manager::stream_reservation::bind_result result) noexcept {
   return result == resource_manager::stream_reservation::bind_result::accepted;
}

[[nodiscard]] bool bind_matches(resource_manager::stream_reservation::bind_result actual,
                                resource_manager::stream_reservation::bind_result expected) noexcept {
   return actual == expected;
}

[[nodiscard]] bool transition_accepted(resource_manager::transition_result result) noexcept {
   return result == resource_manager::transition_result::accepted;
}

[[nodiscard]] bool transition_matches(resource_manager::transition_result actual,
                                      resource_manager::transition_result expected) noexcept {
   return actual == expected;
}

template <typename T>
concept has_legacy_stream_limit = requires(T value) { value.max_streams; };

template <typename T>
concept has_queued_bytes_reservation = requires(T value) { value.reserve_queued_bytes(1U); };

template <typename T>
concept has_relay_stream_reservation = requires(T value) { value.reserve_relay_stream(); };

template <typename T>
concept has_operational_dials = requires(T value) { value.reserve_dial(); };

template <typename T>
concept has_pair_scopes = requires(T value) {
   value.protocol_peer;
   value.service_peer;
};

} // namespace

static_assert(!has_legacy_stream_limit<resource_manager::limits>);
static_assert(!has_queued_bytes_reservation<resource_manager>);
static_assert(!has_relay_stream_reservation<resource_manager>);
static_assert(has_operational_dials<resource_manager>);
static_assert(has_pair_scopes<resource_manager::limits>);

BOOST_AUTO_TEST_SUITE(resource_manager_ledger_tests)

BOOST_AUTO_TEST_CASE(resource_memory_rejection_keeps_exact_scope_snapshot_before_reentrant_release) {
   auto manager = resource_manager{resource_manager::limits{
       .protocol_peer = {.max_memory = 100},
       .stream = {.max_memory = 200},
   }};
   auto stream =
       manager.reserve_stream(test_peer("secret-sentinel peer"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   BOOST_REQUIRE(bind_accepted(stream->bind_protocol(test_protocol("secret-sentinel protocol"))));
   auto held = stream->reserve_memory(79, resource_manager::memory_priority::high);
   BOOST_REQUIRE(held);
   const auto logs = memory_log_capture{};
   logs.captured->after_record = [&] {
      BOOST_TEST(manager.current().system.memory == 79U);
      held->release();
   };
   BOOST_TEST(!stream->reserve_memory(1, resource_manager::memory_priority::high));
   logs.captured->after_record = {};
   BOOST_TEST(manager.current().system.memory == 0U);
   const auto records = logs.captured->snapshot();
   BOOST_REQUIRE_EQUAL(records.size(), 1U);
   const auto& record = records.front();
   BOOST_TEST(record.logger == "forge.net.p2p.resources");
   BOOST_TEST(record.level.value == forge::log_level::warn);
   BOOST_TEST(field_value(record, "event") == "p2p.memory_reservation_failed");
   BOOST_TEST(field_value(record, "reason") == "budget");
   BOOST_TEST(field_value(record, "scope") == "protocol_peer");
   BOOST_TEST(field_value(record, "dimension") == "priority");
   BOOST_TEST(field_value(record, "requested_bytes") == "1");
   BOOST_TEST(field_value(record, "priority") == "203");
   BOOST_TEST(field_value(record, "scope_usage") == "79");
   BOOST_TEST(field_value(record, "scope_limit") == "100");
   BOOST_TEST(field_value(record, "scope_threshold") == "79");
   BOOST_TEST(field_value(record, "reservation_memory") == "79");
   BOOST_TEST(field_value(record, "system_memory") == "79");
   BOOST_TEST(field_value(record, "denied_memory_before") == "0");
   BOOST_TEST(field_value(record, "denied_memory_after") == "1");
   BOOST_TEST(field_value(record, "invalid_transitions_before") == "0");
   BOOST_TEST(field_value(record, "invalid_transitions_after") == "0");
   BOOST_TEST(record.fields.size() == 15U);
   BOOST_TEST(forge::format_text_log_record(record).find("secret-sentinel") == std::string::npos);
   BOOST_TEST(forge::format_json_log_record(record).find("secret-sentinel") == std::string::npos);
   stream->release();
   BOOST_TEST(manager.current().system.outbound_streams == 0U);
}

BOOST_AUTO_TEST_CASE(resource_memory_rejection_keeps_first_budget_and_survives_throwing_sink) {
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_memory = 1},
       .stream = {.max_memory = 1},
   }};
   auto stream =
       manager.reserve_stream(test_peer("secret-sentinel peer"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   auto held = stream->reserve_memory(1);
   BOOST_REQUIRE(held);
   const auto logs = memory_log_capture{};
   logs.captured->after_record = [] { throw std::runtime_error{"secret-sentinel sink"}; };
   auto denied = false;
   BOOST_CHECK_NO_THROW(denied = !stream->reserve_memory(1));
   BOOST_TEST(denied);
   logs.captured->after_record = {};
   const auto records = logs.captured->snapshot();
   BOOST_REQUIRE_EQUAL(records.size(), 1U);
   BOOST_TEST(field_value(records.front(), "reason") == "budget");
   BOOST_TEST(field_value(records.front(), "scope") == "system");
   BOOST_TEST(field_value(records.front(), "dimension") == "memory");
   BOOST_TEST(field_value(records.front(), "scope_usage") == "1");
   BOOST_TEST(field_value(records.front(), "scope_limit") == "1");
   BOOST_TEST(field_value(records.front(), "scope_threshold") == "1");
   BOOST_TEST(manager.current().denied_memory == 1U);
   BOOST_TEST(manager.current().invalid_transitions == 0U);
   held->release();
   stream->release();
   BOOST_TEST(manager.current().system.memory == 0U);
}

BOOST_AUTO_TEST_CASE(resource_memory_rejection_distinguishes_inactive_moved_and_released_reservations) {
   auto manager = resource_manager{};
   const auto logs = memory_log_capture{};
   auto empty = resource_manager::stream_reservation{};
   BOOST_TEST(!empty.reserve_memory(1));
   auto stream =
       manager.reserve_stream(test_peer("secret-sentinel peer"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   auto moved = std::move(*stream);
   BOOST_TEST(!stream->reserve_memory(2));
   moved.release();
   BOOST_TEST(!moved.reserve_memory(3));
   const auto records = logs.captured->snapshot();
   BOOST_REQUIRE_EQUAL(records.size(), 3U);
   for (std::size_t index = 0; index < records.size(); ++index) {
      BOOST_TEST(field_value(records[index], "event") == "p2p.memory_reservation_failed");
      BOOST_TEST(field_value(records[index], "reason") == "inactive_reservation");
      BOOST_TEST(field_value(records[index], "scope") == "reservation");
      BOOST_TEST(field_value(records[index], "requested_bytes") == std::to_string(index + 1));
      BOOST_TEST(field_value(records[index], "priority") == "255");
      BOOST_TEST(records[index].fields.size() == 5U);
      BOOST_TEST(forge::format_json_log_record(records[index]).find("secret-sentinel") == std::string::npos);
   }
   BOOST_TEST(manager.current().denied_memory == 0U);
   BOOST_TEST(manager.current().invalid_transitions == 0U);
   BOOST_TEST(manager.current().system.outbound_streams == 0U);
}

BOOST_AUTO_TEST_CASE(resource_manager_charges_peer_before_protocol_negotiation) {
   auto manager = resource_manager{resource_manager::limits{
       .peer = {.max_inbound_streams = 1, .max_streams = 1},
   }};
   auto stream = manager.reserve_stream(test_peer("unnegotiated-peer"), resource_manager::session_direction::inbound);
   BOOST_REQUIRE(stream);
   BOOST_TEST(!stream->bound());
   BOOST_TEST(!stream->service_bound());

   const auto current = manager.current();
   BOOST_TEST(current.system.inbound_streams == 1U);
   BOOST_TEST(current.transient.inbound_streams == 1U);
   BOOST_TEST(current.peers.inbound_streams == 1U);
   BOOST_TEST(current.streams.inbound_streams == 1U);
   BOOST_TEST(current.protocols.inbound_streams == 0U);
   BOOST_TEST(current.active_peer_scopes == 1U);
   BOOST_TEST(!manager.reserve_stream(test_peer("unnegotiated-peer"), resource_manager::session_direction::inbound));
}

BOOST_AUTO_TEST_CASE(resource_manager_binds_protocol_then_service_in_distinct_atomic_steps) {
   auto manager = resource_manager{};
   auto stream = manager.reserve_stream(test_peer("staged-peer"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   BOOST_REQUIRE(bind_accepted(stream->bind_protocol(test_protocol("/staged/1"))));
   BOOST_TEST(stream->bound());
   BOOST_TEST(!stream->service_bound());
   auto protocol_bound = manager.current();
   BOOST_TEST(protocol_bound.transient.outbound_streams == 0U);
   BOOST_TEST(protocol_bound.peers.outbound_streams == 1U);
   BOOST_TEST(protocol_bound.protocols.outbound_streams == 1U);
   BOOST_TEST(protocol_bound.protocol_peers.outbound_streams == 1U);
   BOOST_TEST(protocol_bound.services.outbound_streams == 0U);

   BOOST_REQUIRE(bind_accepted(stream->bind_service("identify")));
   BOOST_TEST(stream->service_bound());
   const auto service_bound = manager.current();
   BOOST_TEST(service_bound.services.outbound_streams == 1U);
   BOOST_TEST(service_bound.service_peers.outbound_streams == 1U);
   BOOST_TEST(service_bound.active_service_scopes == 1U);
   BOOST_TEST(service_bound.active_service_peer_scopes == 1U);
}

BOOST_AUTO_TEST_CASE(resource_manager_migrates_session_children_from_transient_to_peer) {
   auto manager = resource_manager{resource_manager::limits{
       .transient = {.max_memory = 4, .max_file_descriptors = 1},
       .connection = {.max_file_descriptors = 2},
   }};
   auto session = manager.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(session);
   auto memory = session->reserve_memory(4);
   auto descriptors = session->reserve_file_descriptors(1);
   BOOST_REQUIRE(memory);
   BOOST_REQUIRE(descriptors);
   BOOST_REQUIRE(transition_accepted(session->establish(
       {.peer = test_peer("session-peer"), .direction = resource_manager::session_direction::outbound})));
   BOOST_TEST(session->established());
   const auto established = manager.current();
   BOOST_TEST(established.transient.memory == 0U);
   BOOST_TEST(established.transient.file_descriptors == 0U);
   BOOST_TEST(established.peers.memory == 4U);
   BOOST_TEST(established.peers.file_descriptors == 1U);
   BOOST_TEST(established.connections.memory == 4U);
   BOOST_TEST(established.connections.file_descriptors == 1U);

   auto memory_after_migration = session->reserve_memory(2);
   auto descriptors_after_migration = session->reserve_file_descriptors(1);
   BOOST_REQUIRE(memory_after_migration);
   BOOST_REQUIRE(descriptors_after_migration);
   BOOST_TEST(manager.current().peers.memory == 6U);
   BOOST_TEST(manager.current().peers.file_descriptors == 2U);

   session.reset();
   BOOST_TEST(manager.current().connections.outbound_connections == 0U);
   BOOST_TEST(manager.current().system.memory == 6U);
   memory.reset();
   descriptors.reset();
   memory_after_migration.reset();
   descriptors_after_migration.reset();
   BOOST_TEST(manager.current().system.memory == 0U);
   BOOST_TEST(manager.current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(resource_manager_isolates_protocol_peer_and_service_peer_limits) {
   auto manager = resource_manager{resource_manager::limits{
       .protocol_peer = {.max_streams = 1},
       .service_peer = {.max_streams = 1},
   }};
   auto first = manager.reserve_stream(test_peer("peer-a"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(first);
   BOOST_REQUIRE(bind_accepted(first->bind_protocol(test_protocol("/shared/1"))));

   auto protocol_rejected = manager.reserve_stream(test_peer("peer-a"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(protocol_rejected);
   BOOST_TEST(bind_matches(protocol_rejected->bind_protocol(test_protocol("/shared/1")),
                           resource_manager::stream_reservation::bind_result::policy_rejected));
   BOOST_TEST(!protocol_rejected->bound());
   BOOST_REQUIRE(bind_accepted(protocol_rejected->bind_protocol(test_protocol("/other/1"))));

   auto other_peer = manager.reserve_stream(test_peer("peer-b"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(other_peer);
   BOOST_REQUIRE(bind_accepted(other_peer->bind_protocol(test_protocol("/shared/1"))));

   BOOST_REQUIRE(bind_accepted(first->bind_service("shared-service")));
   BOOST_TEST(bind_matches(protocol_rejected->bind_service("shared-service"),
                           resource_manager::stream_reservation::bind_result::policy_rejected));
   BOOST_TEST(!protocol_rejected->service_bound());
   BOOST_REQUIRE(bind_accepted(other_peer->bind_service("shared-service")));
   BOOST_TEST(manager.current().active_protocol_peer_scopes == 3U);
   BOOST_TEST(manager.current().active_service_peer_scopes == 2U);
}

BOOST_AUTO_TEST_CASE(resource_manager_rejects_and_retries_staged_migrations_without_partial_accounting) {
   auto protocol_manager = resource_manager{resource_manager::limits{
       .protocol = {.max_memory = 3},
   }};
   auto first = protocol_manager.reserve_stream(test_peer("protocol-a"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(first);
   auto first_memory = first->reserve_memory(3);
   BOOST_REQUIRE(first_memory);
   BOOST_REQUIRE(bind_accepted(first->bind_protocol(test_protocol("/limited/1"))));

   auto second =
       protocol_manager.reserve_stream(test_peer("protocol-b"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(second);
   auto second_memory = second->reserve_memory(2);
   BOOST_REQUIRE(second_memory);
   const auto before_protocol = protocol_manager.current();
   BOOST_TEST(bind_matches(second->bind_protocol(test_protocol("/limited/1")),
                           resource_manager::stream_reservation::bind_result::policy_rejected));
   const auto after_protocol = protocol_manager.current();
   BOOST_TEST(!second->bound());
   BOOST_TEST(after_protocol.system.memory == before_protocol.system.memory);
   BOOST_TEST(after_protocol.transient.memory == before_protocol.transient.memory);
   BOOST_TEST(after_protocol.protocols.memory == before_protocol.protocols.memory);
   BOOST_TEST(after_protocol.denied_scope_migrations == before_protocol.denied_scope_migrations + 1U);
   BOOST_REQUIRE(bind_accepted(second->bind_protocol(test_protocol("/retry/1"))));

   auto service_manager = resource_manager{resource_manager::limits{
       .service = {.max_memory = 3},
   }};
   auto service_first =
       service_manager.reserve_stream(test_peer("service-a"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(service_first);
   auto service_first_memory = service_first->reserve_memory(3);
   BOOST_REQUIRE(service_first_memory);
   BOOST_REQUIRE(bind_accepted(service_first->bind_protocol(test_protocol("/service-a/1"))));
   BOOST_REQUIRE(bind_accepted(service_first->bind_service("limited-service")));

   auto service_second =
       service_manager.reserve_stream(test_peer("service-b"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(service_second);
   auto service_second_memory = service_second->reserve_memory(2);
   BOOST_REQUIRE(service_second_memory);
   BOOST_REQUIRE(bind_accepted(service_second->bind_protocol(test_protocol("/service-b/1"))));
   const auto before_service = service_manager.current();
   BOOST_TEST(bind_matches(service_second->bind_service("limited-service"),
                           resource_manager::stream_reservation::bind_result::policy_rejected));
   const auto after_service = service_manager.current();
   BOOST_TEST(!service_second->service_bound());
   BOOST_TEST(after_service.services.memory == before_service.services.memory);
   BOOST_TEST(after_service.service_peers.memory == before_service.service_peers.memory);
   BOOST_REQUIRE(bind_accepted(service_second->bind_service("retry-service")));
}

BOOST_AUTO_TEST_CASE(resource_manager_accessors_are_serialized_with_same_handle_transition) {
   auto manager = resource_manager{};
   auto stream = manager.reserve_stream(test_peer("concurrent-peer"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   auto start = std::atomic_bool{false};
   auto protocol_bound = std::atomic_bool{false};
   auto observed_bound = std::atomic_bool{false};
   auto writer = std::thread{[&] {
      while (!start.load(std::memory_order_acquire)) {
      }
      protocol_bound.store(bind_accepted(stream->bind_protocol(test_protocol("/concurrent/1"))),
                           std::memory_order_release);
   }};
   auto reader = std::thread{[&] {
      start.store(true, std::memory_order_release);
      for (auto attempt = 0; attempt < 1'000; ++attempt) {
         observed_bound.store(observed_bound.load(std::memory_order_relaxed) || stream->bound(),
                              std::memory_order_relaxed);
         static_cast<void>(stream->service_bound());
      }
   }};
   writer.join();
   reader.join();
   BOOST_TEST(protocol_bound.load(std::memory_order_acquire));
   BOOST_TEST(stream->bound());
   const auto observed_or_bound = observed_bound.load(std::memory_order_relaxed) || stream->bound();
   BOOST_TEST(observed_or_bound);
}

BOOST_AUTO_TEST_CASE(resource_manager_has_one_scope_limit_authority_and_progressive_memory_admission) {
   auto defaults = resource_manager{};
   const auto& configured = defaults.configured_limits();
   constexpr auto mib = 1024U * 1024U;
   BOOST_TEST(configured.system.max_memory == 128U * mib);
   BOOST_TEST(configured.system.max_file_descriptors == 256U);
   BOOST_TEST(configured.system.max_streams == 4096U);
   BOOST_TEST(configured.transient.max_memory == 32U * mib);
   BOOST_TEST(configured.transient.max_file_descriptors == 64U);
   BOOST_TEST(configured.transient.max_inbound_connections == 32U);
   BOOST_TEST(configured.transient.max_outbound_connections == 64U);
   BOOST_TEST(configured.transient.max_connections == 64U);
   BOOST_TEST(configured.transient.max_inbound_streams == 128U);
   BOOST_TEST(configured.transient.max_outbound_streams == 256U);
   BOOST_TEST(configured.transient.max_streams == 256U);
   BOOST_TEST(configured.peer.max_memory == 64U * mib);
   BOOST_TEST(configured.peer.max_file_descriptors == 4U);
   BOOST_TEST(configured.peer.max_inbound_connections == 8U);
   BOOST_TEST(configured.peer.max_outbound_connections == 8U);
   BOOST_TEST(configured.peer.max_connections == 8U);
   BOOST_TEST(configured.peer.max_inbound_streams == 256U);
   BOOST_TEST(configured.peer.max_outbound_streams == 512U);
   BOOST_TEST(configured.peer.max_streams == 512U);
   BOOST_TEST(configured.protocol.max_memory == 64U * mib);
   BOOST_TEST(configured.protocol.max_inbound_streams == 512U);
   BOOST_TEST(configured.protocol.max_outbound_streams == 2048U);
   BOOST_TEST(configured.protocol.max_streams == 2048U);
   BOOST_TEST(configured.protocol_peer.max_memory == 16U * mib);
   BOOST_TEST(configured.protocol_peer.max_inbound_streams == 64U);
   BOOST_TEST(configured.protocol_peer.max_outbound_streams == 128U);
   BOOST_TEST(configured.protocol_peer.max_streams == 256U);
   BOOST_TEST(configured.service.max_memory == 64U * mib);
   BOOST_TEST(configured.service.max_inbound_streams == 1024U);
   BOOST_TEST(configured.service.max_outbound_streams == 4096U);
   BOOST_TEST(configured.service.max_streams == 4096U);
   BOOST_TEST(configured.service_peer.max_memory == 16U * mib);
   BOOST_TEST(configured.service_peer.max_inbound_streams == 128U);
   BOOST_TEST(configured.service_peer.max_outbound_streams == 256U);
   BOOST_TEST(configured.service_peer.max_streams == 256U);
   BOOST_TEST(configured.connection.max_memory == 32U * mib);
   BOOST_TEST(configured.connection.max_file_descriptors == 1U);
   BOOST_TEST(configured.connection.max_inbound_connections == 1U);
   BOOST_TEST(configured.connection.max_outbound_connections == 1U);
   BOOST_TEST(configured.connection.max_connections == 1U);
   BOOST_TEST(configured.stream.max_memory == 16U * mib);
   BOOST_TEST(configured.stream.max_inbound_streams == 1U);
   BOOST_TEST(configured.stream.max_outbound_streams == 1U);
   BOOST_TEST(configured.stream.max_streams == 1U);
   BOOST_TEST(configured.max_dial_attempts == 1024U);
   BOOST_TEST(configured.max_dial_attempts_per_peer == 16U);
   BOOST_TEST(configured.max_relay_reservations == 1024U);
   BOOST_TEST(configured.max_malformed_messages_per_peer == 64U);

   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_memory = 100},
   }};
   auto lifecycle = manager.reserve_lifecycle();
   BOOST_REQUIRE(lifecycle);
   auto low = lifecycle->reserve_memory(39, resource_manager::memory_priority::low);
   BOOST_REQUIRE(low);
   BOOST_TEST(!lifecycle->reserve_memory(1, resource_manager::memory_priority::low));
   auto medium = lifecycle->reserve_memory(20, resource_manager::memory_priority::medium);
   BOOST_REQUIRE(medium);
   BOOST_TEST(!lifecycle->reserve_memory(1, resource_manager::memory_priority::medium));
   auto high = lifecycle->reserve_memory(20, resource_manager::memory_priority::high);
   BOOST_REQUIRE(high);
   BOOST_TEST(!lifecycle->reserve_memory(1, resource_manager::memory_priority::high));
   auto always = lifecycle->reserve_memory(21, resource_manager::memory_priority::always);
   BOOST_REQUIRE(always);
   BOOST_TEST(!lifecycle->reserve_memory(1, resource_manager::memory_priority::always));
   BOOST_TEST(manager.current().system.memory == 100U);
   BOOST_TEST(manager.current().denied_memory == 4U);

   constexpr auto unlimited_memory = (std::numeric_limits<std::uint64_t>::max)();
   auto unlimited = resource_manager{resource_manager::limits{
       .system = {.max_memory = unlimited_memory},
   }};
   auto unlimited_lifecycle = unlimited.reserve_lifecycle();
   BOOST_REQUIRE(unlimited_lifecycle);
   auto unlimited_low = unlimited_lifecycle->reserve_memory(unlimited_memory, resource_manager::memory_priority::low);
   BOOST_REQUIRE(unlimited_low);
}

BOOST_AUTO_TEST_CASE(resource_manager_enforces_directional_and_total_scope_dimensions) {
   auto inbound_connections = resource_manager{resource_manager::limits{
       .system = {.max_inbound_connections = 1},
   }};
   auto inbound_connection = inbound_connections.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(inbound_connection);
   BOOST_TEST(!inbound_connections.reserve_session(resource_manager::session_direction::inbound));
   auto inbound_allows_outbound = inbound_connections.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(inbound_allows_outbound);

   auto outbound_connections = resource_manager{resource_manager::limits{
       .system = {.max_outbound_connections = 1},
   }};
   auto outbound_connection = outbound_connections.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(outbound_connection);
   BOOST_TEST(!outbound_connections.reserve_session(resource_manager::session_direction::outbound));
   auto outbound_allows_inbound = outbound_connections.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(outbound_allows_inbound);

   auto total_connections = resource_manager{resource_manager::limits{
       .system = {.max_connections = 1},
   }};
   auto total_connection = total_connections.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(total_connection);
   BOOST_TEST(!total_connections.reserve_session(resource_manager::session_direction::outbound));

   auto inbound_streams = resource_manager{resource_manager::limits{
       .system = {.max_inbound_streams = 1},
   }};
   auto inbound_stream =
       inbound_streams.reserve_stream(test_peer("inbound-a"), resource_manager::session_direction::inbound);
   BOOST_REQUIRE(inbound_stream);
   BOOST_TEST(!inbound_streams.reserve_stream(test_peer("inbound-b"), resource_manager::session_direction::inbound));
   auto inbound_allows_outbound_stream =
       inbound_streams.reserve_stream(test_peer("inbound-c"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(inbound_allows_outbound_stream);

   auto outbound_streams = resource_manager{resource_manager::limits{
       .system = {.max_outbound_streams = 1},
   }};
   auto outbound_stream =
       outbound_streams.reserve_stream(test_peer("outbound-a"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(outbound_stream);
   BOOST_TEST(!outbound_streams.reserve_stream(test_peer("outbound-b"), resource_manager::session_direction::outbound));
   auto outbound_allows_inbound_stream =
       outbound_streams.reserve_stream(test_peer("outbound-c"), resource_manager::session_direction::inbound);
   BOOST_REQUIRE(outbound_allows_inbound_stream);

   auto total_streams = resource_manager{resource_manager::limits{
       .system = {.max_streams = 1},
   }};
   auto total_stream = total_streams.reserve_stream(test_peer("total-a"), resource_manager::session_direction::inbound);
   BOOST_REQUIRE(total_stream);
   BOOST_TEST(!total_streams.reserve_stream(test_peer("total-b"), resource_manager::session_direction::outbound));
}

BOOST_AUTO_TEST_CASE(resource_manager_separates_policy_rejection_from_invalid_and_runtime_failures) {
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_memory = 1},
   }};
   auto lifecycle = manager.reserve_lifecycle();
   BOOST_REQUIRE(lifecycle);
   auto held = lifecycle->reserve_memory(1);
   BOOST_REQUIRE(held);
   const auto before_limit = manager.current();
   BOOST_TEST(!lifecycle->reserve_memory(1));
   const auto after_limit = manager.current();
   BOOST_TEST(after_limit.denied == before_limit.denied + 1U);
   BOOST_TEST(after_limit.denied_memory == before_limit.denied_memory + 1U);
   BOOST_TEST(after_limit.runtime_failures == before_limit.runtime_failures);

   auto stream = manager.reserve_stream(test_peer("invalid-transition"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   const auto before_invalid = manager.current();
   BOOST_TEST(bind_matches(stream->bind_service("service-before-protocol"),
                           resource_manager::stream_reservation::bind_result::invalid_transition));
   const auto after_invalid = manager.current();
   BOOST_TEST(after_invalid.denied == before_invalid.denied);
   BOOST_TEST(after_invalid.invalid_transitions == before_invalid.invalid_transitions + 1U);
   BOOST_TEST(after_invalid.runtime_failures == before_invalid.runtime_failures);

   auto session = manager.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(session);
   detail::fail_next_session_establish_prepare_for_test();
   BOOST_TEST(transition_matches(
       session->establish(
           {.peer = test_peer("session-runtime"), .direction = resource_manager::session_direction::outbound}),
       resource_manager::transition_result::runtime_failure));
   BOOST_TEST(!session->established());

   auto dial = manager.reserve_dial();
   BOOST_REQUIRE(dial);
   detail::fail_next_dial_bind_prepare_for_test();
   BOOST_TEST(transition_matches(dial->bind(test_peer("dial-runtime")),
                                 resource_manager::transition_result::runtime_failure));
   BOOST_TEST(!dial->bound());

   detail::fail_next_malformed_record_prepare_for_test();
   BOOST_TEST(transition_matches(manager.record_malformed(test_peer("malformed-runtime")),
                                 resource_manager::transition_result::runtime_failure));

   const auto after_runtime = manager.current();
   BOOST_TEST(after_runtime.denied == after_invalid.denied);
   BOOST_TEST(after_runtime.runtime_failures == after_invalid.runtime_failures + 3U);
   BOOST_TEST(after_runtime.transient.outbound_connections == 1U);
   BOOST_TEST(after_runtime.peers.outbound_connections == 0U);
}

BOOST_AUTO_TEST_CASE(resource_manager_root_admissions_preserve_correlated_outcomes) {
   auto lifecycle_manager = resource_manager{};
   const auto lifecycle_before = lifecycle_manager.current();
   detail::fail_next_lifecycle_reserve_prepare_for_test();
   auto lifecycle = lifecycle_manager.reserve_lifecycle();
   BOOST_TEST(!lifecycle);
   BOOST_TEST(transition_matches(lifecycle.outcome(), resource_manager::transition_result::runtime_failure));
   const auto lifecycle_after = lifecycle_manager.current();
   BOOST_TEST(lifecycle_after.denied == lifecycle_before.denied);
   BOOST_TEST(lifecycle_after.runtime_failures == lifecycle_before.runtime_failures + 1U);

   auto session_manager = resource_manager{};
   const auto session_before = session_manager.current();
   detail::fail_next_session_reserve_prepare_for_test();
   auto session = session_manager.reserve_session(resource_manager::session_direction::inbound);
   BOOST_TEST(!session);
   BOOST_TEST(transition_matches(session.outcome(), resource_manager::transition_result::runtime_failure));
   const auto session_after = session_manager.current();
   BOOST_TEST(session_after.denied == session_before.denied);
   BOOST_TEST(session_after.runtime_failures == session_before.runtime_failures + 1U);
   BOOST_TEST(session_after.system.inbound_connections == 0U);
   BOOST_TEST(session_after.transient.inbound_connections == 0U);

   auto dial_manager = resource_manager{};
   const auto dial_before = dial_manager.current();
   detail::fail_next_dial_reserve_prepare_for_test();
   auto dial = dial_manager.reserve_dial();
   BOOST_TEST(!dial);
   BOOST_TEST(transition_matches(dial.outcome(), resource_manager::transition_result::runtime_failure));
   const auto dial_after = dial_manager.current();
   BOOST_TEST(dial_after.denied == dial_before.denied);
   BOOST_TEST(dial_after.runtime_failures == dial_before.runtime_failures + 1U);
   BOOST_TEST(dial_after.active_dials == 0U);

   auto invalid_dial_manager = resource_manager{resource_manager::limits{
       .max_dial_attempts = 0,
   }};
   const auto invalid_dial_before = invalid_dial_manager.current();
   detail::fail_next_dial_reserve_prepare_for_test();
   auto validation_first_dial = invalid_dial_manager.reserve_dial(test_peer(""));
   BOOST_TEST(!validation_first_dial);
   BOOST_TEST(
       transition_matches(validation_first_dial.outcome(), resource_manager::transition_result::invalid_transition));
   const auto invalid_dial_after = invalid_dial_manager.current();
   BOOST_TEST(invalid_dial_after.denied == invalid_dial_before.denied);
   BOOST_TEST(invalid_dial_after.invalid_transitions == invalid_dial_before.invalid_transitions + 1U);
   BOOST_TEST(invalid_dial_after.runtime_failures == invalid_dial_before.runtime_failures);

   auto valid_after_invalid = invalid_dial_manager.reserve_dial(test_peer("validation-first"));
   BOOST_TEST(!valid_after_invalid);
   BOOST_TEST(transition_matches(valid_after_invalid.outcome(), resource_manager::transition_result::policy_rejected));

   auto failpoint_probe = resource_manager{};
   auto failed_after_invalid = failpoint_probe.reserve_dial(test_peer("failpoint-remains-armed"));
   BOOST_TEST(!failed_after_invalid);
   BOOST_TEST(transition_matches(failed_after_invalid.outcome(), resource_manager::transition_result::runtime_failure));
   BOOST_TEST(failpoint_probe.current().active_dials == 0U);

   auto stream_manager = resource_manager{};
   const auto stream_before = stream_manager.current();
   detail::fail_next_stream_reserve_prepare_for_test();
   auto stream =
       stream_manager.reserve_stream(test_peer("stream-runtime"), resource_manager::session_direction::outbound);
   BOOST_TEST(!stream);
   BOOST_TEST(transition_matches(stream.outcome(), resource_manager::transition_result::runtime_failure));
   const auto stream_after = stream_manager.current();
   BOOST_TEST(stream_after.denied == stream_before.denied);
   BOOST_TEST(stream_after.runtime_failures == stream_before.runtime_failures + 1U);
   BOOST_TEST(stream_after.system.outbound_streams == 0U);
   BOOST_TEST(stream_after.transient.outbound_streams == 0U);
   BOOST_TEST(stream_after.active_peer_scopes == 0U);

   auto relay_manager = resource_manager{};
   const auto relay_before = relay_manager.current();
   detail::fail_next_relay_reserve_prepare_for_test();
   auto relay = relay_manager.reserve_relay(test_peer("relay-runtime"));
   BOOST_TEST(!relay);
   BOOST_TEST(transition_matches(relay.outcome(), resource_manager::transition_result::runtime_failure));
   const auto relay_after = relay_manager.current();
   BOOST_TEST(relay_after.denied == relay_before.denied);
   BOOST_TEST(relay_after.runtime_failures == relay_before.runtime_failures + 1U);
   BOOST_TEST(relay_after.active_relay_reservations == 0U);

   auto policy_manager = resource_manager{resource_manager::limits{
       .system = {.max_inbound_connections = 0},
   }};
   auto policy = policy_manager.reserve_session(resource_manager::session_direction::inbound);
   BOOST_TEST(!policy);
   BOOST_TEST(transition_matches(policy.outcome(), resource_manager::transition_result::policy_rejected));
   BOOST_TEST(policy_manager.current().denied_connections == 1U);

   auto dial_policy_manager = resource_manager{resource_manager::limits{
       .max_dial_attempts = 0,
   }};
   auto dial_policy = dial_policy_manager.reserve_dial();
   BOOST_TEST(!dial_policy);
   BOOST_TEST(transition_matches(dial_policy.outcome(), resource_manager::transition_result::policy_rejected));
   BOOST_TEST(dial_policy_manager.current().denied_dials == 1U);

   auto stream_policy_manager = resource_manager{resource_manager::limits{
       .system = {.max_streams = 0},
   }};
   auto stream_policy =
       stream_policy_manager.reserve_stream(test_peer("stream-policy"), resource_manager::session_direction::outbound);
   BOOST_TEST(!stream_policy);
   BOOST_TEST(transition_matches(stream_policy.outcome(), resource_manager::transition_result::policy_rejected));
   BOOST_TEST(stream_policy_manager.current().denied_streams == 1U);

   auto relay_policy_manager = resource_manager{resource_manager::limits{
       .max_relay_reservations = 0,
   }};
   auto relay_policy = relay_policy_manager.reserve_relay(test_peer("relay-policy"));
   BOOST_TEST(!relay_policy);
   BOOST_TEST(transition_matches(relay_policy.outcome(), resource_manager::transition_result::policy_rejected));
   BOOST_TEST(relay_policy_manager.current().denied_relays == 1U);

   auto invalid_manager = resource_manager{};
   auto invalid_stream =
       invalid_manager.reserve_stream(test_peer(""), resource_manager::session_direction::outbound);
   BOOST_TEST(!invalid_stream);
   BOOST_TEST(transition_matches(invalid_stream.outcome(), resource_manager::transition_result::invalid_transition));
   auto invalid_dial = invalid_manager.reserve_dial(test_peer(""));
   BOOST_TEST(!invalid_dial);
   BOOST_TEST(transition_matches(invalid_dial.outcome(), resource_manager::transition_result::invalid_transition));
   auto invalid_relay = invalid_manager.reserve_relay(test_peer(""));
   BOOST_TEST(!invalid_relay);
   BOOST_TEST(transition_matches(invalid_relay.outcome(), resource_manager::transition_result::invalid_transition));
   BOOST_TEST(invalid_manager.current().denied == 0U);
   BOOST_TEST(invalid_manager.current().invalid_transitions == 3U);

   auto accepted_manager = resource_manager{};
   auto accepted = accepted_manager.reserve_lifecycle();
   BOOST_REQUIRE(accepted);
   BOOST_TEST(transition_matches(accepted.outcome(), resource_manager::transition_result::accepted));
   BOOST_TEST(accepted->active());
}

BOOST_AUTO_TEST_CASE(resource_manager_root_results_normalize_move_and_reset_states) {
   auto manager = resource_manager{};
   auto accepted = manager.reserve_lifecycle();
   BOOST_REQUIRE(accepted);

   auto moved = std::move(accepted);
   BOOST_TEST(!accepted);
   BOOST_TEST(transition_matches(accepted.outcome(), resource_manager::transition_result::invalid_transition));
   BOOST_REQUIRE(moved);
   BOOST_TEST(transition_matches(moved.outcome(), resource_manager::transition_result::accepted));
   BOOST_TEST(moved->active());

   auto assigned = manager.reserve_lifecycle();
   BOOST_REQUIRE(assigned);
   assigned = std::move(moved);
   BOOST_TEST(!moved);
   BOOST_TEST(transition_matches(moved.outcome(), resource_manager::transition_result::invalid_transition));
   BOOST_REQUIRE(assigned);
   BOOST_TEST(transition_matches(assigned.outcome(), resource_manager::transition_result::accepted));
   BOOST_TEST(assigned->active());

   assigned.reset();
   BOOST_TEST(!assigned);
   BOOST_TEST(transition_matches(assigned.outcome(), resource_manager::transition_result::invalid_transition));

   auto policy_manager = resource_manager{resource_manager::limits{
       .max_dial_attempts = 0,
   }};
   auto rejected = policy_manager.reserve_dial();
   BOOST_TEST(!rejected);
   BOOST_TEST(transition_matches(rejected.outcome(), resource_manager::transition_result::policy_rejected));

   auto moved_rejected = std::move(rejected);
   BOOST_TEST(!rejected);
   BOOST_TEST(transition_matches(rejected.outcome(), resource_manager::transition_result::invalid_transition));
   BOOST_TEST(!moved_rejected);
   BOOST_TEST(transition_matches(moved_rejected.outcome(), resource_manager::transition_result::policy_rejected));
}

BOOST_AUTO_TEST_CASE(resource_manager_reports_service_bind_allocation_failure_without_policy_denial) {
   auto manager = resource_manager{};
   const auto protocol = test_protocol("/service-runtime/1");
   auto stream = manager.reserve_stream(test_peer("service-runtime"), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(stream);
   BOOST_REQUIRE(bind_accepted(stream->bind_protocol(protocol)));
   const auto before = manager.current();

   detail::fail_next_service_bind_prepare_for_test();
   BOOST_TEST(bind_matches(stream->bind_service("p2p.custom:/service-runtime/1"),
                           resource_manager::stream_reservation::bind_result::runtime_failure));

   const auto after_failure = manager.current();
   BOOST_TEST(!stream->service_bound());
   BOOST_TEST(after_failure.denied == before.denied);
   BOOST_TEST(after_failure.denied_scope_migrations == before.denied_scope_migrations);
   BOOST_TEST(after_failure.runtime_failures == before.runtime_failures + 1U);
   BOOST_TEST(after_failure.services.outbound_streams == before.services.outbound_streams);
   BOOST_TEST(after_failure.service_peers.outbound_streams == before.service_peers.outbound_streams);

   BOOST_REQUIRE(bind_accepted(stream->bind_service("p2p.custom:/service-runtime/1")));
   BOOST_TEST(stream->service_bound());
   stream.reset();
   const auto after_release = manager.current();
   BOOST_TEST(after_release.system.outbound_streams == 0U);
   BOOST_TEST(after_release.protocols.outbound_streams == 0U);
   BOOST_TEST(after_release.services.outbound_streams == 0U);
}

BOOST_AUTO_TEST_CASE(resource_manager_preserves_independent_operational_budgets) {
   auto manager = resource_manager{resource_manager::limits{
       .max_dial_attempts = 2,
       .max_dial_attempts_per_peer = 1,
       .max_relay_reservations = 1,
       .max_malformed_messages_per_peer = 1,
   }};

   auto first_dial = manager.reserve_dial();
   BOOST_REQUIRE(first_dial);
   BOOST_REQUIRE(transition_accepted(first_dial->bind(test_peer("dial-peer-a"))));
   auto second_dial = manager.reserve_dial();
   BOOST_REQUIRE(second_dial);
   BOOST_TEST(transition_matches(second_dial->bind(test_peer("dial-peer-a")),
                                 resource_manager::transition_result::policy_rejected));
   BOOST_REQUIRE(transition_accepted(second_dial->bind(test_peer("dial-peer-b"))));
   BOOST_TEST(!manager.reserve_dial());
   const auto dials = manager.current();
   BOOST_TEST(dials.active_dials == 2U);
   BOOST_TEST(dials.dial_attempt_scopes == 2U);
   BOOST_TEST(dials.denied_dials == 2U);

   auto relay = manager.reserve_relay(resource_manager::scope{
       .peer = test_peer("relay-peer-a"),
       .protocol = test_protocol("/relay/1"),
   });
   BOOST_REQUIRE(relay);
   BOOST_TEST(!manager.reserve_relay(test_peer("relay-peer-b")));
   const auto relays = manager.current();
   BOOST_TEST(relays.active_relay_reservations == 1U);
   BOOST_TEST(relays.relay_reservation_scopes == 1U);
   BOOST_TEST(relays.denied_relays == 1U);
   relay.reset();
   auto retried_relay = manager.reserve_relay(test_peer("relay-peer-b"));
   BOOST_REQUIRE(retried_relay);

   BOOST_REQUIRE(transition_accepted(manager.record_malformed(resource_manager::scope{
       .peer = test_peer("malformed-peer-a"),
       .protocol = test_protocol("/malformed/1"),
   })));
   BOOST_TEST(transition_matches(manager.record_malformed(test_peer("malformed-peer-a")),
                                 resource_manager::transition_result::policy_rejected));
   BOOST_REQUIRE(transition_accepted(manager.record_malformed(test_peer("malformed-peer-b"))));
   const auto malformed = manager.current();
   BOOST_TEST(malformed.malformed_scopes == 2U);
   BOOST_TEST(malformed.denied_malformed == 1U);

   auto zero = resource_manager{resource_manager::limits{
       .max_dial_attempts = 0,
       .max_relay_reservations = 0,
       .max_malformed_messages_per_peer = 0,
   }};
   BOOST_TEST(!zero.reserve_dial());
   BOOST_TEST(!zero.reserve_relay(test_peer("zero-relay")));
   BOOST_TEST(transition_matches(zero.record_malformed(test_peer("zero-malformed")),
                                 resource_manager::transition_result::policy_rejected));
   const auto zero_limits = zero.current();
   BOOST_TEST(zero_limits.denied == 3U);
   BOOST_TEST(zero_limits.denied_dials == 1U);
   BOOST_TEST(zero_limits.denied_relays == 1U);
   BOOST_TEST(zero_limits.denied_malformed == 1U);
   BOOST_TEST(zero_limits.invalid_transitions == 0U);

   auto zero_per_peer = resource_manager{resource_manager::limits{
       .max_dial_attempts = 1,
       .max_dial_attempts_per_peer = 0,
   }};
   auto zero_per_peer_dial = zero_per_peer.reserve_dial();
   BOOST_REQUIRE(zero_per_peer_dial);
   BOOST_TEST(transition_matches(zero_per_peer_dial->bind(test_peer("zero-dial-peer")),
                                 resource_manager::transition_result::policy_rejected));
   const auto zero_per_peer_limits = zero_per_peer.current();
   BOOST_TEST(zero_per_peer_limits.denied == 1U);
   BOOST_TEST(zero_per_peer_limits.denied_dials == 1U);
   BOOST_TEST(zero_per_peer_limits.invalid_transitions == 0U);

   auto invalid = resource_manager{};
   auto invalid_dial = invalid.reserve_dial();
   BOOST_REQUIRE(invalid_dial);
   BOOST_TEST(transition_matches(invalid_dial->bind(test_peer("")),
                                 resource_manager::transition_result::invalid_transition));
   BOOST_TEST(!invalid.reserve_relay(test_peer("")));
   BOOST_TEST(transition_matches(invalid.record_malformed(test_peer("")),
                                 resource_manager::transition_result::invalid_transition));
   const auto invalid_operations = invalid.current();
   BOOST_TEST(invalid_operations.denied == 0U);
   BOOST_TEST(invalid_operations.invalid_transitions == 3U);
}

BOOST_AUTO_TEST_CASE(resource_manager_serializes_concurrent_dial_binding) {
   auto manager = resource_manager{resource_manager::limits{
       .max_dial_attempts = 1,
       .max_dial_attempts_per_peer = 1,
   }};
   auto dial = manager.reserve_dial();
   BOOST_REQUIRE(dial);

   auto start = std::atomic_bool{false};
   auto first = std::atomic_bool{false};
   auto second = std::atomic_bool{false};
   auto bind = [&](std::atomic_bool& result, std::string peer) {
      while (!start.load(std::memory_order_acquire)) {
      }
      result.store(dial->bind(test_peer(std::move(peer))) == resource_manager::transition_result::accepted,
                   std::memory_order_release);
   };
   auto first_thread = std::thread{bind, std::ref(first), "dial-race-a"};
   auto second_thread = std::thread{bind, std::ref(second), "dial-race-b"};
   start.store(true, std::memory_order_release);
   first_thread.join();
   second_thread.join();

   const auto exactly_one_bound = first.load(std::memory_order_acquire) != second.load(std::memory_order_acquire);
   BOOST_TEST(exactly_one_bound);
   BOOST_TEST(dial->bound());
   BOOST_TEST(manager.current().active_dials == 1U);
   BOOST_TEST(manager.current().dial_attempt_scopes == 1U);
   dial.reset();
   BOOST_TEST(manager.current().active_dials == 0U);
   BOOST_TEST(manager.current().dial_attempt_scopes == 0U);
}

BOOST_AUTO_TEST_CASE(resource_manager_children_survive_parent_facades_and_check_overflow) {
   constexpr auto maximum_memory = (std::numeric_limits<std::uint64_t>::max)();
   constexpr auto maximum_descriptors = (std::numeric_limits<std::size_t>::max)();
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_memory = maximum_memory, .max_file_descriptors = maximum_descriptors},
       .transient = {.max_memory = maximum_memory, .max_file_descriptors = maximum_descriptors},
       .connection = {.max_memory = maximum_memory, .max_file_descriptors = maximum_descriptors},
   }};
   auto session = manager.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(session);
   auto memory = session->reserve_memory(maximum_memory);
   auto descriptors = session->reserve_file_descriptors(maximum_descriptors);
   BOOST_REQUIRE(memory);
   BOOST_REQUIRE(descriptors);
   BOOST_TEST(!session->reserve_memory(1));
   BOOST_TEST(!session->reserve_file_descriptors(1));
   session.reset();
   BOOST_TEST(manager.current().system.memory == maximum_memory);
   BOOST_TEST(manager.current().system.file_descriptors == maximum_descriptors);
   memory.reset();
   descriptors.reset();
   BOOST_TEST(manager.current().system.memory == 0U);
   BOOST_TEST(manager.current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(resource_manager_serializes_concurrent_reservations) {
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_memory = 16},
   }};
   auto exceeded = std::atomic_bool{false};
   auto workers = std::vector<std::thread>{};
   workers.reserve(8);
   for (auto worker = 0; worker < 8; ++worker) {
      workers.emplace_back([&] {
         for (auto attempt = 0; attempt < 100; ++attempt) {
            auto lifecycle = manager.reserve_lifecycle();
            if (!lifecycle) {
               continue;
            }
            auto memory = lifecycle->reserve_memory(1);
            if (memory && manager.current().system.memory > 16U) {
               exceeded.store(true, std::memory_order_relaxed);
            }
         }
      });
   }
   for (auto& worker : workers) {
      worker.join();
   }
   BOOST_TEST(!exceeded.load(std::memory_order_relaxed));
   BOOST_TEST(manager.current().system.memory == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
