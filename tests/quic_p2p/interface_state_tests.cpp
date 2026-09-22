#include <boost/test/unit_test.hpp>
#include "../../libraries/net/p2p/details/interface_state.hxx"

#include <array>
#include <stdexcept>

namespace {
using state = forge::net::p2p::detail::interface_state;

state::interface interface(std::uint32_t index = 7) {
   return {.index = index, .name = "test" + std::to_string(index), .up = true,
           .running = true, .multicast = true,
           .addresses = {{.value = boost::asio::ip::make_address("192.0.2.1"),
                          .prefix_length = 24, .flags_known = true}}};
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_interface_state_stable_and_normalized) {
   auto value = state{state::limits{}};
   auto first = interface();
   first.addresses.push_back({.value = boost::asio::ip::make_address("fe80::1%7"),
                             .prefix_length = 64, .flags_known = true});
   first.addresses.push_back(first.addresses.front());
   const auto initial = value.reconcile({interface(8), first});
   BOOST_REQUIRE_EQUAL(initial.interfaces.size(), 2U);
   BOOST_TEST(initial.interfaces[0].index == 7U);
   BOOST_TEST(initial.interfaces[0].addresses.size() == 2U);
   const auto& v6 = initial.interfaces[0].addresses[1];
   BOOST_TEST(v6.scope_id == 7U);
   BOOST_TEST(v6.value.to_v6().scope_id() == 0U);
   first.generation = 999;
   const auto again = value.reconcile({first, interface(8)});
   BOOST_TEST(again.revision == initial.revision + 1);
   BOOST_TEST(again.interfaces[0].generation == initial.interfaces[0].generation);
   BOOST_TEST(again.interfaces[1].generation == initial.interfaces[1].generation);
   BOOST_TEST(!again.continuity_lost);
}

BOOST_AUTO_TEST_CASE(p2p_interface_state_reuse_invalidation_and_reset) {
   auto value = state{state::limits{}};
   const auto first = value.reconcile({interface(7), interface(8)});
   const auto invalidated = std::array<std::uint32_t, 1>{7};
   const auto recreated = value.reconcile({interface(7), interface(8)}, invalidated);
   BOOST_TEST(recreated.interfaces[0].generation > first.interfaces[0].generation);
   BOOST_TEST(recreated.interfaces[1].generation == first.interfaces[1].generation);
   const auto removed = value.reconcile({interface(8)});
   BOOST_TEST(removed.interfaces.size() == 1U);
   const auto reused = value.reconcile({interface(7), interface(8)});
   BOOST_TEST(reused.interfaces[0].generation > recreated.interfaces[0].generation);
   const auto reset = value.reconcile({interface(7), interface(8)}, {}, true);
   BOOST_TEST(reset.continuity_lost);
   BOOST_TEST(reset.interfaces[0].generation > reused.interfaces[0].generation);
   BOOST_TEST(reset.interfaces[1].generation > reused.interfaces[1].generation);
}

BOOST_AUTO_TEST_CASE(p2p_interface_state_flags_prefix_and_link_changes_rotate_generation) {
   auto value = state{state::limits{}};
   auto item = interface();
   auto last = value.reconcile({item}).interfaces.front().generation;
   item.addresses.front().tentative = true;
   auto next = value.reconcile({item}).interfaces.front().generation;
   BOOST_TEST(next > last);
   last = next;
   item.addresses.front().tentative = false;
   item.addresses.front().deprecated = true;
   item.addresses.front().prefix_length = 25;
   next = value.reconcile({item}).interfaces.front().generation;
   BOOST_TEST(next > last);
   last = next;
   item.up = false;
   next = value.reconcile({item}).interfaces.front().generation;
   BOOST_TEST(next > last);
   last = next;
   item.up = true;
   item.point_to_point = true;
   next = value.reconcile({item}).interfaces.front().generation;
   BOOST_TEST(next > last);
}

BOOST_AUTO_TEST_CASE(p2p_interface_state_limits_and_invalid_snapshot_are_transactional) {
   auto value = state{state::limits{.interfaces = 1, .addresses_per_interface = 1}};
   const auto initial = value.reconcile({interface()});
   BOOST_CHECK_THROW(value.reconcile({interface(), interface(8)}), std::length_error);
   auto item = interface();
   item.addresses.push_back(item.addresses.front());
   BOOST_CHECK_THROW(value.reconcile({item}), std::length_error);
   item = interface();
   item.addresses.front().prefix_length = 33;
   BOOST_CHECK_THROW(value.reconcile({item}), std::invalid_argument);
   item.addresses = {{.value = boost::asio::ip::make_address("fe80::1%8")}};
   BOOST_CHECK_THROW(value.reconcile({item}), std::invalid_argument);
   item = interface();
   item.index = 0;
   BOOST_CHECK_THROW(value.reconcile({item}), std::invalid_argument);
   BOOST_TEST(value.current().revision == initial.revision);
   BOOST_TEST(value.current().interfaces.front().generation == initial.interfaces.front().generation);
   const auto unchanged = value.reconcile({interface()});
   BOOST_TEST(unchanged.interfaces.front().generation == initial.interfaces.front().generation);
   auto larger = state{state::limits{}};
   BOOST_CHECK_THROW(larger.reconcile({interface(), interface()}), std::invalid_argument);
   BOOST_TEST(larger.current().revision == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_interface_state_prefix_masks) {
   const auto zero = std::array<std::uint8_t, 4>{};
   const auto v4 = std::array<std::uint8_t, 4>{255, 255, 255, 128};
   const auto bad = std::array<std::uint8_t, 4>{255, 0, 255, 0};
   auto v6 = std::array<std::uint8_t, 16>{};
   v6.fill(255);
   BOOST_TEST(state::prefix(zero).value() == 0U);
   BOOST_TEST(state::prefix(v4).value() == 25U);
   BOOST_TEST(state::prefix(v6).value() == 128U);
   BOOST_TEST(!state::prefix(bad).has_value());
   BOOST_TEST(!state::prefix(std::span<const std::uint8_t>{v6}.first(15)).has_value());
}
