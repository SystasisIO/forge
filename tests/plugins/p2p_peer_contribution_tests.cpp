module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

module forge.plugins.net.p2p.node.plugin;

import forge.asio.runtime;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.plugins.net.p2p.node.peer_contribution;

#include "../../../plugins/net/p2p/node/details/peer_contributions.hxx"

namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
using namespace std::chrono_literals;
constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
constexpr auto minimum = (std::numeric_limits<std::int64_t>::min)();

struct contribution_fixture {
   forge::asio::runtime runtime{};
   std::shared_ptr<p2p::node> native;
   std::shared_ptr<np::detail::peer_contributions> owner = std::make_shared<np::detail::peer_contributions>();

   explicit contribution_fixture(std::size_t peers = 2, std::size_t per_peer = 4) {
      const auto identity = forge::tests::p2p::make_identity_fixture("contribution-local");
      auto options = p2p::node::options{.certificate_pem = identity.certificate_pem,
                                        .private_key_pem = identity.private_key_pem,
                                        .allow_insecure_test_mode = true};
      options.limits.topology.operating_mode = p2p::topology::mode::static_only;
      options.limits.topology.max_tagged_peers = peers;
      options.limits.topology.max_tags_per_peer = per_peer;
      const auto limits = options.limits.topology;
      native = std::make_shared<p2p::node>(runtime, std::move(options));
      owner->attach(native, limits);
   }
   ~contribution_fixture() {
      if (owner) { owner->close(); }
      auto stopped = boost::asio::co_spawn(runtime.context(), native->async_stop(), boost::asio::use_future);
      if (stopped.wait_for(5s) != std::future_status::ready) { std::terminate(); }
      stopped.get();
   }
};
}

// Production contribution registry, real native tag/protection mutations.
// No network or plugin-configured pruning claim is made by these component tests.
BOOST_AUTO_TEST_CASE(p2p_peer_contributions_remove_only_their_native_keys) {
   contribution_fixture f;
   const auto peer = p2p::peer_id{"remote"};
   for (const auto key : {"bootstrap", "forge:relay-client", "forge:relay-service", "direct-upgrade"}) {
      f.native->protect_peer(peer, key);
   }
   auto first = f.owner->protect(peer);
   auto second = f.owner->protect(peer);
   first.close();
   BOOST_TEST(!first.active());
   BOOST_TEST(second.active());
   BOOST_TEST(f.native->is_peer_protected(peer));
   auto moved = std::move(second);
   BOOST_TEST(!second.active());
   first = std::move(moved);
   BOOST_TEST(!moved.active());
   first.close();
   first.close();
   BOOST_TEST(f.native->is_peer_protected(peer));
   for (const auto key : {"bootstrap", "forge:relay-client", "forge:relay-service", "direct-upgrade"}) {
      static_cast<void>(f.native->unprotect_peer(peer, key));
   }
   BOOST_TEST(!f.native->is_peer_protected(peer));

   auto own = f.owner->tag(peer, maximum);
   BOOST_CHECK_THROW(f.native->tag_peer(peer, "external", 1), p2p::exceptions::invalid_options);
   own.close();
   f.native->tag_peer(peer, "external", 1);
   auto another = f.owner->tag(peer, -1);
   f.owner.reset();
   BOOST_TEST(!another.active());
   // The external tag remains; the registry's -1 must have been removed.
   BOOST_CHECK_THROW(f.native->tag_peer(peer, "probe", maximum), p2p::exceptions::invalid_options);
   BOOST_TEST(f.native->untag_peer(peer, "external"));
   f.native->tag_peer(peer, "probe", maximum);
   BOOST_TEST(f.native->untag_peer(peer, "probe"));
   another.close();
}

BOOST_AUTO_TEST_CASE(p2p_peer_contributions_bound_both_kinds_and_reclaim_after_removal) {
   contribution_fixture f{2, 2};
   const auto a = p2p::peer_id{"a"}, b = p2p::peer_id{"b"}, c = p2p::peer_id{"c"};
   auto protection = f.owner->protect(a);
   auto tag = f.owner->tag(a, 0);
   BOOST_CHECK_THROW((void)f.owner->protect(a), p2p::exceptions::backpressure_rejected);
   BOOST_CHECK_THROW((void)f.owner->tag(a, 1), p2p::exceptions::backpressure_rejected);
   auto second_peer = f.owner->protect(b);
   BOOST_CHECK_THROW((void)f.owner->protect(c), p2p::exceptions::backpressure_rejected);
   tag.close();
   auto replacement = f.owner->protect(a);
   protection.close();
   BOOST_TEST(f.native->is_peer_protected(a));
   replacement.close();
   BOOST_TEST(!f.native->is_peer_protected(a));
   auto third_peer = f.owner->tag(c, 1);
   f.owner->close();
   BOOST_TEST(!second_peer.active());
   BOOST_TEST(!third_peer.active());
   BOOST_TEST(!f.native->is_peer_protected(b));
   BOOST_CHECK_THROW((void)f.owner->tag(a, 0), p2p::exceptions::canceled);
   f.owner->close();
}

BOOST_AUTO_TEST_CASE(p2p_peer_contributions_native_rejection_rolls_back_reservation) {
   contribution_fixture f{1, 1};
   const auto peer = p2p::peer_id{"remote"};
   f.native->tag_peer(peer, "external", 0);
   BOOST_CHECK_THROW((void)f.owner->tag(peer, 0), p2p::exceptions::backpressure_rejected);
   BOOST_TEST(f.native->untag_peer(peer, "external"));
   auto next = f.owner->tag(peer, minimum);
   BOOST_TEST(next.active());
   next.close();
   auto other = f.owner->protect({"other"});
   BOOST_TEST(f.native->is_peer_protected({"other"}));
   BOOST_CHECK_THROW((void)f.owner->protect({}), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_peer_contributions_check_signed_subtotals_not_only_net_sum) {
   for (const auto negative_first : {false, true}) {
      contribution_fixture f;
      const auto peer = p2p::peer_id{"extremes"};
      auto first = f.owner->tag(peer, negative_first ? minimum : maximum);
      auto second = f.owner->tag(peer, negative_first ? maximum : minimum);
      BOOST_CHECK_THROW((void)f.owner->tag(peer, 1), p2p::exceptions::invalid_options);
      BOOST_CHECK_THROW((void)f.owner->tag(peer, -1), p2p::exceptions::invalid_options);
      first.close();
      second.close();
      auto next = f.owner->tag(peer, maximum);
      next.close();
      f.native->tag_peer(peer, "probe", minimum);
      BOOST_TEST(f.native->untag_peer(peer, "probe"));
   }
   auto order = std::array{0, 1, 2};
   do {
      contribution_fixture f;
      const auto peer = p2p::peer_id{"permutation"};
      auto tokens = std::array{f.owner->tag(peer, minimum / 2), f.owner->tag(peer, minimum / 2),
                               f.owner->tag(peer, maximum)};
      for (const auto index : order) {
         tokens[index].close();
         // Native addition evaluates the remaining aggregate in its real key order.
         f.native->tag_peer(peer, "probe", 0);
         BOOST_TEST(f.native->untag_peer(peer, "probe"));
      }
   } while (std::next_permutation(order.begin(), order.end()));
}
