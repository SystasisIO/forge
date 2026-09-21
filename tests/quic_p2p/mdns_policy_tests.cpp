#include <boost/test/unit_test.hpp>

#include <chrono>

import forge.net.p2p.exceptions;
import forge.net.p2p.mdns_policy;

namespace {

namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

void check_invalid(const p2p::mdns_policy& value) {
   BOOST_CHECK_THROW(p2p::validate(value), p2p::exceptions::invalid_options);
}

} // namespace

BOOST_AUTO_TEST_SUITE(mdns_policy)

BOOST_AUTO_TEST_CASE(defaults_are_valid) {
   BOOST_CHECK_NO_THROW(p2p::validate(p2p::mdns_policy{}));
}

BOOST_AUTO_TEST_CASE(disabled_policy_allows_both_address_families_to_be_off) {
   auto policy = p2p::mdns_policy{};
   policy.ipv4_enabled = false;
   policy.ipv6_enabled = false;
   BOOST_CHECK_NO_THROW(p2p::validate(policy));
}

BOOST_AUTO_TEST_CASE(enabled_policy_requires_an_address_family) {
   auto policy = p2p::mdns_policy{};
   policy.enabled = true;
   policy.ipv4_enabled = false;
   policy.ipv6_enabled = false;
   check_invalid(policy);
}

BOOST_AUTO_TEST_CASE(resource_bounds_must_be_non_zero) {
   const auto expect_zero_rejected = [](auto set_zero) {
      auto policy = p2p::mdns_policy{};
      set_zero(policy);
      check_invalid(policy);
   };

   expect_zero_rejected([](auto& value) { value.max_interfaces = 0; });
   expect_zero_rejected([](auto& value) { value.max_peers = 0; });
   expect_zero_rejected([](auto& value) { value.max_addresses_per_peer = 0; });
   expect_zero_rejected([](auto& value) { value.max_pending_packets = 0; });
   expect_zero_rejected([](auto& value) { value.max_packet_size = 0; });
   expect_zero_rejected([](auto& value) { value.max_records_per_packet = 0; });
}

BOOST_AUTO_TEST_CASE(packet_size_bounds_are_inclusive) {
   auto policy = p2p::mdns_policy{};
   policy.max_packet_size = 511;
   check_invalid(policy);
   policy.max_packet_size = 8933;
   check_invalid(policy);
   policy.max_packet_size = 512;
   BOOST_CHECK_NO_THROW(p2p::validate(policy));
   policy.max_packet_size = 8932;
   BOOST_CHECK_NO_THROW(p2p::validate(policy));
}

BOOST_AUTO_TEST_CASE(ttl_and_query_interval_edges_are_checked) {
   auto policy = p2p::mdns_policy{};
   policy.record_ttl = 0s;
   check_invalid(policy);
   policy.record_ttl = 1s;
   policy.query_interval = 1s;
   check_invalid(policy);
   policy.record_ttl = 1h + 1s;
   check_invalid(policy);

   policy.record_ttl = 2s;
   policy.query_interval = 1s;
   BOOST_CHECK_NO_THROW(p2p::validate(policy));
   policy.query_interval = 999ms;
   check_invalid(policy);
   policy.query_interval = 1001ms;
   check_invalid(policy);

   policy.record_ttl = 1h;
   policy.query_interval = 30min;
   BOOST_CHECK_NO_THROW(p2p::validate(policy));
   policy.query_interval = 30min + 1ms;
   check_invalid(policy);
}

BOOST_AUTO_TEST_SUITE_END()
