module;

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

module forge.net.p2p.node;

import forge.multiformats.multihash;
import forge.net.p2p.endpoint;
import forge.net.p2p.host_event;
import forge.net.p2p.identity;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.exceptions;

#include "../../libraries/net/p2p/details/reachability_state.hxx"

namespace forge::net::p2p {
namespace {

peer_id reachability_peer(std::uint8_t value) {
   const auto data = std::vector<std::uint8_t>{value};
   return peer_id::from_bytes(forge::multiformats::multihash::identity(data).encode());
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_reachability_state)

BOOST_AUTO_TEST_CASE(v1_requires_independent_nonconflicting_observers) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto addresses = std::vector<endpoint>{parse_endpoint("/ip4/8.8.8.8/tcp/4001")};
   const auto generation = state.set_addresses(addresses);
   const auto now = std::chrono::steady_clock::time_point{};
   const auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");
   for (std::uint8_t n = 1; n <= 3; ++n) {
      BOOST_REQUIRE(state.record_v1(reachability_peer(n), remote, generation,
          reachability::state::publicly_reachable, true, now));
   }
   BOOST_CHECK(state.snapshot(now).autonat_v1 == reachability::state::unknown);
   BOOST_REQUIRE(state.record_v1(reachability_peer(2), parse_endpoint("/ip4/2.2.2.2/tcp/4001"), generation,
       reachability::state::publicly_reachable, true, now));
   BOOST_REQUIRE(state.record_v1(reachability_peer(3), parse_endpoint("/ip4/3.3.3.3/tcp/4001"), generation,
       reachability::state::publicly_reachable, true, now));
   BOOST_CHECK(state.snapshot(now).autonat_v1 == reachability::state::publicly_reachable);
   BOOST_REQUIRE(state.record_v1(reachability_peer(4), remote, generation,
       reachability::state::private_network, true, now));
   BOOST_CHECK(state.snapshot(now).autonat_v1 == reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(v2_dialback_is_address_scoped_and_expires) {
   const auto policy = reachability_policy{};
   auto state = detail::reachability_state{policy};
   const auto address = parse_endpoint("/ip4/8.8.8.8/tcp/4001");
   const auto addresses = std::vector<endpoint>{address};
   const auto generation = state.set_addresses(addresses);
   const auto now = std::chrono::steady_clock::time_point{};
   const auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");
   BOOST_TEST(!state.record_v2(reachability_peer(1), remote, generation, address,
       reachability::state::publicly_reachable, false, now));
   BOOST_REQUIRE(state.record_v2(reachability_peer(1), remote, generation, address,
       reachability::state::publicly_reachable, true, now));
   const auto snapshot = state.snapshot(now);
   BOOST_CHECK(snapshot.autonat_v1 == reachability::state::unknown);
   BOOST_CHECK(snapshot.effective == reachability::state::publicly_reachable);
   BOOST_REQUIRE_EQUAL(snapshot.autonat_v2.size(), 1U);
   BOOST_CHECK(state.snapshot(now + policy.observation_ttl).effective == reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(v2_dialback_keeps_scoped_ipv6_interfaces_distinct) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto first = parse_endpoint("/ip6zone/1/ip6/2001:4860::1/tcp/4001");
   const auto second = parse_endpoint("/ip6zone/2/ip6/2001:4860::1/tcp/4001");
   const auto generation = state.set_addresses(std::vector<endpoint>{first});
   const auto now = std::chrono::steady_clock::time_point{};
   const auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");

   BOOST_TEST(!state.record_v2(reachability_peer(1), remote, generation, second,
       reachability::state::publicly_reachable, true, now));
   BOOST_REQUIRE(state.record_v2(reachability_peer(1), remote, generation, first,
       reachability::state::publicly_reachable, true, now));
}

BOOST_AUTO_TEST_CASE(lan_dialback_never_claims_public_internet) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto address = parse_endpoint("/ip4/192.168.1.2/tcp/4001");
   const auto generation = state.set_addresses(std::vector<endpoint>{address});
   const auto now = std::chrono::steady_clock::time_point{};
   BOOST_REQUIRE(state.record_v2(reachability_peer(1), parse_endpoint("/ip4/192.168.1.3/tcp/4001"),
       generation, address, reachability::state::publicly_reachable, true, now));
   BOOST_REQUIRE_EQUAL(state.snapshot(now).autonat_v2.size(), 1U);
   BOOST_CHECK(state.snapshot(now).effective == reachability::state::unknown);
   BOOST_TEST(!state.record_v1(reachability_peer(1), parse_endpoint("/ip4/192.168.1.3/tcp/4001"),
       generation, reachability::state::private_network, false, now));
}

BOOST_AUTO_TEST_CASE(address_change_invalidates_pending_evidence) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto address = parse_endpoint("/ip4/8.8.8.8/tcp/4001");
   const auto generation = state.set_addresses(std::vector<endpoint>{address});
   const auto now = std::chrono::steady_clock::time_point{};
   const auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");
   BOOST_REQUIRE(state.record_v2(reachability_peer(1), remote, generation, address,
       reachability::state::publicly_reachable, true, now));
   const auto replacement = parse_endpoint("/ip4/8.8.4.4/tcp/4001");
   const auto next = state.set_addresses(std::vector<endpoint>{replacement});
   BOOST_TEST(next == generation + 1);
   BOOST_TEST(state.set_addresses(std::vector<endpoint>{replacement}) == next);
   BOOST_TEST(!state.record_v2(reachability_peer(1), remote, generation, address,
       reachability::state::publicly_reachable, true, now));
   BOOST_TEST(!state.record_v1(reachability_peer(1), remote, generation,
       reachability::state::publicly_reachable, true, now));
   BOOST_CHECK(state.snapshot(now).effective == reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(negative_v2_does_not_classify_whole_node) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto address = parse_endpoint("/ip4/8.8.8.8/tcp/4001");
   const auto generation = state.set_addresses(std::vector<endpoint>{address});
   const auto now = std::chrono::steady_clock::time_point{};
   for (std::uint8_t n = 1; n <= 3; ++n) {
      auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");
      remote.transport.host = std::to_string(n) + ".1.1.1";
      BOOST_REQUIRE(state.record_v2(reachability_peer(n), remote, generation, address,
          reachability::state::private_network, false, now));
   }
   const auto snapshot = state.snapshot(now);
   BOOST_REQUIRE_EQUAL(snapshot.autonat_v2.size(), 1U);
   BOOST_CHECK(snapshot.autonat_v2.front().value == reachability::state::private_network);
   BOOST_CHECK(snapshot.effective == reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(address_generation_rejects_aba_and_preserves_identical_canonical_sets) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto address = parse_endpoint("/ip4/8.8.8.8/tcp/4001");
   const auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");
   const auto now = std::chrono::steady_clock::time_point{};
   const auto first = state.set_addresses(std::vector<endpoint>{address});
   static_cast<void>(state.set_addresses(std::vector<endpoint>{parse_endpoint("/ip4/8.8.4.4/tcp/4001")}));
   const auto returned = state.set_addresses(std::vector<endpoint>{address});
   BOOST_TEST(returned == first + 2);
   BOOST_TEST(!state.record_v1(reachability_peer(1), remote, first,
       reachability::state::publicly_reachable, true, now));
   BOOST_TEST(!state.record_v2(reachability_peer(1), remote, first, address,
       reachability::state::publicly_reachable, true, now));
   BOOST_REQUIRE(state.record_v2(reachability_peer(1), remote, returned, address,
       reachability::state::publicly_reachable, true, now));
   auto qualified = address;
   qualified.peer = reachability_peer(2);
   BOOST_TEST(state.set_addresses(std::vector<endpoint>{qualified, address}) == returned);
   const auto candidates = state.candidates();
   BOOST_TEST(candidates.generation == returned);
   BOOST_REQUIRE_EQUAL(candidates.addresses.size(), 1U);
   BOOST_TEST(!candidates.addresses.front().peer);
   BOOST_TEST(state.snapshot(now).autonat_v2.size() == 1U);
}

BOOST_AUTO_TEST_CASE(invalidation_and_close_reject_pending_evidence_without_reopening_admission) {
   auto state = detail::reachability_state{reachability_policy{}};
   const auto address = parse_endpoint("/ip4/8.8.8.8/tcp/4001");
   const auto remote = parse_endpoint("/ip4/1.1.1.1/tcp/4001");
   const auto now = std::chrono::steady_clock::time_point{};
   const auto first = state.set_addresses(std::vector<endpoint>{address});
   BOOST_REQUIRE(state.record_v2(reachability_peer(1), remote, first, address,
       reachability::state::publicly_reachable, true, now));
   static_assert(noexcept(state.invalidate()));
   static_assert(noexcept(state.close()));
   state.invalidate();
   BOOST_TEST(state.candidates().generation == first + 1);
   BOOST_TEST(state.candidates().addresses.empty());
   BOOST_TEST(state.snapshot(now).autonat_v2.empty());
   const auto restored = state.set_addresses(std::vector<endpoint>{address});
   BOOST_TEST(!state.record_v2(reachability_peer(1), remote, first, address,
       reachability::state::publicly_reachable, true, now));
   state.close();
   state.close();
   state.invalidate();
   BOOST_TEST(!state.record_v2(reachability_peer(1), remote, restored, address,
       reachability::state::publicly_reachable, true, now));
   BOOST_TEST(!state.record_v1(reachability_peer(1), remote, restored,
       reachability::state::publicly_reachable, true, now));
   BOOST_CHECK_THROW(static_cast<void>(state.set_addresses(std::vector<endpoint>{address})), exceptions::closed);
   BOOST_TEST(state.candidates().addresses.empty());
}

BOOST_AUTO_TEST_CASE(invalid_policy_fails_before_runtime) {
   auto policy = reachability_policy{};
   policy.max_pending_probes = 0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);
   policy = reachability_policy{};
   policy.observation_ttl = std::chrono::milliseconds::max();
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
