#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <string>
#include <vector>
#include "../../libraries/net/p2p/details/mdns_wire.hxx"

namespace {
namespace p2p = forge::net::p2p;
namespace wire = p2p::detail::mdns_wire;
namespace codec = p2p::detail::mdns_codec;
namespace ip = boost::asio::ip;
const auto service = codec::name{"_p2p", "_udp", "local"};
const auto label = std::string(32, 'a');
const auto local = p2p::peer_id{"QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC"};

p2p::detail::interface_state::interface interface() {
   return {.index = 7, .generation = 3, .name = "test7", .up = true, .running = true, .multicast = true,
           .addresses = {{.value = ip::make_address("192.0.2.10"), .prefix_length = 24, .flags_known = true},
                         {.value = ip::make_address("fe80::10"), .prefix_length = 64, .scope_id = 7,
                          .flags_known = true}}};
}

codec::message advertisement() {
   const auto listeners = std::vector{p2p::parse_endpoint("/ip4/0.0.0.0/udp/4001/quic-v1"),
                                      p2p::parse_endpoint("/ip6zone/7/ip6/fe80::10/tcp/4001")};
   return wire::advertisement({}, service, label, local, interface(), listeners);
}

codec::message canonical_advertisement() {
   const codec::name owner{label, "_p2p", "_udp", "local"};
   const codec::name host{label, "local"};
   return {.head = {.flags = 0x8400},
       .answers = {{service, codec::type_ptr, 1, 120, codec::ptr{owner}}},
       .additionals = {
           {owner, codec::type_txt, 0x8001, 120, codec::txt{{codec::bytes{'x'}}}},
           {owner, codec::type_srv, 0x8001, 120, codec::srv{0, 0, 4001, host}},
           {host, codec::type_a, 0x8001, 120, codec::a{ip::make_address_v4("192.0.2.10").to_bytes()}},
           {host, codec::type_aaaa, 0x8001, 120, codec::aaaa{ip::make_address_v6("fe80::10").to_bytes()}}}};
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_rust_reader_requires_additional_txt) {
   const auto packet = codec::decode(codec::encode(advertisement()));
   BOOST_REQUIRE_EQUAL(packet.answers.size(), 1U);
   BOOST_REQUIRE_EQUAL(packet.additionals.size(), 4U);
   BOOST_REQUIRE(packet.answers.front().type == codec::type_ptr);
   BOOST_CHECK(packet.answers.front().owner == service);
   const auto& target = std::get<codec::ptr>(packet.answers.front().data).target;
   const auto txt = std::ranges::find_if(packet.additionals, [&](const auto& record) {
      return record.type == codec::type_txt && record.owner == target;
   });
   BOOST_REQUIRE(txt != packet.additionals.end());
   BOOST_TEST(std::get<codec::txt>(txt->data).attributes.size() == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_canonical_detail_dependencies) {
   const auto advertised = canonical_advertisement();
   for (const auto& record : advertised.additionals) {
      const auto request = codec::message{.questions = {{record.owner, record.type, 1}}};
      const auto response = wire::answer(request, advertised, false);
      BOOST_REQUIRE(response.has_value());
      BOOST_REQUIRE_EQUAL(response->answers.size(), 1U);
      BOOST_CHECK(response->answers.front() == record);
      if (record.type == codec::type_srv) {
         BOOST_REQUIRE_EQUAL(response->additionals.size(), 2U);
         BOOST_CHECK(response->additionals[0] == advertised.additionals[2]);
         BOOST_CHECK(response->additionals[1] == advertised.additionals[3]);
      } else {
         BOOST_TEST(response->additionals.empty());
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_canonical_suppression_without_orphans) {
   const auto advertised = canonical_advertisement();
   auto request = wire::query(service);
   auto response = wire::answer(request, advertised, false);
   BOOST_REQUIRE(response.has_value());
   BOOST_CHECK(response->answers == advertised.answers);
   BOOST_CHECK(response->additionals == advertised.additionals);

   // Known answers live in the query answer section, even when suppressing
   // records that would otherwise be emitted as additional dependencies.
   request.answers = {advertised.additionals[0]};
   request.answers.front().ttl = 60;
   response = wire::answer(request, advertised, false);
   BOOST_REQUIRE(response.has_value());
   BOOST_TEST(response->answers.size() == 1U);
   BOOST_REQUIRE_EQUAL(response->additionals.size(), 3U);
   BOOST_TEST(std::ranges::none_of(response->additionals, [](const auto& r) { return r.type == codec::type_txt; }));
   request.answers.front().ttl = 59;
   response = wire::answer(request, advertised, false);
   BOOST_REQUIRE(response.has_value());
   BOOST_TEST(response->additionals.size() == 4U);

   request.answers = advertised.answers;
   BOOST_TEST(!wire::answer(request, advertised, false).has_value()); // No orphan dependencies.
   request.questions.push_back({advertised.additionals[0].owner, codec::type_txt, 1});
   response = wire::answer(request, advertised, false);
   BOOST_REQUIRE(response.has_value());
   BOOST_REQUIRE_EQUAL(response->answers.size(), 1U);
   BOOST_CHECK(response->answers.front() == advertised.additionals[0]);
   BOOST_TEST(response->additionals.empty());

}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_canonical_legacy_both_sections) {
   const auto advertised = canonical_advertisement();
   auto request = wire::query(service);
   request.head.id = 123;
   const auto legacy = wire::answer(request, advertised, true);
   BOOST_REQUIRE(legacy.has_value());
   BOOST_TEST(legacy->head.id == 123U);
   BOOST_REQUIRE_EQUAL(legacy->questions.size(), 1U);
   BOOST_REQUIRE_EQUAL(legacy->answers.size(), 1U);
   BOOST_REQUIRE_EQUAL(legacy->additionals.size(), 4U);
   for (const auto* records : {&legacy->answers, &legacy->additionals}) {
      for (const auto& record : *records) {
         BOOST_TEST(record.ttl == 10U);
         BOOST_TEST(record.class_code == 1U);
      }
   }
   request.answers = {advertised.additionals.front()};
   request.answers.front().ttl = 60;
   const auto suppressed = wire::answer(request, advertised, true);
   BOOST_REQUIRE(suppressed.has_value());
   BOOST_TEST(suppressed->additionals.size() == 3U);
   request.answers = advertised.answers;
   BOOST_TEST(!wire::answer(request, advertised, true).has_value());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_known_srv_preserves_ptr_address_dependencies) {
   const auto advertised = canonical_advertisement();
   for (const auto knows_a : {false, true}) {
      auto request = wire::query(service);
      request.answers = {advertised.additionals[1]};
      request.answers.front().ttl = 60;
      if (knows_a) {
         request.answers.push_back(advertised.additionals[2]);
         request.answers.back().ttl = 60;
      }
      const auto response = wire::answer(request, advertised, false);
      BOOST_REQUIRE(response.has_value());
      BOOST_CHECK(response->answers == advertised.answers);
      // SRV suppression must not erase the PTR's dependency on its target
      // addresses. Each known RR suppresses only that individual record.
      auto expected = std::vector<codec::record>{advertised.additionals[0]};
      if (!knows_a) { expected.push_back(advertised.additionals[2]); }
      expected.push_back(advertised.additionals[3]);
      BOOST_CHECK(response->additionals == expected);
   }
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_canonical_multi_question_promotes_dependencies_once) {
   const auto advertised = canonical_advertisement();
   auto request = wire::query(service);
   request.questions.push_back({advertised.additionals[0].owner, 255, 1});
   request.questions.push_back(request.questions.back());
   request.questions.push_back({advertised.additionals[2].owner, codec::type_a, 1});
   const auto response = wire::answer(request, advertised, false);
   BOOST_REQUIRE(response.has_value());
   BOOST_REQUIRE_EQUAL(response->answers.size(), 4U);
   BOOST_CHECK(response->answers[0] == advertised.answers[0]);
   BOOST_CHECK(response->answers[1] == advertised.additionals[0]);
   BOOST_CHECK(response->answers[2] == advertised.additionals[1]);
   BOOST_CHECK(response->answers[3] == advertised.additionals[2]);
   BOOST_REQUIRE_EQUAL(response->additionals.size(), 1U);
   BOOST_CHECK(response->additionals[0] == advertised.additionals[3]);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_advertisement_expands_wildcard_and_strips_local_zone) {
   const auto packet = advertisement();
   BOOST_REQUIRE_EQUAL(packet.answers.size(), 1U);
   BOOST_REQUIRE_EQUAL(packet.additionals.size(), 4U);
   BOOST_TEST(packet.head.flags == 0x8400U);
   BOOST_TEST(packet.answers[0].type == codec::type_ptr);
   BOOST_TEST(packet.answers[0].class_code == 1U);
   const auto& attributes = std::get<codec::txt>(packet.additionals[0].data).attributes;
   BOOST_REQUIRE_EQUAL(attributes.size(), 2U);
   const auto first = std::string(attributes[0].begin(), attributes[0].end());
   const auto second = std::string(attributes[1].begin(), attributes[1].end());
   BOOST_TEST(first == "dnsaddr=/ip4/192.0.2.10/udp/4001/quic-v1/p2p/" + local.value);
   BOOST_TEST(second == "dnsaddr=/ip6/fe80::10/tcp/4001/p2p/" + local.value);
   for (const auto& record : packet.additionals) {
      BOOST_TEST(record.class_code == 0x8001U);
   }
   BOOST_CHECK(codec::decode(codec::encode(packet)) == packet);
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_filters_unusable_and_foreign_interface_addresses) {
   auto iface = interface();
   iface.addresses[0].tentative = true;
   iface.addresses[1].deprecated = true;
   const auto listeners = std::vector{p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4001"),
                                      p2p::parse_endpoint("/ip6/::/tcp/4001")};
   BOOST_TEST(wire::advertisement({}, service, label, local, iface, listeners).answers.empty());
   iface = interface();
   const auto foreign = std::vector{p2p::parse_endpoint("/ip4/198.51.100.1/tcp/4001"),
                                    p2p::parse_endpoint("/ip6zone/8/ip6/fe80::10/tcp/4001")};
   BOOST_TEST(wire::advertisement({}, service, label, local, iface, foreign).answers.empty());
   iface.loopback = true;
   BOOST_TEST(!wire::usable(iface));
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_packet_info_and_unicast_prefix_boundary) {
   const auto iface = interface();
   auto route = forge::net::transport::datagram_io::received{
       .remote = {ip::make_address("192.0.2.20"), 49152},
       .local = wire::group(false, 7), .interface_index = 7};
   BOOST_TEST(wire::admit(iface, false, route)); // Rust ephemeral answer/query port.
   route.interface_index = 8;
   BOOST_TEST(!wire::admit(iface, false, route));
   route.interface_index = 7;
   route.local = {ip::make_address("192.0.2.10"), 5353};
   BOOST_TEST(wire::admit(iface, false, route));
   route.remote = {ip::make_address("192.0.3.20"), 49152};
   BOOST_TEST(!wire::admit(iface, false, route));
   route.local = wire::group(false, 7);
   BOOST_TEST(wire::admit(iface, false, route));
   route.local = {ip::make_address("224.0.0.252"), 5353};
   BOOST_TEST(!wire::admit(iface, false, route));
   route = {.remote = {ip::make_address("fe80::20%7"), 49152},
            .local = wire::group(true, 7), .interface_index = 7};
   BOOST_TEST(wire::admit(iface, true, route));
   route.remote = {ip::make_address("fe80::20%8"), 49152};
   BOOST_TEST(!wire::admit(iface, true, route));
   auto unknown_prefix = iface;
   unknown_prefix.addresses[0].prefix_length.reset();
   BOOST_TEST(!wire::on_link(unknown_prefix, ip::make_address("192.0.2.20")));
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_answers_queries_not_responses_and_suppresses_known_answers) {
   const auto advertised = advertisement();
   auto request = wire::query(service);
   BOOST_REQUIRE(wire::answer(request, advertised, false).has_value());
   request.answers = advertised.answers;
   for (auto& record : request.answers) { record.ttl = 60; }
   BOOST_TEST(!wire::answer(request, advertised, false).has_value());
   for (auto& record : request.answers) { record.ttl = 59; }
   BOOST_REQUIRE(wire::answer(request, advertised, false).has_value());
   request.head.flags = 0x8400;
   BOOST_TEST(!wire::answer(request, advertised, false).has_value());
   request = wire::query({"_p2p-other", "_udp", "local"});
   BOOST_TEST(!wire::answer(request, advertised, false).has_value());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_legacy_qu_detail_and_meta_queries) {
   const auto advertised = advertisement();
   auto request = wire::query(service);
   request.head.id = 0x1234;
   request.questions[0].class_code = 0x8001;
   const auto multicast = wire::answer(request, advertised, false);
   const auto legacy = wire::answer(request, advertised, true);
   BOOST_REQUIRE(multicast.has_value());
   BOOST_REQUIRE(legacy.has_value());
   BOOST_TEST(multicast->head.id == 0U);
   BOOST_TEST(legacy->head.id == 0x1234U);
   BOOST_REQUIRE_EQUAL(legacy->questions.size(), 1U);
   BOOST_TEST(legacy->questions[0].class_code == 1U);
   for (const auto* records : {&legacy->answers, &legacy->additionals}) {
      for (const auto& record : *records) {
         BOOST_TEST(record.ttl == 10U);
         BOOST_TEST(record.class_code == 1U);
      }
   }
   for (const auto* records : {&advertised.answers, &advertised.additionals}) {
      for (const auto& record : *records) {
         request = {.questions = {{record.owner, record.type, 1}}};
         BOOST_TEST(wire::answer(request, advertised, false).has_value());
      }
   }
   request = {.questions = {{{"_services", "_dns-sd", "_udp", "local"}, codec::type_ptr, 1}}};
   const auto meta = wire::answer(request, advertised, false);
   BOOST_REQUIRE(meta.has_value());
   BOOST_REQUIRE_EQUAL(meta->answers.size(), 1U);
   BOOST_TEST(meta->answers[0].class_code == 1U);
   BOOST_TEST(std::get<codec::ptr>(meta->answers[0].data).target == service, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_mdns_wire_accepts_any_class_with_and_without_qu) {
   auto advertised = codec::message{};
   advertised.answers.push_back({service, codec::type_ptr, 1, 120, codec::ptr{{"instance", "local"}}});
   auto request = wire::query(service);
   for (const auto qclass : {0x00ffU, 0x80ffU}) {
      request.questions[0].class_code = static_cast<std::uint16_t>(qclass);
      const auto response = wire::answer(request, advertised, false);
      BOOST_REQUIRE(response.has_value());
      BOOST_REQUIRE_EQUAL(response->answers.size(), 1U);
      BOOST_TEST(response->answers[0].class_code == 1U);
   }
   request.questions[0].class_code = 3;
   BOOST_TEST(!wire::answer(request, advertised, false).has_value());
}
