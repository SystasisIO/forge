module;

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.multiformats.multiaddr;

#include "../../libraries/net/p2p/details/host_addresses.hxx"
#include "../../libraries/net/p2p/details/observed_address_manager.hxx"

namespace {

namespace p2p = forge::net::p2p;
using manager = p2p::detail::observed_address_manager;
using time_point = std::chrono::steady_clock::time_point;
using namespace std::chrono_literals;
constexpr auto start = time_point{} + 1h;

p2p::peer_id peer(std::uint64_t value) {
   auto bytes = std::vector<std::uint8_t>(32, 0);
   for (auto index = std::size_t{}; index < sizeof(value); ++index) {
      bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
   }
   return p2p::make_peer_id({.type = p2p::public_key::type::ed25519, .data = std::move(bytes)});
}

p2p::endpoint address(std::string_view value) {
   return p2p::parse_endpoint(value);
}

bool observe4(manager& value, std::uint64_t session, std::uint64_t identity, std::string_view observer,
              const p2p::endpoint& reported, time_point now = start) {
   const auto local = address("/ip4/10.0.0.2/tcp/4001");
   const auto listened = std::array{address("/ip4/0.0.0.0/tcp/4001")};
   const auto remote = address("/ip4/" + std::string{observer} + "/tcp/5001");
   return value.observe(session, peer(identity), local, remote, reported, listened, now);
}

void quorum4(manager& value, const p2p::endpoint& reported, std::uint64_t first = 1,
             time_point now = start) {
   for (auto index = std::uint64_t{}; index < 4; ++index) {
      BOOST_REQUIRE(observe4(value, first + index, first + index, "11.0.0." + std::to_string(index + 1), reported, now));
   }
}

std::vector<std::string> strings(const std::vector<p2p::endpoint>& values) {
   auto result = std::vector<std::string>{};
   for (const auto& value : values) {
      result.push_back(value.to_string());
   }
   return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(net_p2p_observed_address_tests)

BOOST_AUTO_TEST_CASE(observed_address_observer_group_is_numeric_and_shared_with_reachability) {
   const auto ipv4 = p2p::host_addresses::observer_group(address("/ip4/11.0.0.1/tcp/5001"));
   const auto mapped = p2p::host_addresses::observer_group(address("/ip6/::ffff:11.0.0.1/tcp/6001"));
   BOOST_REQUIRE(ipv4);
   BOOST_TEST(*ipv4 == "11.0.0.1");
   BOOST_CHECK(ipv4 == mapped);
   const auto first = p2p::host_addresses::observer_group(address("/ip6/2001:4860:abcd:1200::1/tcp/5001"));
   const auto same = p2p::host_addresses::observer_group(address("/ip6/2001:4860:abcd:12ff::2/tcp/6001"));
   const auto other = p2p::host_addresses::observer_group(address("/ip6/2001:4860:abcd:1300::1/tcp/5001"));
   BOOST_REQUIRE(first);
   BOOST_CHECK(first == same);
   BOOST_CHECK(first != other);
   BOOST_TEST(!p2p::host_addresses::observer_group(address("/dns4/observer.test/tcp/5001")).has_value());
   auto invalid = address("/ip4/11.0.0.1/tcp/5001");
   invalid.transport.host = "invalid";
   BOOST_TEST(!p2p::host_addresses::observer_group(invalid).has_value());
}

BOOST_AUTO_TEST_CASE(host_addresses_rejects_interface_zones_before_dnsaddr_fallback_and_egress) {
   const auto local = peer(90);
   const auto scoped = address("/ip6zone/receiver0/ip6/2001:4860::1/tcp/4001");
   const auto public_address = address("/ip6/2001:4860::2/tcp/4001");
   const auto context = p2p::host_addresses::learning_context{
       .source = p2p::host_addresses::source_kind::authenticated,
   };

   BOOST_TEST(!p2p::host_addresses::learned(scoped, local, context).has_value());

   auto mixed = forge::multiformats::multiaddr{};
   mixed.push({.code = forge::multiformats::protocol_code::ip6zone, .value = "receiver0"});
   mixed.push({.code = forge::multiformats::protocol_code::dnsaddr, .value = "bootstrap.example"});
   BOOST_TEST(!p2p::host_addresses::learned(mixed, local, context).has_value());

   const auto advertised = p2p::host_addresses::merge_advertised({scoped, public_address}, {scoped}, local);
   BOOST_REQUIRE_EQUAL(advertised.size(), 1U);
   BOOST_TEST(advertised.front().to_string() == public_address.to_string() + "/p2p/" + local.to_string());
}

BOOST_AUTO_TEST_CASE(observed_address_requires_listener_scope_match) {
   const auto local = address("/ip6zone/1/ip6/fd00::2/tcp/4001");
   const auto remote = address("/ip6zone/1/ip6/2001:4860::1/tcp/5001");
   const auto reported = address("/ip6/2606:4700::1111/tcp/8000");
   const auto listener = address("/ip6zone/1/ip6/fd00::2/tcp/4001");
   const auto other_interface = address("/ip6zone/2/ip6/fd00::2/tcp/4001");

   auto matched = manager{};
   BOOST_REQUIRE(matched.observe(1, peer(1), local, remote, reported, std::array{listener}, start));

   auto mismatched = manager{};
   BOOST_TEST(!mismatched.observe(1, peer(1), local, remote, reported, std::array{other_interface}, start));
}

BOOST_AUTO_TEST_CASE(observed_address_requires_distinct_authenticated_peers_and_ipv4_addresses) {
   const auto reported = address("/ip4/8.8.8.8/tcp/8000");
   auto value = manager{};
   // Four peers and four IPs alone are insufficient: all but one peer
   // share a single IP, so there are only two independent endorsements.
   for (auto index = std::uint64_t{1}; index <= 4; ++index) {
      BOOST_REQUIRE(observe4(value, index, 1, "11.0.0." + std::to_string(index), reported));
   }
   for (auto index = std::uint64_t{2}; index <= 4; ++index) {
      BOOST_REQUIRE(observe4(value, 10 + index, index, "11.0.0.1", reported));
   }
   BOOST_TEST(value.confirmed(start).empty());
   BOOST_REQUIRE(observe4(value, 20, 2, "11.0.0.2", reported));
   BOOST_TEST(value.confirmed(start).empty());
   BOOST_REQUIRE(observe4(value, 21, 3, "11.0.0.3", reported));
   BOOST_REQUIRE_EQUAL(value.confirmed(start).size(), 1U);
   BOOST_TEST(value.confirmed(start).front().to_string() == reported.to_string());
   value.remove(21);
   BOOST_TEST(value.confirmed(start).empty());
}

BOOST_AUTO_TEST_CASE(observed_address_ipv6_observers_share_the_first_56_bits) {
   auto value = manager{};
   const auto local = address("/ip6/fd00::2/tcp/4001");
   const auto listened = std::array{address("/ip6/::/tcp/4001")};
   auto reported = address("/ip6/2606:4700:4700:0:0:0:0:1111/tcp/8000");
   reported.peer = peer(100);
   for (auto index = std::uint64_t{1}; index <= 4; ++index) {
      const auto remote = address("/ip6/2001:4860:abcd:12ff::" + std::to_string(index) + "/tcp/5001");
      BOOST_REQUIRE(value.observe(index, peer(index), local, remote, reported, listened, start));
   }
   BOOST_TEST(value.confirmed(start).empty());
   auto session = std::uint64_t{5};
   for (const auto* prefix : {"1300", "1400", "1500"}) {
      const auto remote = address("/ip6/2001:4860:abcd:" + std::string{prefix} + "::1/tcp/5001");
      BOOST_REQUIRE(value.observe(session, peer(session), local, remote, reported, listened, start));
      ++session;
   }
   const auto confirmed = value.confirmed(start);
   BOOST_REQUIRE_EQUAL(confirmed.size(), 1U);
   BOOST_TEST(confirmed.front().to_string() == "/ip6/2606:4700:4700::1111/tcp/8000");
   BOOST_TEST(!confirmed.front().peer.has_value());
   value.remove(5);
   BOOST_TEST(value.confirmed(start).empty());
}

BOOST_AUTO_TEST_CASE(observed_address_refresh_expiry_removal_and_replacement_retire_votes) {
   auto value = manager{};
   const auto first = address("/ip4/8.8.8.8/tcp/8000");
   const auto second = address("/ip4/8.8.4.4/tcp/9000");
   quorum4(value, first);
   BOOST_CHECK(!observe4(value, 1, 1, "11.0.0.1", address("/ip4/127.0.0.1/tcp/8000")));
   BOOST_REQUIRE_EQUAL(value.confirmed(start).size(), 1U);
   BOOST_REQUIRE(observe4(value, 1, 1, "11.0.0.1", second));
   BOOST_TEST(value.confirmed(start).empty());
   for (auto session = std::uint64_t{2}; session <= 4; ++session) {
      BOOST_REQUIRE(observe4(value, session, session, "11.0.0." + std::to_string(session), second));
   }
   BOOST_REQUIRE_EQUAL(value.confirmed(start).size(), 1U);
   BOOST_TEST(value.confirmed(start).front().to_string() == second.to_string());
   BOOST_REQUIRE(observe4(value, 1, 1, "11.0.0.1", second, start + 1min));
   BOOST_REQUIRE_EQUAL(value.confirmed(start + 10min - 1ns).size(), 1U);
   BOOST_TEST(value.confirmed(start + 10min).empty());
   value.expire(start + 10min);
   quorum4(value, second, 10, start + 10min);
   value.remove(10);
   BOOST_REQUIRE_EQUAL(value.confirmed(start + 10min).size(), 1U);
   value.remove(1);
   value.remove(1);
   BOOST_TEST(value.confirmed(start + 10min).empty());
   value.expire(start + 20min);
   BOOST_TEST(value.confirmed(start + 20min).empty());
}

BOOST_AUTO_TEST_CASE(observed_address_rejects_nonroutable_dns_nat64_and_circuit_reports) {
   auto value = manager{};
   const auto local4 = address("/ip4/10.0.0.2/tcp/4001");
   const auto local6 = address("/ip6/fd00::2/tcp/4001");
   const auto listened = std::array{local4, local6};
   for (const auto* text : {"/ip4/127.0.0.1/tcp/8000", "/ip4/0.0.0.0/tcp/8000",
                           "/ip4/169.254.1.2/tcp/8000", "/ip4/192.0.2.1/tcp/8000",
                           "/ip4/224.0.0.1/tcp/8000", "/dns4/public.test/tcp/8000",
                           "/ip6/::1/tcp/8000", "/ip6/fe80::1/tcp/8000",
                           "/ip6/64:ff9b::808:808/tcp/8000", "/ip6/64:ff9b:1::808:808/tcp/8000",
                           "/ip6/::ffff:8.8.8.8/tcp/8000", "/ip6/2001:db8::1/tcp/8000"}) {
      BOOST_TEST_CONTEXT(text) {
         const auto reported = address(text);
         const auto ipv6 = reported.transport.host_type == p2p::endpoint::host_kind::ip6;
         const auto remote = address(ipv6 ? "/ip6/2001:4860::1/tcp/5001" : "/ip4/11.0.0.1/tcp/5001");
         BOOST_TEST(!value.observe(1, peer(1), ipv6 ? local6 : local4, remote, reported, listened, start));
      }
   }
   auto relayed = address("/ip4/8.8.8.8/tcp/8000");
   relayed.relayed = p2p::endpoint::circuit{.target = peer(2)};
   BOOST_TEST(!observe4(value, 1, 1, "11.0.0.1", relayed));
   BOOST_TEST(value.confirmed(start).empty());
}

BOOST_AUTO_TEST_CASE(observed_address_requires_matching_local_listener_and_transport) {
   auto value = manager{};
   const auto local = address("/ip4/10.0.0.2/tcp/4001");
   const auto remote = address("/ip4/11.0.0.1/tcp/5001");
   const auto reported = address("/ip4/8.8.8.8/tcp/8000");
   for (const auto* text : {"/ip4/10.0.0.2/tcp/4002", "/ip4/10.0.0.3/tcp/4001",
                           "/ip4/0.0.0.0/udp/4001/quic-v1", "/ip6/::/tcp/4001",
                           "/dns4/local.test/tcp/4001", "/ip4/0.0.0.0/tcp/4001/ws"}) {
      const auto listened = std::array{address(text)};
      BOOST_TEST(!value.observe(1, peer(1), local, remote, reported, listened, start));
   }
   const auto listened = std::array{address("/ip4/0.0.0.0/tcp/4001")};
   BOOST_TEST(!value.observe(1, peer(1), address("/ip4/10.0.0.2/tcp/54321"), remote, reported, listened, start));
   for (const auto* text : {"/ip4/8.8.8.8/udp/8000/quic-v1", "/ip6/2606:4700::1/tcp/8000",
                           "/ip4/8.8.8.8/tcp/8000/ws", "/ip4/8.8.8.8/tcp/0"}) {
      BOOST_TEST(!value.observe(1, peer(1), local, remote, address(text), listened, start));
   }
   for (const auto* text : {"/dns4/observer.test/tcp/5001", "/ip4/127.0.0.1/tcp/5001",
                           "/ip4/224.0.0.1/tcp/5001", "/ip4/11.0.0.1/udp/5001/quic-v1"}) {
      BOOST_TEST(!value.observe(1, peer(1), local, address(text), reported, listened, start));
   }
   auto wrong_remote = remote;
   wrong_remote.peer = peer(2);
   BOOST_TEST(!value.observe(1, peer(1), local, wrong_remote, reported, listened, start));
   BOOST_TEST(!value.observe(1, p2p::peer_id{}, local, remote, reported, listened, start));
   auto wrong_family = reported;
   wrong_family.transport.host_type = p2p::endpoint::host_kind::ip6;
   BOOST_TEST(!value.observe(1, peer(1), local, remote, wrong_family, listened, start));
   BOOST_TEST(value.confirmed(start).empty());
}

BOOST_AUTO_TEST_CASE(observed_address_quic_and_private_numeric_observations_keep_their_transport) {
   for (const auto quic : {false, true}) {
      auto value = manager{};
      const auto suffix = std::string{quic ? "/udp/4001/quic-v1" : "/tcp/4001"};
      const auto local = address("/ip4/10.0.0.2" + suffix);
      const auto listened = std::array{local};
      const auto reported = address("/ip4/192.168.1.2" + suffix);
      for (auto id = std::uint64_t{1}; id <= 4; ++id) {
         const auto remote = address("/ip4/10.1.0." + std::to_string(id) + suffix);
         BOOST_REQUIRE(value.observe(id, peer(id), local, remote, reported, listened, start));
      }
      const auto confirmed = value.confirmed(start);
      BOOST_REQUIRE_EQUAL(confirmed.size(), 1U);
      BOOST_TEST(confirmed.front().to_string() == reported.to_string());
   }
}

BOOST_AUTO_TEST_CASE(observed_address_wildcard_quic_bound_listener_collects_quorum) {
   for (const auto ipv6 : {false, true}) {
      auto value = manager{};
      const auto local = address(ipv6 ? "/ip6/::/udp/4001/quic-v1" : "/ip4/0.0.0.0/udp/4001/quic-v1");
      const auto listened = std::array{local};
      const auto reported = address(ipv6 ? "/ip6/2606:4700::1111/udp/8000/quic-v1"
                                         : "/ip4/8.8.8.8/udp/8000/quic-v1");
      for (auto id = std::uint64_t{1}; id <= 4; ++id) {
         const auto remote = address(ipv6 ? "/ip6/2001:4860:" + std::to_string(id) + "::1/udp/5001/quic-v1"
                                          : "/ip4/11.0.0." + std::to_string(id) + "/udp/5001/quic-v1");
         BOOST_REQUIRE(value.observe(id, peer(id), local, remote, reported, listened, start));
         if (id < 4) { BOOST_TEST(value.confirmed(start).empty()); }
      }
      const auto confirmed = value.confirmed(start);
      BOOST_REQUIRE_EQUAL(confirmed.size(), 1U);
      BOOST_TEST(confirmed.front().to_string() == reported.to_string());
      value.remove(4);
      BOOST_TEST(value.confirmed(start).empty());
   }
}

BOOST_AUTO_TEST_CASE(observed_address_wildcard_quic_requires_exact_owned_bind) {
   for (const auto ipv6 : {false, true}) {
      auto value = manager{};
      const auto local = address(ipv6 ? "/ip6/::/udp/4001/quic-v1" : "/ip4/0.0.0.0/udp/4001/quic-v1");
      const auto remote = address(ipv6 ? "/ip6/2001:4860::1/udp/5001/quic-v1" : "/ip4/11.0.0.1/udp/5001/quic-v1");
      const auto reported = address(ipv6 ? "/ip6/2606:4700::1111/udp/8000/quic-v1" : "/ip4/8.8.8.8/udp/8000/quic-v1");
      const auto invalid = ipv6
          ? std::array{"/ip6/fd00::2/udp/4001/quic-v1", "/ip6/::/udp/4002/quic-v1",
                       "/ip4/0.0.0.0/udp/4001/quic-v1", "/ip6/::/tcp/4001"}
          : std::array{"/ip4/10.0.0.2/udp/4001/quic-v1", "/ip4/0.0.0.0/udp/4002/quic-v1",
                       "/ip6/::/udp/4001/quic-v1", "/ip4/0.0.0.0/tcp/4001"};
      for (const auto* listener : invalid) {
         BOOST_TEST(!value.observe(1, peer(1), local, remote, reported, std::array{address(listener)}, start));
      }
      BOOST_TEST(!value.observe(1, peer(1), local, remote, reported, {}, start));
      BOOST_TEST(!value.observe(1, peer(1), local, remote, local, std::array{local}, start));
      BOOST_TEST(!value.observe(1, peer(1), local, local, reported, std::array{local}, start));
      const auto tcp_local = address(ipv6 ? "/ip6/::/tcp/4001" : "/ip4/0.0.0.0/tcp/4001");
      const auto tcp_remote = address(ipv6 ? "/ip6/2001:4860::1/tcp/5001" : "/ip4/11.0.0.1/tcp/5001");
      const auto tcp_reported = address(ipv6 ? "/ip6/2606:4700::1111/tcp/8000" : "/ip4/8.8.8.8/tcp/8000");
      BOOST_TEST(!value.observe(1, peer(1), tcp_local, tcp_remote, tcp_reported, std::array{tcp_local}, start));
      BOOST_TEST(value.confirmed(start).empty());
   }
}

BOOST_AUTO_TEST_CASE(observed_address_capacity_rejection_preserves_previous_votes) {
   const auto first = address("/ip4/8.8.8.8/tcp/8000");
   const auto second = address("/ip4/8.8.4.4/tcp/9000");
   auto observations = manager{manager::options{.max_observations = 4}};
   quorum4(observations, first);
   BOOST_TEST(!observe4(observations, 5, 5, "11.0.0.5", first));
   BOOST_REQUIRE(observe4(observations, 1, 1, "11.0.0.1", first, start + 1s));
   BOOST_REQUIRE_EQUAL(observations.confirmed(start + 1s).size(), 1U);
   observations.remove(4);
   BOOST_REQUIRE(observe4(observations, 5, 5, "11.0.0.5", first, start + 1s));
   BOOST_REQUIRE_EQUAL(observations.confirmed(start + 1s).size(), 1U);

   auto candidates = manager{manager::options{.max_candidates = 1}};
   quorum4(candidates, first);
   BOOST_TEST(!observe4(candidates, 1, 1, "11.0.0.1", second));
   BOOST_TEST(!observe4(candidates, 5, 5, "11.0.0.5", second));
   BOOST_REQUIRE_EQUAL(candidates.confirmed(start).size(), 1U);
   for (auto id = std::uint64_t{2}; id <= 4; ++id) {
      candidates.remove(id);
   }
   BOOST_REQUIRE(observe4(candidates, 1, 1, "11.0.0.1", second));
   candidates.expire(start + 10min);
   quorum4(candidates, first, 10, start + 10min);
   BOOST_REQUIRE_EQUAL(candidates.confirmed(start + 10min).size(), 1U);
}

BOOST_AUTO_TEST_CASE(observed_address_top_three_uses_counts_then_binary_address_order_per_local) {
   auto values = std::array{address("/ip4/8.8.8.10/tcp/8000"), address("/ip4/8.8.8.2/tcp/8000"),
                            address("/ip4/8.8.8.4/tcp/8000"), address("/ip4/8.8.8.3/tcp/8000")};
   auto first = manager{};
   auto reversed = manager{};
   for (auto index = std::size_t{}; index < values.size(); ++index) {
      quorum4(first, values[index], 1 + index * 10);
      quorum4(reversed, values[values.size() - 1 - index], 1 + index * 10);
   }
   BOOST_REQUIRE(observe4(first, 100, 100, "11.0.0.5", values.front()));
   BOOST_REQUIRE(observe4(reversed, 100, 100, "11.0.0.5", values.front()));
   const auto expected = std::vector<std::string>{"/ip4/8.8.8.10/tcp/8000", "/ip4/8.8.8.2/tcp/8000",
                                                 "/ip4/8.8.8.3/tcp/8000"};
   BOOST_CHECK(strings(first.confirmed(start)) == expected);
   BOOST_CHECK(strings(reversed.confirmed(start)) == expected);
   BOOST_CHECK(strings(first.confirmed(start)) == expected);
   const auto local = address("/ip4/10.0.0.3/tcp/5001");
   const auto listened = std::array{local};
   const auto extra = address("/ip4/8.8.4.4/tcp/9000");
   for (auto id = std::uint64_t{1}; id <= 4; ++id) {
      const auto remote = address("/ip4/11.0.0." + std::to_string(id) + "/tcp/6001");
      BOOST_REQUIRE(first.observe(200 + id, peer(id), local, remote, extra, listened, start));
   }
   BOOST_TEST(first.confirmed(start).size() == 4U);
}

BOOST_AUTO_TEST_CASE(observed_address_validates_options_and_saturates_expiry) {
   auto invalid = manager::options{};
   invalid.max_observations = 0;
   BOOST_CHECK_THROW(manager{invalid}, p2p::exceptions::invalid_options);
   invalid = {};
   invalid.max_candidates = 0;
   BOOST_CHECK_THROW(manager{invalid}, p2p::exceptions::invalid_options);
   invalid = {};
   invalid.min_observers = 0;
   BOOST_CHECK_THROW(manager{invalid}, p2p::exceptions::invalid_options);
   invalid = {};
   invalid.max_confirmed_per_local = 0;
   BOOST_CHECK_THROW(manager{invalid}, p2p::exceptions::invalid_options);
   invalid = {};
   invalid.ttl = 0s;
   BOOST_CHECK_THROW(manager{invalid}, p2p::exceptions::invalid_options);
   auto value = manager{};
   const auto near_end = time_point::max() - 1min;
   quorum4(value, address("/ip4/8.8.8.8/tcp/8000"), 1, near_end);
   BOOST_TEST(value.confirmed(near_end).size() == 1U);
   BOOST_TEST(value.confirmed(time_point::max()).empty());
}

BOOST_AUTO_TEST_CASE(observed_address_concurrent_observe_query_and_remove_share_bounded_state) {
   auto value = manager{};
   const auto reported = address("/ip4/8.8.8.8/tcp/8000");
   auto accepted = std::atomic_size_t{};
   auto workers = std::vector<std::jthread>{};
   for (auto worker = std::uint64_t{}; worker < 4; ++worker) {
      workers.emplace_back([&, worker] {
         for (auto index = std::uint64_t{1}; index <= 8; ++index) {
            const auto id = worker * 8 + index;
            if (observe4(value, id, id, "11.0.0." + std::to_string(id), reported)) {
               ++accepted;
            }
            static_cast<void>(value.confirmed(start));
         }
      });
   }
   workers.clear();
   BOOST_TEST(accepted.load() == 32U);
   BOOST_REQUIRE_EQUAL(value.confirmed(start).size(), 1U);
   for (auto worker = std::uint64_t{}; worker < 4; ++worker) {
      workers.emplace_back([&, worker] {
         for (auto index = std::uint64_t{1}; index <= 8; ++index) {
            value.remove(worker * 8 + index);
            static_cast<void>(value.confirmed(start));
         }
      });
   }
   workers.clear();
   BOOST_TEST(value.confirmed(start).empty());
}

BOOST_AUTO_TEST_SUITE_END()
