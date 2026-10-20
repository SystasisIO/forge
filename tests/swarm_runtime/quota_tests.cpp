#include <boost/test/unit_test.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <limits>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

import forge.swarm.runtime.admission;
import forge.api.core.trusted_invocation;

namespace runtime_quota_test {
namespace swarm = forge::swarm::runtime;
namespace api = forge::api::core;
using caller = forge::api::auth::authenticated_caller;

caller principal() {
   return {forge::api::auth::caller_source::p2p_peer,
      forge::swarm::core::sha256::hash(std::string{"quota-test-principal"})};
}
caller trusted() {
   auto value = caller{};
   const auto authority = api::trusted_invocation_builder{}.set(principal()).build();
   if (!api::server_supplied<caller>::apply(value, authority)) throw std::runtime_error{"missing fixture caller"};
   return value;
}
swarm::quota::options config(std::uint64_t bytes) { return {.requester = principal(), .bytes = bytes}; }

BOOST_AUTO_TEST_CASE(quota_only_refunds_never_attempted_and_finalizes_once) {
   auto quota = swarm::quota{config(100)};
   { auto unused = quota.reserve(20); unused.finalize(); unused.finalize(); }
   BOOST_CHECK_EQUAL(quota.inspect().available, 100U);
   { auto unknown = quota.reserve(30); unknown.queue(); }
   BOOST_CHECK_EQUAL(quota.inspect().available, 70U);
   BOOST_CHECK_EQUAL(quota.inspect().traffic.uncertain, 30U);
   { auto accepted = quota.reserve(40); accepted.queue(); accepted.queued(); }
   const auto result = quota.inspect();
   BOOST_CHECK_EQUAL(result.available, 30U);
   BOOST_CHECK_EQUAL(result.held, 0U);
   BOOST_CHECK_EQUAL(result.finalized, 3U);
   BOOST_CHECK_EQUAL(result.traffic.attempted, 70U);
   BOOST_CHECK_EQUAL(result.traffic.queued, 40U);
   BOOST_CHECK_EQUAL(result.traffic.sent, 0U);
}

BOOST_AUTO_TEST_CASE(shared_grant_overlapping_threads_cannot_overspend) {
   auto quota = std::make_shared<swarm::quota>(config(64));
   auto accepted = std::atomic_uint{};
   auto workers = std::vector<std::jthread>{};
   auto start = std::barrier{17};
   auto attempted = std::latch{16};
   auto release = std::latch{1};
   for (auto i = 0U; i < 16; ++i) workers.emplace_back([&] {
      auto held = std::optional<swarm::quota::reservation>{};
      start.arrive_and_wait();
      try { held.emplace(quota->reserve(8)); held->queue(); held->queued(); ++accepted; }
      catch (const api::exceptions::invocation_denied&) {}
      attempted.count_down();
      release.wait();
   });
   start.arrive_and_wait(); attempted.wait();
   BOOST_CHECK_EQUAL(quota->inspect().held, 64U);
   BOOST_CHECK_EQUAL(quota->inspect().available, 0U);
   BOOST_CHECK_EQUAL(quota->inspect().finalized, 0U);
   release.count_down();
   workers.clear();
   BOOST_CHECK_EQUAL(accepted.load(), 8U);
   BOOST_CHECK_EQUAL(quota->inspect().available, 0U);
   BOOST_CHECK_EQUAL(quota->inspect().held, 0U);
   BOOST_CHECK_EQUAL(quota->inspect().finalized, 8U);
}

BOOST_AUTO_TEST_CASE(revocation_blocks_previously_reserved_output_and_notifies) {
   auto quota = swarm::quota{config(64)};
   auto reserved = quota.reserve(16);
   BOOST_CHECK(quota.permits({}, trusted()));
   BOOST_CHECK(!quota.permits({}, principal()));
   quota.revoke();
   BOOST_CHECK(quota.stop_token().stop_requested());
   BOOST_CHECK(!quota.permits({}, trusted()));
   BOOST_CHECK_THROW(reserved.queue(), api::exceptions::invocation_denied);
   reserved.finalize();
   BOOST_CHECK_EQUAL(quota.inspect().available, 64U);
   BOOST_CHECK_THROW(quota.reserve(1), api::exceptions::invocation_denied);
}

BOOST_AUTO_TEST_CASE(expired_grants_and_saturating_rejection_diagnostics) {
   auto options = config(10);
   options.expires = std::chrono::steady_clock::now();
   auto expired = swarm::quota{options};
   BOOST_CHECK_THROW(expired.reserve(1), api::exceptions::invocation_denied);
   auto quota = swarm::quota{config(10)};
   const auto maximum = std::numeric_limits<std::uint64_t>::max();
   BOOST_CHECK_THROW(quota.reserve(maximum), api::exceptions::invocation_denied);
   BOOST_CHECK_THROW(quota.reserve(maximum), api::exceptions::invocation_denied);
   BOOST_CHECK_EQUAL(quota.inspect().traffic.requested, maximum);
   BOOST_CHECK_EQUAL(quota.inspect().available, 10U);
}

BOOST_AUTO_TEST_CASE(moved_reservation_has_one_terminal_owner) {
   auto quota = swarm::quota{config(10)};
   { auto first = quota.reserve(5); auto second = std::move(first); second.queue(); second.queued(); }
   BOOST_CHECK_EQUAL(quota.inspect().finalized, 1U);
   BOOST_CHECK_EQUAL(quota.inspect().available, 5U);
}

} // namespace runtime_quota_test
