module;

#include <boost/asio/strand.hpp>

#include "../../libraries/net/p2p/details/mdns_registry.hxx"
#include "../../libraries/net/p2p/details/interface_watcher.hxx"
#include "../../libraries/net/p2p/details/mdns_wire.hxx"
#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.net.p2p.exceptions;
import forge.net.p2p.lifecycle;
import forge.net.p2p.resource_manager;

#include "../../libraries/net/p2p/details/mdns_service.hxx"

namespace forge::net::p2p::detail {
// Narrow private bridge: use production response admission without native I/O
// or wall-clock sleeps; no runtime/test switch is present in the service.
struct mdns_service_fixture {
   static void goodbye_sections(mdns_service& service) {
      service._instance = std::string(32, 'a');
      auto owner = receiver(service);
      auto advertised = response(service);
      advertised.additionals.push_back(advertised.answers.back());
      advertised.answers.pop_back();
      owner->advertisement = advertised;
      service._callbacks.listeners = [] { return std::vector<endpoint>{}; };
      service.refresh();
      BOOST_REQUIRE_EQUAL(owner->outbound.size(), 1U);
      const auto goodbye = mdns_codec::decode(owner->outbound.front().bytes);
      BOOST_REQUIRE_EQUAL(goodbye.answers.size(), advertised.answers.size());
      BOOST_REQUIRE_EQUAL(goodbye.additionals.size(), advertised.additionals.size());
      for (auto& record : advertised.answers) { record.ttl = 0; }
      for (auto& record : advertised.additionals) { record.ttl = 0; }
      BOOST_CHECK(goodbye == advertised);
   }
   static std::shared_ptr<mdns_service::worker> receiver(mdns_service& service) {
      auto interface = interface_state::interface{.index = 7, .generation = 1, .up = true,
          .running = true, .multicast = true,
          .addresses = {{.value = boost::asio::ip::make_address("192.0.2.10"), .prefix_length = 24,
                         .flags_known = true}}};
      auto owner = std::make_shared<mdns_service::worker>(service._strand, interface, false,
          interface.addresses.front().value, 8932);
      service._workers.emplace(std::pair{7U, false}, owner);
      service._registry = std::make_unique<mdns_registry>(service._policy, mdns_registry::limits{},
                                                         service._service, peer_id{});
      service._registry->replace_interfaces(std::vector{interface});
      service._addresses_changed = false;
      return owner;
   }

   static mdns_codec::message response(const mdns_service& service) {
      auto instance = service._service;
      instance.insert(instance.begin(), "remote");
      const auto text = "dnsaddr=/ip4/192.0.2.20/tcp/4001/p2p/" + service._local.value;
      return {.head = {.flags = 0x8400}, .answers = {
          {service._service, mdns_codec::type_ptr, 1, 1, mdns_codec::ptr{instance}},
          {instance, mdns_codec::type_txt, 1, 1,
           mdns_codec::txt{{mdns_codec::bytes{text.begin(), text.end()}}}}}};
   }

   static forge::net::transport::datagram_io::received route() {
      return {.remote = {boost::asio::ip::make_address("192.0.2.20"), 40000},
              .local = mdns_wire::group(false, 7), .interface_index = 7};
   }

   static void receive_boundary(mdns_service& service) {
      using namespace std::chrono_literals;
      auto owner = receiver(service);
      auto published = std::vector<std::size_t>{};
      service._callbacks.replace = [&](auto leases) { published.push_back(leases.size()); };
      const auto now = mdns_service::time_point{100s};
      for (const auto flags : {0x8c00U, 0x8403U}) {
         auto packet = response(service);
         packet.head.flags = static_cast<std::uint16_t>(flags);
         service.process_packet(*owner, mdns_codec::encode(packet), route(), now);
         BOOST_TEST(service._registry->record_count() == 0U);
      }
      const auto bytes = mdns_codec::encode(response(service));
      auto wrong = route();
      wrong.interface_index = 8;
      service.process_packet(*owner, bytes, wrong, now);
      service.process_packet(*owner, std::span{bytes}.first(3), route(), now);
      auto stale = std::make_shared<mdns_service::worker>(service._strand, owner->interface, false,
                                                        owner->source, 8932);
      stale->interface.generation = 0;
      service.process_packet(*stale, bytes, route(), now);
      BOOST_TEST(published.empty());
      BOOST_TEST(service._registry->record_count() == 0U);
      service.process_packet(*owner, bytes, route(), now);
      BOOST_REQUIRE_EQUAL(published.size(), 1U);
      BOOST_TEST(published.back() == 1U);
      BOOST_TEST(owner->outbound.empty());
      service.publish(now + 999ms);
      BOOST_TEST(published.back() == 1U);
      service._registry->expire(now + 1s);
      service.publish(now + 1s);
      BOOST_TEST(published.back() == 0U);
      service._callbacks.replace = [](auto) {};
   }

   static void cross_interface_dispatch(mdns_service& service) {
      auto target = receiver(service);
      auto interface = target->interface;
      interface.index = 8;
      interface.addresses.front().value = boost::asio::ip::make_address("198.51.100.10");
      auto socket_owner = std::make_shared<mdns_service::worker>(service._strand, interface, false,
          interface.addresses.front().value, 8932);
      service._workers.emplace(std::pair{8U, false}, socket_owner);
      service._registry->replace_interfaces(std::vector{target->interface, interface});
      auto evidence = std::vector<mdns_registry::lease>{};
      auto publications = 0U;
      service._callbacks.replace = [&](auto leases) { evidence = std::move(leases); ++publications; };
      auto incoming = route();
      incoming.local = {target->source, 5353};
      const auto now = std::chrono::steady_clock::now();
      // Linux reuseport can select socket 8 while pktinfo proves ingress on 7.
      service.process_packet(*socket_owner, mdns_codec::encode(response(service)), incoming, now);
      BOOST_CHECK_EQUAL(evidence.size(), 1U);
      if (!evidence.empty()) {
         BOOST_TEST(evidence.front().interface_index == 7U);
         BOOST_TEST(evidence.front().generation == target->interface.generation);
      }
      target->advertisement = response(service);
      service.process_packet(*socket_owner, mdns_codec::encode(mdns_wire::query(service._service)), incoming, now);
      BOOST_TEST(target->outbound.size() == 2U);
      BOOST_TEST(socket_owner->outbound.empty());
      for (const auto& packet : target->outbound) { BOOST_CHECK(packet.source == target->source); }
      const auto before = publications;
      service.stop_worker(*socket_owner);
      ++interface.generation;
      auto replacement = std::make_shared<mdns_service::worker>(service._strand, interface, false,
          interface.addresses.front().value, 8932);
      service._workers[{8U, false}] = replacement;
      service._registry->replace_interfaces(std::vector{target->interface, interface});
      service.process_packet(*socket_owner, mdns_codec::encode(response(service)), incoming, now);
      BOOST_TEST(publications == before); // Old receiving generation cannot forward late packets.
      service.stop_worker(*target);
      service.process_packet(*replacement, mdns_codec::encode(response(service)), incoming, now);
      BOOST_TEST(publications == before); // Nor can a current source revive a stopped target.
      service._callbacks.replace = [](auto) {};
   }

   static void expiry_timer(boost::asio::io_context& io, mdns_service& service) {
      using namespace std::chrono_literals;
      auto owner = receiver(service);
      auto published = std::vector<std::size_t>{};
      service._callbacks.replace = [&](auto leases) { published.push_back(leases.size()); };
      auto maintenance = boost::asio::co_spawn(service._strand, service.maintain(), boost::asio::use_future);
      io.poll(); // Empty registry has reached the distant query-deadline wait.
      BOOST_CHECK(!published.empty());
      const auto start = std::chrono::steady_clock::now();
      boost::asio::post(service._strand, [&] {
         service.process_packet(*owner, mdns_codec::encode(response(service)), route(), std::chrono::steady_clock::now());
      });
      io.restart();
      io.run_for(1500ms); // Independent bound, far shorter than query_interval (60s).
      const auto live = std::find(published.begin(), published.end(), 1U);
      BOOST_CHECK(live != published.end());
      if (live != published.end()) { BOOST_CHECK(std::find(live, published.end(), 0U) != published.end()); }
      BOOST_TEST(owner->outbound.empty()); // No second packet/query drove expiry.
      BOOST_CHECK(std::chrono::steady_clock::now() - start < service._policy.query_interval);
      service.request_stop();
      io.restart();
      io.run_for(1s);
      BOOST_REQUIRE(maintenance.wait_for(0ms) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(maintenance.get());
      service._callbacks.replace = [](auto) {};
   }

   static void responses(mdns_service& service, bool flood) {
      namespace ip = boost::asio::ip;
      using namespace std::chrono_literals;
      auto interface = interface_state::interface{};
      interface.index = 7;
      interface.generation = 1;
      interface.up = interface.running = interface.multicast = true;
      auto address = interface_state::address{};
      address.value = ip::make_address("192.0.2.10");
      address.prefix_length = 24;
      address.flags_known = true;
      interface.addresses.push_back(address);
      auto owner = std::make_shared<mdns_service::worker>(service._strand, interface, false, address.value, 8932);
      service._workers.emplace(std::pair{7U, false}, owner);
      owner->advertisement.head.flags = 0x8400;
      owner->advertisement.answers.push_back({service._service, mdns_codec::type_ptr, 1, 120,
                                              mdns_codec::ptr{{"instance", "local"}}});
      auto request = mdns_wire::query(service._service);
      request.head.id = 17;
      auto route = forge::net::transport::datagram_io::received{};
      route.remote = {ip::make_address("192.0.2.20"), 40000};
      route.local = mdns_wire::group(false, 7);
      route.interface_index = 7;
      const auto now = mdns_service::time_point{10s};
      service.respond(*owner, request, route, now);
      service.respond(*owner, request, route, now + 1ms);
      BOOST_REQUIRE_EQUAL(owner->outbound.size(), 2U); // multicast + one coalesced legacy
      route.remote = {ip::make_address("192.0.2.21"), 40001};
      service.respond(*owner, request, route, now + 2ms);
      BOOST_REQUIRE_EQUAL(owner->outbound.size(), 3U);
      BOOST_CHECK(owner->outbound[1].destination != owner->outbound[2].destination);
      BOOST_TEST(mdns_codec::decode(owner->outbound[1].bytes).head.id == 17U);
      BOOST_TEST(mdns_codec::decode(owner->outbound[2].bytes).head.id == 17U);
      request.head.id = 18;
      service.respond(*owner, request, route, now + 3ms);
      BOOST_REQUIRE_EQUAL(owner->outbound.size(), 4U); // same destination, new transaction
      if (flood) {
         // Simulate a drained queue: service-wide rate admission must survive it.
         service._queued -= owner->outbound.size();
         owner->outbound.clear();
         auto admitted = std::size_t{3};
         for (unsigned id = 100; id < 200; ++id) {
            request.head.id = static_cast<std::uint16_t>(id);
            service.respond(*owner, request, route, now + 4ms);
            admitted += owner->outbound.size();
            service._queued -= owner->outbound.size();
            owner->outbound.clear();
         }
         BOOST_TEST(admitted == 32U);
         // Another interface cannot multiply the global budget.
         interface.index = 8;
         auto other = std::make_shared<mdns_service::worker>(service._strand, interface, false, address.value, 8932);
         other->advertisement = owner->advertisement;
         other->response_at = now + 1s;
         service._workers.emplace(std::pair{8U, false}, other);
         service.respond(*other, request, route, now + 5ms);
         BOOST_TEST(other->outbound.empty());
         service.respond(*owner, request, route, now + 1s);
         BOOST_REQUIRE_EQUAL(owner->outbound.size(), 2U);
         BOOST_TEST(service._unicast_admitted == 1U);
         service.stop_worker(*other);
      } else {
         service._policy.max_pending_packets = service._queued;
         ++request.head.id;
         service.respond(*owner, request, route, now + 4ms);
         BOOST_TEST(owner->outbound.size() == 4U);
         BOOST_TEST(service._queued == 4U);
      }
      service.stop_worker(*owner);
      BOOST_TEST(service._queued == 0U);
   }
};
} // namespace forge::net::p2p::detail

namespace {
namespace p2p = forge::net::p2p;
namespace asio = boost::asio;
using service = p2p::detail::mdns_service;
using namespace std::chrono_literals;

template <typename T>
void finish(asio::io_context& io, const std::shared_ptr<service>& owner, std::future<T>& result) {
   io.restart();
   io.run_for(5s);
   const auto ready = result.wait_for(0ms) == std::future_status::ready;
   if (!ready) {
      owner->request_stop();
      io.restart();
      io.run_for(1s);
   }
   BOOST_REQUIRE_MESSAGE(ready, "mDNS service exceeded independent test deadline");
}

std::shared_ptr<service> make_service(asio::io_context& io, p2p::resource_manager& resources, unsigned& replacements) {
   auto policy = p2p::mdns_policy{};
   policy.enabled = true;
   return std::make_shared<service>(io.get_executor(), resources, policy,
       p2p::peer_id{"QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC"},
       p2p::detail::mdns_codec::name{"_forge-service-test", "_udp", "local"},
       service::callbacks{
           .listeners = [] { return std::vector<p2p::endpoint>{}; },
           .replace = [&](auto values) { BOOST_TEST(values.empty()); ++replacements; }});
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_mdns_service_stop_before_execution_releases_tracked_owner) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto tracker = p2p::detail::lifecycle_tracker{io.get_executor()};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   owner->start(tracker);
   tracker.request_stop();
   auto joined = asio::co_spawn(io, owner->async_join(), asio::use_future);
   finish(io, owner, joined);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(replacements == 1U);
   BOOST_TEST(resources.current().system.memory == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
   auto tracked = asio::co_spawn(io, tracker.wait(), asio::use_future);
   finish(io, owner, tracked);
   BOOST_CHECK_NO_THROW(tracked.get());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_admission_failure_precedes_native_socket_creation) {
   auto io = asio::io_context{};
   auto limits = p2p::resource_manager::limits{};
   limits.system.max_file_descriptors = 0;
   auto resources = p2p::resource_manager{limits};
   auto tracker = p2p::detail::lifecycle_tracker{io.get_executor()};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   owner->start(tracker);
   auto joined = asio::co_spawn(io, owner->async_join(), asio::use_future);
   finish(io, owner, joined);
   BOOST_CHECK_THROW(joined.get(), p2p::exceptions::backpressure_rejected);
   BOOST_TEST(resources.current().system.memory == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
   BOOST_TEST(resources.current().denied_file_descriptors == 1U);
   BOOST_TEST(replacements == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_direct_prestop_tracker_wait_joins_bridge) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto tracker = p2p::detail::lifecycle_tracker{io.get_executor()};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   owner->start(tracker);
   owner->request_stop();
   // Natural completion of the stopped work branch must join the bridge's
   // lifecycle waiter even though tracker.request_stop() was never called.
   auto joined = asio::co_spawn(io, owner->async_join(), asio::use_future);
   finish(io, owner, joined);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(replacements == 1U);
   tracker.request_stop();
   auto tracked = asio::co_spawn(io, tracker.wait(), asio::use_future);
   finish(io, owner, tracked);
   BOOST_CHECK_NO_THROW(tracked.get());
   BOOST_TEST(replacements == 1U);
   BOOST_TEST(resources.current().system.memory == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_native_start_stop_joins_all_workers) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto tracker = p2p::detail::lifecycle_tracker{io.get_executor()};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   owner->start(tracker);
   auto stop = [&]() -> asio::awaitable<void> {
      auto timer = asio::steady_timer{io};
      timer.expires_after(100ms);
      co_await timer.async_wait(asio::use_awaitable);
      owner->notify_addresses_changed();
      tracker.request_stop();
      co_await owner->async_join();
      co_await tracker.wait();
   };
   auto joined = asio::co_spawn(io, stop(), asio::use_future);
   finish(io, owner, joined);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(replacements >= 1U);
   BOOST_TEST(resources.current().system.memory == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
   owner->request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_retains_resource_ledger_after_caller_destruction) {
   auto io = asio::io_context{};
   auto tracker = p2p::detail::lifecycle_tracker{io.get_executor()};
   auto replacements = 0U;
   auto owner = std::shared_ptr<service>{};
   {
      auto limits = p2p::resource_manager::limits{};
      limits.system.max_file_descriptors = 0;
      auto resources = p2p::resource_manager{limits};
      owner = make_service(io, resources, replacements);
      owner->start(tracker);
   }
   // run() has not executed: admission must use the retained shared ledger,
   // never the destroyed caller's resource_manager object.
   auto joined = asio::co_spawn(io, owner->async_join(), asio::use_future);
   finish(io, owner, joined);
   BOOST_CHECK_THROW(joined.get(), p2p::exceptions::backpressure_rejected);
   BOOST_TEST(replacements == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_distinct_legacy_queriers_bypass_multicast_cooldown) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   p2p::detail::mdns_service_fixture::responses(*owner, false);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_unicast_flood_budget_survives_drain_and_interfaces) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   p2p::detail::mdns_service_fixture::responses(*owner, true);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_goodbye_zeroes_both_sections) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   p2p::detail::mdns_service_fixture::goodbye_sections(*owner);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_receive_boundary_rejects_bad_headers_and_expires_leases) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   p2p::detail::mdns_service_fixture::receive_boundary(*owner);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_received_expiry_wakes_pending_maintenance_timer) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   p2p::detail::mdns_service_fixture::expiry_timer(io, *owner);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_service_dispatches_unicast_by_pktinfo_not_receiving_socket) {
   auto io = asio::io_context{};
   auto resources = p2p::resource_manager{};
   auto replacements = 0U;
   auto owner = make_service(io, resources, replacements);
   p2p::detail::mdns_service_fixture::cross_interface_dispatch(*owner);
}
