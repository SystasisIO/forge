#include <boost/test/unit_test.hpp>

#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../../libraries/net/p2p/details/mdns_registry.hxx"

namespace {
namespace p2p = forge::net::p2p;
namespace codec = p2p::detail::mdns_codec;
using registry = p2p::detail::mdns_registry;
using namespace std::chrono_literals;

const codec::name service{"_p2p", "_udp", "local"};
const codec::name instance{"instance", "_p2p", "_udp", "local"};
const std::string remote = "QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC";
const auto epoch = registry::time_point{100s};

auto interfaces(std::uint64_t generation = 1) {
   return std::vector<p2p::detail::interface_state::interface>{
       {.index = 7, .generation = generation, .up = true, .running = true, .multicast = true},
       {.index = 8, .generation = 1, .up = true, .running = true, .multicast = true}};
}

registry make_registry(registry::limits bounds = {}, p2p::mdns_policy policy = {}) {
   auto result = registry{policy, bounds, service, {}};
   result.replace_interfaces(interfaces());
   return result;
}

codec::record pointer(std::uint32_t ttl = 120) {
   return {.owner = service, .type = codec::type_ptr, .ttl = ttl, .data = codec::ptr{instance}};
}

codec::record text(std::string address = "/ip6/fe80::1/tcp/4001", std::uint32_t ttl = 120, bool flush = false) {
   const auto value = "dnsaddr=" + address + "/p2p/" + remote;
   return {.owner = instance,
           .type = codec::type_txt,
           .class_code = static_cast<std::uint16_t>(flush ? 0x8001 : 1),
           .ttl = ttl,
           .data = codec::txt{{codec::bytes{value.begin(), value.end()}}}};
}

codec::message packet(std::vector<codec::record> records) {
   return {.head = {.flags = 0x8400}, .answers = std::move(records)};
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_correlates_ptr_txt_and_preserves_local_scope) {
   auto cache = make_registry();
   auto ptr = pointer(60);
   ptr.owner = {"_P2P", "_UDP", "LOCAL"};
   cache.apply(7, 1, packet({text()}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(8, 1, packet({ptr}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({ptr}), epoch);
   auto values = cache.snapshot(epoch);
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().peer.to_string() == remote);
   BOOST_TEST(values.front().address.to_string() == "/ip6zone/7/ip6/fe80::1/tcp/4001/p2p/" + remote);
   BOOST_TEST(values.front().interface_index == 7U);
   BOOST_TEST(values.front().generation == 1U);
   BOOST_CHECK(values.front().expires_at == epoch + 60s);
   cache.apply(7, 1, packet({pointer(120)}), epoch + 30s);
   BOOST_CHECK(cache.snapshot(epoch + 60s).front().expires_at == epoch + 120s);
   BOOST_TEST(cache.snapshot(epoch + 120s).empty());
   cache.expire(epoch + 150s);
   BOOST_TEST(cache.record_count() == 0U);
   BOOST_TEST(cache.byte_count() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_rust_single_label_correlates_and_expires) {
   // Pinned Rust dns.rs::generate_peer_name emits one 32..63-byte label.
   // query.rs::MdnsResponse requires the service PTR owner, then MdnsPeer
   // correlates its target with the additional TXT owner, without a suffix.
   for (const auto length : {32U, 63U}) {
      for (const auto ptr_expires_first : {false, true}) {
         auto cache = make_registry();
         const codec::name target{std::string(length, 'a')};
         auto ptr = pointer(ptr_expires_first ? 2 : 5);
         ptr.data = codec::ptr{target};
         auto txt = text("/ip4/10.231.77.2/tcp/4001", ptr_expires_first ? 5 : 2);
         txt.owner = target;
         auto response = packet({ptr});
         response.additionals = {txt};
         cache.apply(7, 1, response, epoch);
         const auto values = cache.snapshot(epoch);
         BOOST_REQUIRE_EQUAL(values.size(), 1U);
         BOOST_TEST(values.front().peer.to_string() == remote);
         BOOST_TEST(values.front().address.to_string() == "/ip4/10.231.77.2/tcp/4001/p2p/" + remote);
         BOOST_TEST(values.front().interface_index == 7U);
         BOOST_TEST(values.front().generation == 1U);
         BOOST_CHECK(values.front().expires_at == epoch + 2s);
         BOOST_TEST(cache.snapshot(epoch + 1999ms).size() == 1U);
         BOOST_TEST(cache.snapshot(epoch + 2s).empty());
         cache.expire(epoch + 5s);
         BOOST_TEST(cache.record_count() == 0U);
         BOOST_TEST(cache.byte_count() == 0U);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_rust_single_label_requires_service_and_exact_txt_target) {
   const codec::name private_service{"_p2p-45fc986bbc9388a11d939df26f730f0c", "_udp", "local"};
   const codec::name target{std::string(32, 'b')};
   for (const auto& expected_service : {service, private_service}) {
      auto cache = registry{p2p::mdns_policy{}, registry::limits{}, expected_service, {}};
      cache.replace_interfaces(interfaces());
      auto ptr = pointer();
      ptr.owner = expected_service == service ? private_service : service;
      ptr.data = codec::ptr{target};
      auto txt = text();
      txt.owner = target;
      cache.apply(7, 1, packet({ptr, txt}), epoch);
      BOOST_TEST(cache.snapshot(epoch).empty());

      // Only an exact service owner authorizes the correlated TXT record.
      ptr.owner = expected_service;
      cache.apply(7, 1, packet({ptr}), epoch);
      BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   }
   auto cache = make_registry();
   auto ptr = pointer();
   ptr.data = codec::ptr{target};
   auto txt = text();
   txt.owner = {std::string(32, 'c')};
   cache.apply(7, 1, packet({ptr, txt}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   txt.owner = target;
   cache.apply(7, 1, packet({txt}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_rust_single_label_requires_same_interface_generation) {
   auto cache = make_registry();
   const codec::name target{std::string(32, 'd')};
   auto ptr = pointer();
   ptr.data = codec::ptr{target};
   auto txt = text();
   txt.owner = target;
   cache.apply(7, 1, packet({ptr}), epoch);
   cache.apply(8, 1, packet({txt}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({txt}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);

   cache.replace_interfaces(interfaces(2));
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({ptr, txt}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 2, packet({ptr}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({txt}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 2, packet({txt}), epoch);
   const auto values = cache.snapshot(epoch);
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().interface_index == 7U);
   BOOST_TEST(values.front().generation == 2U);
   BOOST_TEST(values.front().address.to_string() == "/ip6zone/7/ip6/fe80::1/tcp/4001/p2p/" + remote);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_rejects_root_peer_target) {
   auto cache = make_registry();
   auto ptr = pointer();
   ptr.data = codec::ptr{{}};
   auto txt = text();
   txt.owner.clear();
   // The DNS root is codec-valid but is not a nonempty peer instance name.
   cache.apply(7, 1, codec::decode(codec::encode(packet({ptr, txt}))), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_goodbye_never_extends_or_allocates) {
   auto cache = make_registry();
   cache.apply(7, 1, packet({pointer(0), text("/ip6/fe80::1/tcp/4001", 0)}), epoch);
   BOOST_TEST(cache.record_count() == 0U);
   BOOST_TEST(cache.byte_count() == 0U);
   cache.apply(7, 1, packet({pointer(2), text()}), epoch);
   cache.apply(7, 1, packet({pointer(0)}), epoch + 1500ms);
   BOOST_CHECK(cache.snapshot(epoch + 1500ms).front().expires_at == epoch + 2s);
   cache.apply(7, 1, packet({pointer(120)}), epoch + 1600ms);
   cache.apply(7, 1, packet({pointer(0)}), epoch + 2s);
   BOOST_CHECK(cache.snapshot(epoch + 2s).front().expires_at == epoch + 3s);
   BOOST_TEST(cache.snapshot(epoch + 3s).empty());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_flush_is_packet_wide_and_protects_split_rrsets) {
   auto cache = make_registry();
   const auto a = text("/ip4/192.168.1.1/tcp/4001", 120, true);
   const auto b = text("/ip4/192.168.1.2/tcp/4001", 120, true);
   const auto c = text("/ip4/192.168.1.3/tcp/4001", 120, true);
   cache.apply(7, 1, packet({pointer(), a, b, c}), epoch);
   auto response = packet({a});
   response.additionals = {b};
   cache.apply(7, 1, response, epoch + 2s);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch + 2500ms).size(), 3U);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch + 3s).size(), 2U);
   // The second packet of a split RRset rescues c and cannot flush a/b
   // received less than a second ago.
   cache.apply(7, 1, packet({c}), epoch + 2500ms);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch + 4s).size(), 3U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_shared_ptr_ignores_cache_flush) {
   auto cache = make_registry();
   auto second_pointer = pointer();
   auto second_text = text("/ip4/192.168.1.2/tcp/4001");
   auto second_instance = instance;
   second_instance.front() = "second";
   second_pointer.data = codec::ptr{second_instance};
   second_text.owner = second_instance;
   auto peer_bytes = p2p::peer_id::from_string(remote).to_bytes();
   peer_bytes.back() ^= 1;
   const auto second_peer = p2p::peer_id::from_bytes(peer_bytes).to_string();
   const auto attribute = "dnsaddr=/ip4/192.168.1.2/tcp/4001/p2p/" + second_peer;
   second_text.data = codec::txt{{codec::bytes{attribute.begin(), attribute.end()}}};
   cache.apply(7, 1, packet({pointer(), text(), second_pointer, second_text}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 2U);

   auto injected = pointer();
   auto third_instance = instance;
   third_instance.front() = "third";
   injected.data = codec::ptr{third_instance};
   injected.class_code |= codec::class_high_bit;
   cache.apply(7, 1, packet({injected}), epoch + 2s);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch + 4s).size(), 2U);
   // An exact goodbye still withdraws its own shared record, not its peers.
   cache.apply(7, 1, packet({pointer(0)}), epoch + 4s);
   const auto remaining = cache.snapshot(epoch + 5s);
   BOOST_REQUIRE_EQUAL(remaining.size(), 1U);
   BOOST_TEST(remaining.front().peer.to_string() == second_peer);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_interfaces_withdraw_independently_and_reject_old_generations) {
   auto cache = make_registry();
   const auto response = packet({pointer(), text()});
   cache.apply(7, 1, response, epoch);
   cache.apply(8, 1, response, epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 2U);
   cache.withdraw_interface(7, 1);
   auto values = cache.snapshot(epoch);
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().interface_index == 8U);
   cache.replace_interfaces(interfaces(2));
   cache.apply(7, 1, response, epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   cache.apply(7, 2, response, epoch);
   cache.withdraw_interface(7, 1);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 2U);
   cache.replace_interfaces(interfaces(3));
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   cache.apply(7, 2, response, epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   cache.replace_interfaces({});
   BOOST_TEST(cache.record_count() == 0U);
   BOOST_TEST(cache.byte_count() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_duplicate_address_keeps_independent_interface_leases) {
   auto cache = make_registry();
   cache.apply(7, 1, packet({pointer(2), text("/ip4/192.168.1.1/tcp/4001", 2)}), epoch);
   cache.apply(8, 1, packet({pointer(60), text("/ip4/192.168.1.1/tcp/4001", 60)}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 2U);
   const auto values = cache.snapshot(epoch + 2s);
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().interface_index == 8U);
   cache.withdraw_interface(7, 1);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch + 2s).size(), 1U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_refresh_can_shorten_ttl_and_txt_is_case_sensitive) {
   auto cache = make_registry();
   auto txt = text();
   txt.owner = {"INSTANCE", "_P2P", "_UDP", "LOCAL"};
   std::get<codec::txt>(txt.data).attributes.front().front() = 'D';
   cache.apply(7, 1, packet({pointer(), txt}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({text()}), epoch);
   cache.apply(7, 1, packet({text("/ip6/fe80::1/tcp/4001", 2)}), epoch + 1s);
   BOOST_CHECK(cache.snapshot(epoch + 2s).front().expires_at == epoch + 3s);
   BOOST_TEST(cache.snapshot(epoch + 3s).empty());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_bounds_records_and_bytes_without_usable_peers) {
   auto limited = make_registry({.records = 2, .bytes = 4096});
   for (auto i = 0; i < 10; ++i) {
      auto r = text();
      r.owner = {std::to_string(i), "local"};
      r.data = codec::txt{{codec::bytes(20, 'x')}};
      limited.apply(7, 1, packet({r}), epoch);
   }
   BOOST_TEST(limited.record_count() == 2U);
   BOOST_TEST(limited.byte_count() <= 4096U);
   BOOST_TEST(limited.snapshot(epoch).empty());
   auto tiny = make_registry({.records = 100, .bytes = 1});
   tiny.apply(7, 1, packet({pointer(), text()}), epoch);
   BOOST_TEST(tiny.record_count() == 0U);
   BOOST_TEST(tiny.byte_count() == 0U);
   auto oversized = text();
   oversized.data = codec::txt{{codec::bytes(4097, 'x')}};
   limited.expire(epoch + 120s);
   limited.apply(7, 1, packet({oversized}), epoch + 120s);
   BOOST_TEST(limited.record_count() == 0U);
   auto probe = make_registry();
   probe.apply(7, 1, packet({pointer()}), epoch);
   const auto exact_bytes = probe.byte_count();
   auto exact = make_registry({.records = 100, .bytes = exact_bytes});
   exact.apply(7, 1, packet({pointer(), text()}), epoch);
   BOOST_TEST(exact.record_count() == 1U);
   BOOST_TEST(exact.byte_count() == exact_bytes);
   auto short_by_one = make_registry({.records = 100, .bytes = exact_bytes - 1});
   short_by_one.apply(7, 1, packet({pointer()}), epoch);
   BOOST_TEST(short_by_one.record_count() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_filters_untrusted_endpoints_and_limits_derived_addresses) {
   auto policy = p2p::mdns_policy{};
   policy.max_peers = 1;
   policy.max_addresses_per_peer = 1;
   auto cache = make_registry({}, policy);
   auto response = packet({pointer()});
   for (const auto address :
        {"/ip6zone/remote0/ip6/2001:4860::1/tcp/4001", "/dns4/example.test/tcp/4001", "/ip4/0.0.0.0/tcp/4001",
         "/ip4/127.0.0.1/tcp/4001", "/ip4/224.0.0.1/tcp/4001", "/ip6/::/tcp/4001", "/ip6/::1/tcp/4001",
         "/ip6/::ffff:127.0.0.1/tcp/4001", "/ip6/::ffff:0.0.0.0/tcp/4001", "/ip6/ff02::1/tcp/4001",
         "/ip4/192.168.1.1/tcp/4001/ws", "/ip4/192.168.1.1/tcp/4001/p2p-circuit"}) {
      response.additionals.push_back(text(address));
   }
   cache.apply(7, 1, response, epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({text("/ip4/192.168.1.2/tcp/4001"), text("/ip4/192.168.1.3/tcp/4001")}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   auto second_peer_bytes = p2p::peer_id::from_string(remote).to_bytes();
   second_peer_bytes.back() ^= 1;
   const auto second_peer = p2p::peer_id::from_bytes(second_peer_bytes).to_string();
   auto other = text("/ip4/192.168.1.4/tcp/4001");
   const auto other_text = "dnsaddr=/ip4/192.168.1.4/tcp/4001/p2p/" + second_peer;
   other.data = codec::txt{{codec::bytes{other_text.begin(), other_text.end()}}};
   cache.apply(7, 1, packet({other}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   BOOST_TEST(cache.snapshot(epoch).front().peer.to_string() == remote);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_ttl_saturates_and_queries_do_not_learn) {
   auto cache = make_registry();
   auto response = packet({pointer((std::numeric_limits<std::uint32_t>::max)()),
                           text("/ip4/192.168.1.1/tcp/4001", (std::numeric_limits<std::uint32_t>::max)())});
   response.head.flags = 0;
   cache.apply(7, 1, response, epoch);
   BOOST_TEST(cache.record_count() == 0U);
   response.head.flags = 0x8400;
   cache.apply(7, 1, response, registry::time_point::max() - 500ms);
   const auto values = cache.snapshot(registry::time_point::max() - 1ms);
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_CHECK(values.front().expires_at == registry::time_point::max());
   BOOST_CHECK(cache.next_expiry() == registry::time_point::max());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_ptr_accepts_valid_targets_without_service_hierarchy) {
   for (const codec::name& target :
        {codec::name{"instance", "_other", "_udp", "local"}, codec::name{"extra", "instance", "_p2p", "_udp", "local"},
         service, instance}) {
      auto cache = make_registry();
      auto ptr = pointer();
      ptr.data = codec::ptr{target};
      auto txt = text();
      txt.owner = target;
      cache.apply(7, 1, packet({ptr, txt}), epoch);
      const auto values = cache.snapshot(epoch);
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_TEST(values.front().peer.to_string() == remote);
      BOOST_TEST(values.front().address.to_string() == "/ip6zone/7/ip6/fe80::1/tcp/4001/p2p/" + remote);
   }
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_ptr_case_folding_deduplicates_targets) {
   auto cache = make_registry();
   auto response = packet({pointer()});
   for (auto i = 0; i < 60; ++i) {
      auto ptr = pointer(60 + i);
      ptr.owner = {"_P2P", "_UDP", "LOCAL"};
      ptr.data = codec::ptr{{"INSTANCE", "_P2P", "_UDP", "LOCAL"}};
      response.answers.push_back(ptr);
   }
   response.additionals = {text()};
   cache.apply(7, 1, response, epoch);
   BOOST_TEST(cache.record_count() == 2U);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_rejects_nul_and_oversized_txt_attributes) {
   auto cache = make_registry();
   auto nul = text();
   auto& attr = std::get<codec::txt>(nul.data).attributes.front();
   attr.insert(attr.begin() + 15, 0);
   auto oversized = text();
   std::get<codec::txt>(oversized.data).attributes.front().resize(256, 'x');
   cache.apply(7, 1, packet({pointer(), nul, oversized}), epoch);
   BOOST_TEST(cache.snapshot(epoch).empty());
   cache.apply(7, 1, packet({text()}), epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_registry_bounds_total_leases_and_copied_endpoint_bytes) {
   auto cache = make_registry({.leases = 1});
   const auto response = packet({pointer(), text("/ip4/192.168.1.1/tcp/4001")});
   cache.apply(7, 1, response, epoch);
   cache.apply(8, 1, response, epoch);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   BOOST_TEST(cache.snapshot(epoch).front().interface_index == 7U);
   cache.withdraw_interface(7, 1);
   BOOST_REQUIRE_EQUAL(cache.snapshot(epoch).size(), 1U);
   BOOST_TEST(cache.snapshot(epoch).front().interface_index == 8U);

   auto no_payload_space = make_registry({.lease_bytes = sizeof(registry::lease)});
   no_payload_space.apply(7, 1, response, epoch);
   BOOST_TEST(no_payload_space.record_count() == 2U);
   BOOST_TEST(no_payload_space.snapshot(epoch).empty());
   const auto one_lease_bytes = sizeof(registry::lease) + remote.size() * 2 + std::string{"192.168.1.1"}.size() + 4;
   auto exact = make_registry({.lease_bytes = one_lease_bytes});
   exact.apply(7, 1, response, epoch);
   exact.apply(8, 1, response, epoch);
   BOOST_REQUIRE_EQUAL(exact.snapshot(epoch).size(), 1U);
   auto short_by_one = make_registry({.lease_bytes = one_lease_bytes - 1});
   short_by_one.apply(7, 1, response, epoch);
   BOOST_TEST(short_by_one.snapshot(epoch).empty());
}
