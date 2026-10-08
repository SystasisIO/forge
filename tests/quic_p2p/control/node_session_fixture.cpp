module;

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include "pubsub_control_allocation.hxx"

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;

#include "../../../libraries/net/p2p/details/pubsub_control_queue.hxx"
#include "../../../libraries/net/p2p/details/pubsub_backoff.hxx"

namespace forge::tests::p2p { class pubsub_router_fixture; }
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;

peer_id node_session_fixture::peer(std::uint8_t value) {
   return make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, value)});
}

void node_session_fixture::queue_bounds() {
   auto limits = pubsub::limits{};
   limits.max_topics = (std::numeric_limits<std::size_t>::max)();
   limits.max_peers_per_topic = (std::numeric_limits<std::size_t>::max)();
   limits.max_remote_topic_entries = 37;
   limits.max_control_entries = 29;
   BOOST_TEST(detail::pubsub_control_queue::capacity_for(limits) == 29U);
   limits.max_topics = 2;
   limits.max_peers_per_topic = 3;
   BOOST_TEST(detail::pubsub_control_queue::capacity_for(limits) == 6U);
   limits.max_topics = 0;
   BOOST_TEST(detail::pubsub_control_queue::capacity_for(limits) == 0U);
}

void node_session_fixture::queue_latest_ack() {
   using queue = detail::pubsub_control_queue;
   auto limits = pubsub::limits{};
   limits.max_control_entries = 3;
   auto pool = queue{limits};
   const auto remote = peer(1);
   auto first = pool.prepare({queue::command{.peer = remote, .generation = 7,
       .args = {.subject = {"topic"}}}});
   BOOST_REQUIRE(first);
   pool.commit(std::move(*first));
   const auto old = pool.acquire(remote);
   BOOST_REQUIRE(old);
   auto next = pool.prepare({queue::command{.peer = remote, .generation = 7, .operation = queue::kind::prune,
       .args = {.subject = {"topic"}, .backoff = 17s}}});
   BOOST_REQUIRE(next);
   pool.commit(std::move(*next));
   BOOST_TEST(pool.size() == 2U); // Superseded in-flight and current both consume capacity.
   BOOST_CHECK(!pool.current(*old));
   BOOST_CHECK(!pool.acquire(remote));
   auto latest = pool.prepare({queue::command{.peer = remote, .generation = 7,
       .args = {.subject = {"topic"}}}});
   BOOST_REQUIRE(latest);
   pool.commit(std::move(*latest));
   BOOST_TEST(pool.size() == 2U); // Replacing an unleased pending PRUNE releases its own credit.
   pool.finish(old, true);
   BOOST_TEST(pool.size() == 1U);
   const auto current = pool.acquire(remote);
   BOOST_REQUIRE(current);
   BOOST_CHECK(current->items.front()->operation == queue::kind::graft);
   BOOST_TEST(current->items.front()->revision > old->items.front()->revision);
   pool.finish(old, true); // Delayed/double acknowledgement cannot consume another lease.
   BOOST_CHECK(pool.current(*current));
   pool.finish(current, true);
   BOOST_TEST(pool.size() == 0U);
   BOOST_TEST(pool.bytes() == 0U);
}

void node_session_fixture::queue_failure_and_lifetime() {
   using queue = detail::pubsub_control_queue;
   auto pool = queue{pubsub::limits{}};
   const auto remote = peer(2);
   auto staged = pool.prepare({queue::command{.peer = remote, .generation = 11,
       .operation = queue::kind::prune, .args = {.subject = {"topic"}, .backoff = 21s}}});
   BOOST_REQUIRE(staged);
   pool.commit(std::move(*staged));
   const auto first = pool.acquire(remote);
   pool.finish(first, false);
   const auto retry = pool.acquire(remote);
   BOOST_REQUIRE(retry);
   BOOST_TEST(retry->items.front()->revision == first->items.front()->revision);
   BOOST_TEST(retry->items.front()->args.backoff.count() == 21);
   pool.forget(remote);
   BOOST_TEST(pool.size() == 1U);
   auto fresh = pool.prepare({queue::command{.peer = remote, .generation = 12,
       .args = {.subject = {"topic"}}}});
   BOOST_REQUIRE(fresh);
   pool.commit(std::move(*fresh));
   BOOST_CHECK(!pool.current(*retry));
   BOOST_CHECK(!pool.acquire(remote));
   pool.finish(retry, true);
   const auto reconnected = pool.acquire(remote);
   BOOST_REQUIRE(reconnected);
   BOOST_TEST(reconnected->generation == 12U);
   pool.clear();
   pool.finish(reconnected, false);
   BOOST_TEST(pool.size() == 0U);
   BOOST_TEST(pool.bytes() == 0U);
}

void node_session_fixture::queue_batch_rollback() {
   using queue = detail::pubsub_control_queue;
   auto limits = pubsub::limits{};
   limits.max_control_entries = 2;
   auto pool = queue{limits};
   const auto remote = peer(3);
   auto staged = pool.prepare({queue::command{.peer = remote, .generation = 1,
       .args = {.subject = {"topic"}}}});
   BOOST_REQUIRE(staged);
   pool.commit(std::move(*staged));
   const auto old = pool.acquire(remote);
   auto refused = pool.prepare({
       queue::command{.peer = remote, .generation = 1, .operation = queue::kind::prune,
                      .args = {.subject = {"topic"}, .backoff = 30s}},
       queue::command{.peer = peer(4), .generation = 1, .args = {.subject = {"other"}}}});
   BOOST_CHECK(!refused);
   BOOST_CHECK(pool.current(*old));
   BOOST_TEST(pool.size() == 1U);
   const auto now = std::chrono::steady_clock::time_point{100s};
   auto backoff = detail::pubsub_backoff{};
   auto initial = backoff.prepare_local(std::vector<detail::pubsub_backoff::local_request>{{"topic", remote, 10s}}, now, 2);
   backoff.commit_local(std::move(initial));
   {
      auto aborted = backoff.prepare_local(std::vector<detail::pubsub_backoff::local_request>{
          {"topic", remote, 40s}, {"other", peer(4), 40s}}, now, 2);
      BOOST_TEST(aborted.rows.size() == 2U);
   }
   BOOST_CHECK(backoff.local_status("topic", remote, now + 11s) == detail::pubsub_backoff::status::none);
   BOOST_CHECK(backoff.local_status("other", peer(4), now) == detail::pubsub_backoff::status::none);
   pool.finish(old, true);
}

void node_session_fixture::queue_byte_bound() {
   using queue = detail::pubsub_control_queue;
   const auto remote = peer(5);
   const auto command = queue::command{.peer = remote, .generation = 1, .args = {.subject = {"topic"}}};
   auto measure = queue{pubsub::limits{}};
   auto staged = measure.prepare({command});
   BOOST_REQUIRE(staged);
   measure.commit(std::move(*staged));
   const auto charge = measure.bytes();
   auto limits = pubsub::limits{};
   limits.max_outbound_queue_bytes = charge;
   auto pool = queue{limits};
   staged = pool.prepare({command});
   BOOST_REQUIRE(staged);
   pool.commit(std::move(*staged));
   const auto old = pool.acquire(remote);
   BOOST_CHECK(!pool.prepare({command}));
   BOOST_CHECK(pool.current(*old));
   pool.finish(old, false);
   BOOST_TEST(pool.bytes() == charge);
   pool.forget(remote);
   BOOST_TEST(pool.bytes() == 0U);
   BOOST_REQUIRE(pool.prepare({command}));
}

void node_session_fixture::queue_legacy_bytes() {
   using queue = detail::pubsub_control_queue;
   auto pool = queue{pubsub::limits{}};
   auto staged = pool.prepare({queue::command{.peer = peer(6), .generation = 1, .operation = queue::kind::prune,
       .args = {.subject = {"topic"}, .peers = {{.peer = peer(7), .signed_peer_record = {1, 2, 3}}}, .backoff = 17s}}});
   BOOST_REQUIRE(staged);
   pool.commit(std::move(*staged));
   const auto batch = pool.acquire(peer(6));
   BOOST_REQUIRE(batch);
   const auto rpc = queue::rpc(*batch);
   const auto legacy = pubsub::options{.preferred = pubsub::version::v1_0};
   const auto bytes = pubsub::codec::encode(rpc, legacy);
   const auto expected = pubsub::codec::encode(pubsub::rpc{.control_value = pubsub::control{
       .prunes = {{.subject = {"topic"}}}}}, legacy);
   BOOST_TEST(bytes == expected, boost::test_tools::per_element());
   const auto decoded = pubsub::codec::decode(bytes, legacy);
   BOOST_TEST(decoded.control_value->prunes.front().peers.size() == 0U);
   BOOST_TEST(decoded.control_value->prunes.front().backoff.count() == 0);
   const auto modern = pubsub::codec::decode(pubsub::codec::encode(rpc));
   BOOST_TEST(modern.control_value->prunes.front().peers.size() == 1U);
   BOOST_TEST(modern.control_value->prunes.front().backoff.count() == 17);
   pool.finish(batch, false);
   const auto retry = pool.acquire(peer(6));
   BOOST_REQUIRE(retry);
   BOOST_TEST(pubsub::codec::encode(queue::rpc(*retry)) == pubsub::codec::encode(rpc), boost::test_tools::per_element());
   pool.finish(retry, true);
}

void node_session_fixture::queue_ephemeral_bound() {
   auto limits = pubsub::limits{};
   limits.max_control_entries = 2;
   auto pool = detail::pubsub_control_queue{limits};
   const auto first = pool.acquire(peer(1), 1, 12);
   const auto second = pool.acquire(peer(2), 1, 12);
   BOOST_REQUIRE(first);
   BOOST_REQUIRE(second);
   BOOST_CHECK(!pool.acquire(peer(3), 1, 12));
   BOOST_CHECK(!pool.acquire(peer(1), 1, 12));
   BOOST_TEST(pool.bytes() == 24U);
   pool.acknowledge(first);
   BOOST_CHECK(!pool.acquire(peer(1), 1, 12)); // An ACK is not release of the peer dispatch slot.
   pool.forget(peer(1));
   pool.finish(first, false);
   BOOST_TEST(pool.bytes() == 12U);
   const auto reconnected = pool.acquire(peer(1), 2, 12);
   BOOST_REQUIRE(reconnected);
   BOOST_TEST(reconnected->generation == 2U);
   pool.finish(second, false);
   pool.finish(reconnected, false);
   BOOST_TEST(pool.bytes() == 0U);
}

void node_session_fixture::backoff_allocation_refusal() {
   using backoff = detail::pubsub_backoff;
   using allocation = forge::tests::p2p::pubsub_control_allocation;
   const auto now = backoff::clock::time_point{100s};
   const auto remote = peer(9);
   const auto subject = std::string{"topic"};
   auto measured = std::size_t{};
   {
      auto value = backoff{};
      auto fault = allocation{};
      value.record_remote(subject, remote, 20s, now, 2);
      measured = fault.calls();
   }
   BOOST_REQUIRE(measured >= 2U);
   BOOST_REQUIRE(measured <= 8U);
   for (auto ordinal = std::size_t{1}; ordinal <= measured; ++ordinal) {
      auto value = backoff{};
      auto calls = std::size_t{};
      {
         auto fault = allocation{ordinal};
         value.record_remote(subject, remote, 20s, now, 2);
         calls = fault.calls();
      }
      BOOST_TEST(calls == ordinal);
      BOOST_TEST(value.size() == 0U);
      BOOST_CHECK(value.remote_status(subject, remote, now) == backoff::status::saturated);
      BOOST_CHECK(value.local_status(subject, remote, now) == backoff::status::none);
      value.expire(now + 21s);
      BOOST_CHECK(value.remote_status(subject, remote, now + 21s) == backoff::status::none);
      {
         auto fault = allocation{};
         value.record_remote(subject, remote, 20s, now + 21s, 2);
         calls = fault.calls();
      }
      BOOST_TEST(calls == measured); // No empty outer topic survived the inner-node failure.
      BOOST_TEST(value.size() == 1U);
   }
   auto existing = backoff{};
   existing.record_remote(subject, remote, 20s, now, 2);
   const auto another = peer(10);
   {
      auto fault = allocation{1};
      existing.record_remote(subject, another, 10s, now, 2);
   }
   BOOST_TEST(existing.size() == 1U);
   BOOST_CHECK(existing.remote_status(subject, remote, now) == backoff::status::exact);
   BOOST_CHECK(existing.remote_status(subject, another, now) == backoff::status::saturated);
   {
      const auto other_topic = std::string{"other"};
      auto fault = allocation{1};
      existing.record_local(other_topic, another, 5s, now, 2);
   }
   BOOST_CHECK(existing.local_status(subject, another, now) == backoff::status::saturated);
   existing.expire(now + 21s);
   BOOST_TEST(existing.size() == 0U);
}

void node_session_fixture::backoff_graft_slack() {
   using backoff = detail::pubsub_backoff;
   const auto now = backoff::clock::time_point{100s};
   const auto remote = peer(9);
   const auto subject = std::string{"topic"};
   const auto slack = backoff::graft_slack(400ms);
   BOOST_CHECK(slack == 800ms);
   auto sender = backoff{};
   auto receiver = backoff{};
   sender.record_local(subject, remote, 10s, now, 2);
   receiver.record_remote(subject, remote, 10s, now + 750ms, 2);
   sender.expire(now + 10s, slack);
   BOOST_CHECK(sender.local_status(subject, remote, now + 10s) == backoff::status::none);
   BOOST_CHECK(receiver.remote_status(subject, remote, now + 10s) == backoff::status::exact);
   BOOST_TEST(sender.graft_blocked(subject, remote, now + 10s, slack));
   BOOST_TEST(sender.size() == 1U);
   BOOST_TEST(sender.graft_blocked(subject, remote, now + 10s + 799ms, slack));
   BOOST_TEST(!sender.graft_blocked(subject, remote, now + 10s + 800ms, slack));
   BOOST_CHECK(receiver.remote_status(subject, remote, now + 10s + 800ms) == backoff::status::none);
   receiver.expire(now + 10s + 750ms, slack);
   BOOST_TEST(receiver.graft_blocked(subject, remote, now + 11s, slack));
   BOOST_TEST(!receiver.graft_blocked(subject, remote, now + 11s + 550ms, slack));
   receiver.expire(now + 11s + 550ms, slack);
   BOOST_TEST(receiver.size() == 0U);

   // Monotonic extensions and transactional preparation retain both exact and slack boundaries.
   sender.record_remote(subject, remote, 20s, now + 1s, 2);
   auto prepared = sender.prepare_local(std::vector<backoff::local_request>{{subject, remote, 30s}}, now, 2);
   sender.commit_local(std::move(prepared));
   sender.record_local(subject, remote, 1s, now + 2s, 2);
   BOOST_CHECK(sender.local_status(subject, remote, now + 29s) == backoff::status::exact);
   BOOST_CHECK(sender.local_status(subject, remote, now + 30s) == backoff::status::none);
   sender.expire(now + 30s, slack);
   BOOST_TEST(sender.graft_blocked(subject, remote, now + 30s, slack));
   sender.expire(now + 30s + 800ms, slack);
   BOOST_TEST(sender.size() == 0U);

   // Capacity refusal remains conservative after exact expiry without inventing a peer penalty.
   auto saturated = backoff{};
   saturated.record_local(subject, remote, 10s, now, 1);
   saturated.record_remote(subject, peer(10), 20s, now, 1);
   saturated.expire(now + 20s, slack);
   BOOST_CHECK(saturated.remote_status(subject, peer(10), now + 20s) == backoff::status::none);
   BOOST_TEST(saturated.graft_blocked(subject, peer(10), now + 20s, slack));
   BOOST_TEST(saturated.size() == 0U);
   saturated.expire(now + 20s + 800ms, slack);
   BOOST_TEST(!saturated.graft_blocked(subject, peer(10), now + 20s + 800ms, slack));

   BOOST_CHECK(backoff::graft_slack(std::chrono::milliseconds::max()) == backoff::clock::duration::max());
   auto extreme = backoff{};
   const auto end = backoff::clock::time_point::max();
   extreme.record_remote(subject, remote, 1s, end - 100ms, 1);
   extreme.expire(end - 1ms, slack);
   BOOST_TEST(extreme.graft_blocked(subject, remote, end - 1ms, slack));
   BOOST_CHECK(extreme.remote_status(subject, remote, end - 1ms) == backoff::status::exact);
   extreme.expire(end, slack);
   BOOST_TEST(extreme.size() == 0U);
   BOOST_TEST(!extreme.graft_blocked(subject, remote, end, slack));
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_CASE(control_queue_overflow_checked_capacity) { forge::net::p2p::node_session_fixture::queue_bounds(); }
BOOST_AUTO_TEST_CASE(control_queue_latest_revision_ack_and_inflight_bound) { forge::net::p2p::node_session_fixture::queue_latest_ack(); }
BOOST_AUTO_TEST_CASE(control_queue_failure_retry_and_peer_lifetime) { forge::net::p2p::node_session_fixture::queue_failure_and_lifetime(); }
BOOST_AUTO_TEST_CASE(control_queue_batch_refusal_preserves_pending_and_backoff) { forge::net::p2p::node_session_fixture::queue_batch_rollback(); }
BOOST_AUTO_TEST_CASE(control_queue_encoded_byte_bound_includes_inflight) { forge::net::p2p::node_session_fixture::queue_byte_bound(); }
BOOST_AUTO_TEST_CASE(control_queue_v10_bytes_unchanged_and_v11_args_frozen) { forge::net::p2p::node_session_fixture::queue_legacy_bytes(); }
BOOST_AUTO_TEST_CASE(control_queue_ephemeral_dispatch_peer_and_byte_bound) { forge::net::p2p::node_session_fixture::queue_ephemeral_bound(); }
BOOST_AUTO_TEST_CASE(control_backoff_first_and_second_allocation_refusal_saturates_without_partial_rows) { forge::net::p2p::node_session_fixture::backoff_allocation_refusal(); }
BOOST_AUTO_TEST_CASE(control_backoff_outgoing_slack_preserves_exact_incoming_deadline_and_bounds) { forge::net::p2p::node_session_fixture::backoff_graft_slack(); }
