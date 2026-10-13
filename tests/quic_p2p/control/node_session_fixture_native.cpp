module;

#include <forge/exceptions/macros.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <stop_token>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include "pubsub_control_allocation.hxx"

module forge.net.p2p.node;

import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.multiformats.multiaddr;
import forge.multiformats.varint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../../../libraries/net/p2p/details/node_impl.hxx"
#include "../../../libraries/net/p2p/details/pubsub_peer_score.hxx"
#include "../../../libraries/net/p2p/details/pubsub_router.hxx"
#include "../../../libraries/net/p2p/details/length_delimited.hxx"
#include "../pubsub_router_fixture.hxx"
#include "../gossipsub_test_shutdown.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;
using forge::tests::p2p::pubsub_router_fixture;
using forge::tests::p2p::gossipsub_test_shutdown;

void node_session_fixture::run(forge::asio::runtime& runtime, boost::asio::awaitable<void> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (result.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   result.get();
}

pubsub::options node_session_fixture::manual_options() {
   auto out = pubsub::options{};
   out.limits.heartbeat_initial_delay = 60s;
   out.limits.heartbeat_interval = 60s;
   return out;
}

std::size_t node_session_fixture::controls(const pubsub_router_fixture& fixture,
                                           const node& owner, const peer_id& peer, bool graft) {
   auto count = std::size_t{};
   for (const auto& receipt : fixture.receipts(owner)) {
      if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != peer || receipt.frame.empty()) { continue; }
      const auto rpc = pubsub::codec::decode(receipt.frame, owner.impl_->options.limits.pubsub);
      if (rpc.control_value) {
         count += graft ? rpc.control_value->grafts.size() : rpc.control_value->prunes.size();
      }
   }
   return count;
}

void node_session_fixture::native_retry(bool rejected, bool legacy) {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   if (legacy) { config.preferred = pubsub::version::v1_0; }
   auto& source = fixture.add("control-retry-source", config);
   auto& target = fixture.add("control-retry-target", config);
   auto self = source.impl_;
   const auto peer = target.local_peer();
   auto reserved = false;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      if (reserved) {
         self->release_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
         reserved = false;
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(peer) &&
          self->pubsub_value.peer_topics.at(peer).contains(fixture.topic.value) &&
          self->pubsub_value.outbound.contains(peer) && !self->pubsub_value.outbound.at(peer).snapshot_pending;
   }));
   const auto failures = source.peers().find(peer)->failures;
   self->reserve_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
   reserved = true;
   auto backoff_until = std::chrono::steady_clock::time_point{};
   if (rejected) {
      auto session = std::shared_ptr<node::impl::session_state>{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         session = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
         BOOST_REQUIRE(session);
         const auto now = std::chrono::steady_clock::now();
         auto staged = self->pubsub_value.backoffs.prepare_local(
             std::vector<detail::pubsub_backoff::local_request>{{fixture.topic.value, peer, 1s}}, now, 32);
         self->pubsub_value.backoffs.commit_local(std::move(staged));
      }
      run(fixture.runtime, self->handle_pubsub_control(session,
          pubsub::control{.grafts = {{.subject = fixture.topic}}},
          legacy ? builtins::meshsub_v10 : builtins::meshsub_v11));
      const auto lock = std::scoped_lock{self->mutex};
      auto inspect = self->pubsub_value.backoffs.prepare_local(
          std::vector<detail::pubsub_backoff::local_request>{{fixture.topic.value, peer, 1s}},
          std::chrono::steady_clock::time_point{}, 32);
      backoff_until = inspect.rows.at(fixture.topic.value).at(peer).local_until;
   } else {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      BOOST_TEST(source.pubsub_snapshot().mesh_edges == 1U);
   }
   BOOST_TEST(controls(fixture, source, peer, !rejected) == 0U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.controls->size() == 1U);
   }
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   self->release_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
   reserved = false;
   BOOST_REQUIRE(fixture.wait([&] {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      return controls(fixture, source, peer, !rejected) == 1U;
   }));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->size() == 0;
   }));
   if (rejected) {
      const auto lock = std::scoped_lock{self->mutex};
      auto inspect = self->pubsub_value.backoffs.prepare_local(
          std::vector<detail::pubsub_backoff::local_request>{{fixture.topic.value, peer, 1s}},
          std::chrono::steady_clock::time_point{}, 32);
      BOOST_CHECK(inspect.rows.at(fixture.topic.value).at(peer).local_until == backoff_until);
   }
   if (legacy) {
      auto prunes = std::size_t{};
      for (const auto& receipt : fixture.receipts(source)) {
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != peer || receipt.frame.empty()) { continue; }
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         if (!rpc.control_value || rpc.control_value->prunes.empty()) { continue; }
         BOOST_CHECK(receipt.protocol == builtins::meshsub_v10);
         for (const auto& prune : rpc.control_value->prunes) {
            BOOST_TEST(prune.backoff.count() == 0);
            BOOST_TEST(prune.peers.size() == 0U);
            ++prunes;
         }
      }
      BOOST_TEST(prunes == 1U);
   }
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_unsubscribe_rollback() {
   using queue = detail::pubsub_control_queue;
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.limits.max_control_entries = 2;
   auto& source = fixture.add("control-batch-source", config);
   auto& first = fixture.add("control-batch-first", manual_options());
   auto& second = fixture.add("control-batch-second", manual_options());
   const auto self = source.impl_;
   auto old_first = std::shared_ptr<const queue::batch>{};
   auto old_second = std::shared_ptr<const queue::batch>{};
   auto reserved = false;
   auto release = [&] {
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->pubsub_value.controls->finish(old_first, false);
         self->pubsub_value.controls->finish(old_second, false);
      }
      if (reserved) {
         self->release_pubsub_outbound_bytes(first.local_peer(), config.limits.max_outbound_queue_bytes);
         reserved = false;
      }
   };
   auto second_shutdown = gossipsub_test_shutdown{fixture.runtime, source, second, release};
   auto first_shutdown = gossipsub_test_shutdown{fixture.runtime, source, first, release};
   fixture.subscribe(source);
   fixture.subscribe(first);
   fixture.subscribe(second);
   fixture.connect(source, first);
   fixture.connect(source, second);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.size() == 2U && self->pubsub_value.outbound.size() == 2U &&
          std::ranges::all_of(self->pubsub_value.outbound, [](const auto& row) { return !row.second.snapshot_pending; });
   }));
   self->reserve_pubsub_outbound_bytes(first.local_peer(), config.limits.max_outbound_queue_bytes);
   reserved = true;
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_TEST(source.pubsub_snapshot().mesh_edges == 2U);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      if (!old_first) { old_first = self->pubsub_value.controls->acquire(first.local_peer()); }
      if (!old_second) { old_second = self->pubsub_value.controls->acquire(second.local_peer()); }
      return old_first && old_second;
   }));
   try {
      run(fixture.runtime, source.async_unsubscribe(fixture.topic));
      BOOST_FAIL("full unsubscribe batch must refuse before local changes");
   } catch (const forge::exceptions::base& error) {
      BOOST_CHECK(exceptions::is(error, exceptions::code::backpressure_rejected));
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->pubsub_value.handlers.contains(fixture.topic.value));
      BOOST_TEST(self->pubsub_value.mesh.at(fixture.topic.value).size() == 2U);
      BOOST_CHECK(self->pubsub_value.controls->current(*old_first));
      BOOST_CHECK(self->pubsub_value.controls->current(*old_second));
      BOOST_TEST(self->pubsub_value.backoffs.size() == 0U);
   }
   release();
   BOOST_REQUIRE(fixture.wait([&] {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->size() == 0U;
   }));
   run(fixture.runtime, source.async_unsubscribe(fixture.topic));
   BOOST_TEST(source.pubsub_snapshot().mesh_edges == 0U);
   BOOST_REQUIRE(fixture.wait([&] {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      return controls(fixture, source, first.local_peer(), false) == 1U &&
          controls(fixture, source, second.local_peer(), false) == 1U;
   }));
   first_shutdown.join();
   second_shutdown.join();
}

void node_session_fixture::native_unsubscribe_after_stop() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.preferred = pubsub::version::v1_3;
   config.partial_messages = true;
   config.flood_publish = true;
   auto& owner = fixture.add("unsubscribe-stop-owner", config);
   auto& remote = fixture.add("unsubscribe-stop-remote", config);
   const auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto entered = std::atomic_bool{false}, exited = std::atomic_bool{false};
   auto pending = std::future<void>{};
   auto token = pubsub::partial_topic{};
   const auto join = [&](auto deadline) {
      if (!pending.valid()) { return; }
      if (pending.wait_until(deadline) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
      pending.get();
   };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, [release] { release->notify(); }, join};
   const auto handler = [&](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      // Keep the held wait's cancellation policy out of the native inbound parent.
      const auto held = [release, epoch, &entered, &exited]() -> boost::asio::awaitable<pubsub::validation_result> {
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
         entered = true;
         static_cast<void>(co_await release->async_wait(epoch));
         exited = true;
         co_return pubsub::validation_result::accept;
      };
      co_return co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
          held(), boost::asio::use_awaitable);
   };
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      token = co_await owner.async_subscribe(fixture.topic, handler, pubsub::partial_options{
          .receive = [](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; },
          .gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; }});
   }());
   fixture.subscribe(remote);
   fixture.connect(remote, owner);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{remote.impl_->mutex};
      const auto out = remote.impl_->pubsub_value.outbound.find(owner.local_peer());
      return remote.impl_->pubsub_value.peer_topics.contains(owner.local_peer()) &&
          remote.impl_->pubsub_value.peer_topics.at(owner.local_peer()).contains(fixture.topic.value) &&
          out != remote.impl_->pubsub_value.outbound.end() && !out->second.snapshot_pending;
   }));
   static_cast<void>(fixture.publish(remote, "unsubscribe-held-callback"));
   BOOST_REQUIRE(fixture.wait([&] { return entered.load(); }));
   owner.request_stop();
   const auto writes = [&] {
      return std::ranges::count_if(fixture.receipts(owner), [](const auto& value) {
         return value.kind == pubsub::trace_kind::rpc_write;
      });
   };
   const auto before = writes();
   run(fixture.runtime, owner.async_unsubscribe(fixture.topic));
   run(fixture.runtime, owner.async_unsubscribe(fixture.topic));
   BOOST_CHECK_THROW(run(fixture.runtime, owner.async_unsubscribe(token)), forge::exceptions::base);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().topics, 0U);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().partial_groups, 0U);
   BOOST_CHECK(!exited.load());
   BOOST_CHECK_EQUAL(writes(), before);
   pending = boost::asio::co_spawn(fixture.runtime.context(), owner.async_stop(), boost::asio::use_future);
   BOOST_CHECK(pending.wait_for(0ms) != std::future_status::ready);
   release->notify();
   join(std::chrono::steady_clock::now() + 5s);
   BOOST_CHECK(exited.load());
   run(fixture.runtime, owner.async_unsubscribe(fixture.topic));
   BOOST_CHECK_EQUAL(writes(), before);
   shutdown.join();
}

void node_session_fixture::control_preparation_allocation_rollback() {
   using queue = detail::pubsub_control_queue;
   using allocation = forge::tests::p2p::pubsub_control_allocation;
   auto fixture = pubsub_router_fixture{};
   auto& owner = fixture.add("control-atomic-owner", manual_options());
   auto& remote_a = fixture.add("control-atomic-first", manual_options());
   auto& remote_b = fixture.add("control-atomic-second", manual_options());
   const auto self = owner.impl_;
   auto first = std::shared_ptr<const queue::batch>{};
   auto second = std::shared_ptr<const queue::batch>{};
   const auto release = [&] {
      const auto lock = std::scoped_lock{self->mutex};
      self->pubsub_value.controls->finish(first, false);
      self->pubsub_value.controls->finish(second, false);
   };
   auto first_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote_a, release};
   auto second_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote_b, release};
   fixture.subscribe(owner);
   fixture.connect(owner, remote_a);
   fixture.connect(owner, remote_b);
   const auto a = remote_a.local_peer();
   const auto b = remote_b.local_peer();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.outbound.contains(a) && self->pubsub_value.outbound.contains(b) &&
          !self->pubsub_value.outbound.at(a).snapshot_pending && !self->pubsub_value.outbound.at(b).snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0;
   }));
   const auto now = std::chrono::steady_clock::now();
   const auto retained_topic = std::string{"retained.backoff"};
   auto commands = std::vector<queue::command>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_REQUIRE(self->connect_pubsub_peer_locked(a));
      BOOST_REQUIRE(self->connect_pubsub_peer_locked(b));
      BOOST_REQUIRE(self->admit_pubsub_control_locked(self->make_pubsub_control_locked(
          a, fixture.topic, queue::kind::graft, 0s, {}), now));
      BOOST_REQUIRE(self->admit_pubsub_control_locked(self->make_pubsub_control_locked(
          b, fixture.topic, queue::kind::graft, 0s, {}), now));
      first = self->pubsub_value.controls->acquire(a);
      second = self->pubsub_value.controls->acquire(b);
      commands.push_back(self->make_pubsub_control_locked(a, fixture.topic, queue::kind::prune, 10s, {}));
      commands.push_back(self->make_pubsub_control_locked(b, fixture.topic, queue::kind::prune, 10s, {}));
      auto retained = self->pubsub_value.backoffs.prepare_local(
          std::vector<detail::pubsub_backoff::local_request>{{retained_topic, a, 20s}}, now, 32);
      self->pubsub_value.backoffs.commit_local(std::move(retained));
   }
   auto measured = std::size_t{};
   auto repeated = std::size_t{};
   auto queued_calls = std::size_t{};
   auto backoff_calls = std::size_t{};
   auto backoffs = std::vector<detail::pubsub_backoff::local_request>{};
   for (const auto& command : commands) {
      backoffs.push_back({command.args.subject.value, command.peer, command.args.backoff});
   }
   BOOST_REQUIRE(commands.size() == 2U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      auto fault = allocation{};
      const auto prepared = self->prepare_pubsub_controls_locked(commands, now);
      measured = fault.calls();
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      auto fault = allocation{};
      const auto prepared = self->prepare_pubsub_controls_locked(commands, now);
      repeated = fault.calls();
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      auto fault = allocation{};
      const auto prepared = self->pubsub_value.controls->prepare(commands);
      queued_calls = fault.calls();
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      auto fault = allocation{};
      const auto prepared = self->pubsub_value.backoffs.prepare_local(backoffs, now,
          detail::pubsub_backoff::limit_for(self->options.limits.pubsub.limits.max_topics,
                                           self->options.limits.max_sessions));
      backoff_calls = fault.calls();
   }
   std::fprintf(stderr, "control-preparation allocations total=%zu repeat=%zu input+queue+wire=%zu backoff=%zu\n",
       measured, repeated, queued_calls, backoff_calls);
   std::fflush(stderr);
   BOOST_REQUIRE(measured > 0U);
   BOOST_REQUIRE(repeated == measured);
   BOOST_REQUIRE(queued_calls > 0U && backoff_calls > 0U);
   BOOST_REQUIRE(measured > queued_calls + backoff_calls);
   // Sweep every measured allocation for these exact two immutable commands,
   // including both canonical wire versions; a serializer-specific cap misses sites.
   auto exercised = std::size_t{};
   for (auto ordinal = std::size_t{1}; ordinal <= measured; ++ordinal) {
      auto failed = false;
      auto calls = std::size_t{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         auto fault = allocation{ordinal};
         try { static_cast<void>(self->prepare_pubsub_controls_locked(commands, now)); }
         catch (const std::bad_alloc&) { failed = true; }
         calls = fault.calls();
      }
      BOOST_REQUIRE(failed);
      BOOST_TEST(calls == ordinal);
      ++exercised;
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->pubsub_value.controls->current(*first));
      BOOST_CHECK(self->pubsub_value.controls->current(*second));
      BOOST_TEST(self->pubsub_value.controls->size() == 2U);
      BOOST_TEST(self->pubsub_value.mesh.at(fixture.topic.value).size() == 2U);
      BOOST_CHECK(self->pubsub_value.handlers.contains(fixture.topic.value));
      BOOST_TEST(self->pubsub_value.backoffs.size() == 1U);
      BOOST_CHECK(self->pubsub_value.backoffs.local_status(retained_topic, a, now + 19s) ==
          detail::pubsub_backoff::status::exact);
      BOOST_CHECK(self->pubsub_value.backoffs.local_status(retained_topic, a, now + 21s) ==
          detail::pubsub_backoff::status::none);
      BOOST_CHECK(self->pubsub_value.backoffs.local_status(fixture.topic.value, a, now) ==
          detail::pubsub_backoff::status::none);
   }
   BOOST_TEST(exercised == measured);
   second_shutdown.join();
   first_shutdown.join();
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(remote_a.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(remote_b.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_heartbeat_allocation_recovery() {
   using allocation = forge::tests::p2p::pubsub_control_allocation;
   auto fixture = pubsub_router_fixture{};
   const auto arm = std::make_shared<std::atomic_bool>(false);
   const auto injected = std::make_shared<std::atomic_size_t>(0);
   const auto samples = std::make_shared<std::atomic_size_t>(0);
   auto config = manual_options();
   config.limits.heartbeat_initial_delay = 1s;
   config.limits.heartbeat_interval = 50ms;
   config.scoring = pubsub::scoring_params{.app_specific_score = [arm, injected, samples](const peer_id&) {
      const auto sample = ++*samples;
      if (arm->exchange(false)) { allocation::fail_next(*injected); }
      return static_cast<double>(sample);
   }, .app_specific_weight = 1.0};
   auto& source = fixture.add("control-heartbeat-pressure-source", config);
   auto& target = fixture.add("control-heartbeat-pressure-target", manual_options());
   const auto self = source.impl_;
   auto reserved = false;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      arm->store(false);
      if (reserved) {
         self->release_pubsub_outbound_bytes(target.local_peer(), config.limits.max_outbound_queue_bytes);
         reserved = false;
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(target.local_peer()) &&
          self->pubsub_value.outbound.contains(target.local_peer()) &&
          !self->pubsub_value.outbound.at(target.local_peer()).snapshot_pending;
   }));
   self->reserve_pubsub_outbound_bytes(target.local_peer(), config.limits.max_outbound_queue_bytes);
   reserved = true;
   run(fixture.runtime, self->pubsub_heartbeat_once());
   run(fixture.runtime, source.async_unsubscribe(fixture.topic));
   const auto healthy_topic = pubsub::topic{"forge.control.pressure.healthy"};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(healthy_topic, accept));
      static_cast<void>(co_await target.async_subscribe(healthy_topic, accept));
   }());
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.at(target.local_peer()).contains(healthy_topic.value);
   }));
   const auto failures = source.peers().find(target.local_peer())->failures;
   const auto before_samples = samples->load();
   arm->store(true);
   BOOST_REQUIRE(fixture.wait([&] { return injected->load(std::memory_order_acquire) == 1U; }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->pubsub_value.controls->peers().empty());
   }
   self->release_pubsub_outbound_bytes(target.local_peer(), config.limits.max_outbound_queue_bytes);
   reserved = false;
   // No manual heartbeat after the fault: the original lifecycle-owned loop must resume itself.
   BOOST_REQUIRE(fixture.wait([&] {
      auto repaired = false;
      for (const auto& receipt : fixture.receipts(source)) {
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.frame.empty()) { continue; }
         const auto rpc = pubsub::codec::decode(receipt.frame);
         if (rpc.control_value) {
            repaired = repaired || std::ranges::any_of(rpc.control_value->grafts,
                [&](const auto& graft) { return graft.subject == healthy_topic; });
         }
      }
      return repaired && samples->load() >= before_samples + 2U;
   }));
   BOOST_TEST(injected->load() == 1U);
   BOOST_TEST(source.peers().find(target.local_peer())->failures == failures);
   BOOST_TEST(source.pubsub_snapshot().application_score_failures == 0U);
   shutdown.join();
}

void node_session_fixture::native_neutral_wire_topic() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.signatures = pubsub::signature_policy::lax_no_sign;
   config.limits.max_topic_size = 512;
   BOOST_CHECK(!config.scoring);
   auto& source = fixture.add("neutral-topic-source", config);
   auto& target = fixture.add("neutral-topic-target", config);
   const auto self = target.impl_;
   const auto subject = pubsub::topic{std::string(256, 't')};
   const auto validations = std::make_shared<std::atomic_size_t>(0);
   auto incoming = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] { incoming.request_cancel(); }};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(subject, accept));
      static_cast<void>(co_await target.async_subscribe(subject,
          [validations, subject](pubsub::event event) -> boost::asio::awaitable<pubsub::validation_result> {
             if (event.value.subject != subject || event.value.data != std::vector<std::uint8_t>{'w'}) {
                co_return pubsub::validation_result::reject;
             }
             ++*validations;
             co_return pubsub::validation_result::accept;
          }));
   }());
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(source.local_peer()) &&
          self->pubsub_value.peer_topics.at(source.local_peer()).contains(subject.value);
   }));
   // Permissive legacy codec/neutral capacity regression, not a normative eight-byte seqno proof.
   const auto value = pubsub::message{.from = source.local_peer(), .data = {'w'},
       .seqno = std::vector<std::uint8_t>(512, 1), .subject = subject};
   const auto id = pubsub::codec::message_id(value, config);
   BOOST_TEST(id.size() > 256U);
   const auto frame = pubsub::codec::encode(pubsub::rpc{.messages = {value}}, config);
   BOOST_CHECK(pubsub::codec::decode(frame, config).messages.front().subject == subject);
   incoming = fixture.open(source, target);
   run(fixture.runtime, incoming.async_write(frame));
   BOOST_REQUIRE(fixture.wait([&] { return target.pubsub_snapshot().messages_delivered == 1U; }));
   BOOST_TEST(validations->load() == 1U);
   BOOST_TEST(target.pubsub_snapshot().messages_received == 1U);
   BOOST_TEST(target.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(target.pubsub_scores().capacity_rejections == 0U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto key = bytes_key(id);
      BOOST_CHECK(self->pubsub_value.cache.contains(key));
      BOOST_CHECK(self->can_serve_pubsub_message_locked(key));
      BOOST_TEST(self->pubsub_value.scoring->score(source.local_peer(), std::chrono::steady_clock::now()) == 0.0);
   }
   run(fixture.runtime, incoming.async_close());
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_owned_codec_topic(bool sign) {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.signatures = sign ? pubsub::signature_policy::strict_sign : pubsub::signature_policy::strict_no_sign;
   config.limits.max_topic_size = 512;
   auto& source = fixture.add(sign ? "owned-codec-signed-source" : "owned-codec-nosign-source", config);
   auto& target = fixture.add(sign ? "owned-codec-signed-target" : "owned-codec-nosign-target", config);
   const auto self = target.impl_;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target};
   const auto subject = pubsub::topic{std::string(256, 't')};
   const auto validations = std::make_shared<std::atomic_size_t>(0);

   // Old typed function-pointer signatures remain callable; options affect bounds, never wire bytes.
   std::vector<std::uint8_t> (*encode_default)(const pubsub::message&) = &pubsub::codec::encode_message;
   std::vector<std::uint8_t> (*payload_default)(const pubsub::message&) = &pubsub::codec::signing_payload;
   std::vector<std::uint8_t> (*id_default)(const pubsub::message&) = &pubsub::codec::message_id;
   void (*sign_default)(pubsub::message&, const forge::crypto::asymmetric::private_key&) = &pubsub::codec::sign_message;
   bool (*verify_default)(const pubsub::message&) = &pubsub::codec::verify_message;
   const auto plain = pubsub::message{.data = {'x'}, .subject = pubsub::topic{"t"}};
   const auto golden = std::vector<std::uint8_t>{0x12, 0x01, 'x', 0x22, 0x01, 't'};
   BOOST_CHECK(encode_default(plain) == golden);
   BOOST_CHECK(pubsub::codec::encode_message(plain, config) == golden);
   const auto prefix = std::string_view{"libp2p-pubsub:"};
   auto signing_golden = std::vector<std::uint8_t>{prefix.begin(), prefix.end()};
   signing_golden.insert(signing_golden.end(), golden.begin(), golden.end());
   BOOST_CHECK(payload_default(plain) == signing_golden);
   BOOST_CHECK(pubsub::codec::signing_payload(plain, config) == signing_golden);
   BOOST_CHECK(id_default(plain) == pubsub::codec::message_id(plain, config));
   auto short_signed = plain;
   short_signed.seqno = {1};
   BOOST_REQUIRE(source.impl_->identity.private_key);
   const auto& key = *source.impl_->identity.private_key;
   sign_default(short_signed, key);
   BOOST_CHECK(verify_default(short_signed));
   BOOST_CHECK(pubsub::codec::verify_message(short_signed, config));
   BOOST_CHECK(encode_default(short_signed) == pubsub::codec::encode_message(short_signed, config));
   BOOST_CHECK(payload_default(short_signed) == pubsub::codec::signing_payload(short_signed, config));
   BOOST_CHECK(id_default(short_signed) == pubsub::codec::message_id(short_signed, config));

   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(subject, accept));
      static_cast<void>(co_await target.async_subscribe(subject,
          [validations, subject, config, sign](pubsub::event event) -> boost::asio::awaitable<pubsub::validation_result> {
             if (event.value.subject != subject || event.value.data != std::vector<std::uint8_t>{'o'} ||
                 (sign ? !pubsub::codec::verify_message(event.value, config) : !event.value.signature.empty())) {
                co_return pubsub::validation_result::reject;
             }
             ++*validations;
             co_return pubsub::validation_result::accept;
          }));
   }());
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      {
         const auto lock = std::scoped_lock{source.impl_->mutex};
         const auto topics = source.impl_->pubsub_value.peer_topics.find(target.local_peer());
         if (topics == source.impl_->pubsub_value.peer_topics.end() || !topics->second.contains(subject.value)) {
            return false;
         }
      }
      const auto lock = std::scoped_lock{self->mutex};
      const auto topics = self->pubsub_value.peer_topics.find(source.local_peer());
      return topics != self->pubsub_value.peer_topics.end() && topics->second.contains(subject.value);
   }));
   run(fixture.runtime, source.impl_->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      {
         const auto lock = std::scoped_lock{source.impl_->mutex};
         const auto mesh = source.impl_->pubsub_value.mesh.find(subject.value);
         if (mesh == source.impl_->pubsub_value.mesh.end() || !mesh->second.contains(target.local_peer())) {
            return false;
         }
      }
      const auto lock = std::scoped_lock{self->mutex};
      const auto mesh = self->pubsub_value.mesh.find(subject.value);
      return mesh != self->pubsub_value.mesh.end() && mesh->second.contains(source.local_peer());
   })); // Real heartbeat GRAFT makes the subscribed publisher's recipient eligible.
   auto published = pubsub::message{};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      published = co_await source.async_publish(subject, {'o'}, pubsub::publish_options{.sign = sign});
   }());
   const auto id = pubsub::codec::message_id(published, config);
   BOOST_REQUIRE(fixture.wait([&] { return target.pubsub_snapshot().messages_delivered == 1U; }));
   BOOST_TEST(validations->load() == 1U);
   BOOST_TEST(target.pubsub_snapshot().messages_received == 1U);
   BOOST_TEST(target.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(target.pubsub_scores().capacity_rejections == 0U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto cache = self->pubsub_value.cache.find(bytes_key(id));
      BOOST_REQUIRE(cache != self->pubsub_value.cache.end());
      BOOST_CHECK(cache->second.subject == subject);
      BOOST_CHECK(cache->second.signature == published.signature);
      BOOST_CHECK(self->can_serve_pubsub_message_locked(bytes_key(id)));
   }
   {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      BOOST_CHECK(source.impl_->pubsub_value.cache.contains(bytes_key(id)));
   }
   auto smaller = config;
   smaller.limits.max_topic_size = 255;
   BOOST_CHECK_THROW(static_cast<void>(encode_default(published)), exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(payload_default(published)), exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::encode_message(published, smaller)), exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::signing_payload(published, smaller)), exceptions::invalid_options);
   auto unsigned_copy = published;
   unsigned_copy.signature.clear();
   BOOST_CHECK_THROW(sign_default(unsigned_copy, key), exceptions::invalid_options);
   if (sign) {
      BOOST_REQUIRE(published.from);
      BOOST_CHECK(*published.from == source.local_peer());
      BOOST_REQUIRE(!published.signature.empty());
      BOOST_CHECK(!verify_default(published));
      BOOST_CHECK(!pubsub::codec::verify_message(published, smaller));
      BOOST_CHECK(pubsub::codec::verify_message(published, config));
      BOOST_CHECK(id_default(published) == id); // Existing author+seqno fast path is unchanged.
      auto tampered = published;
      tampered.data.front() ^= 1;
      BOOST_CHECK(!pubsub::codec::verify_message(tampered, config));
   } else {
      BOOST_CHECK(!published.from);
      BOOST_CHECK(published.seqno.empty());
      BOOST_CHECK(published.signature.empty());
      BOOST_CHECK(published.key.empty());
      BOOST_TEST(id.size() == 32U); // The real public NoSign path took the configured SHA-256 fallback.
      BOOST_CHECK_THROW(static_cast<void>(id_default(published)), exceptions::invalid_options);
      BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::message_id(published, smaller)), exceptions::invalid_options);
   }
   run(fixture.runtime, self->pubsub_heartbeat_once()); // Hash cached rows again with the actual owner limits.
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::gossip_payload_bounds() {
   using queue = detail::pubsub_control_queue;
   for (const auto version : {pubsub::version::v1_0, pubsub::version::v1_1}) {
      for (const auto payload : {64U, 127U, 128U}) {
         auto config = manual_options();
         config.preferred = version;
         config.limits.max_rpc_size = payload;
         const auto id_size = payload - 6U;
         const auto id = std::vector<std::uint8_t>(id_size, 'b');
         auto cursor = pubsub::codec::gossip_cursor{};
         const auto gossip = pubsub::control{.want = {{.message_ids = {id}}}};
         const auto chunk = pubsub::codec::next_gossip(gossip, cursor, config);
         BOOST_REQUIRE(chunk);
         const auto encoded = pubsub::codec::encode(chunk->value, config);
         const auto prefix = forge::multiformats::varint_decode(encoded);
         BOOST_TEST(prefix.value == payload);
         BOOST_TEST(prefix.size == (payload == 128U ? 2U : 1U));
         BOOST_TEST(encoded.size() == payload + prefix.size);
         BOOST_TEST(chunk->wire_bytes == encoded.size());
         BOOST_REQUIRE(pubsub::codec::gossip_wire_size(gossip, config));
         BOOST_TEST(*pubsub::codec::gossip_wire_size(gossip, config) == encoded.size());
         BOOST_CHECK(!pubsub::codec::next_gossip(gossip, cursor, config));
         auto pool = queue{config.limits};
         const auto topic_size = payload - 6U;
         const auto topic = pubsub::topic{std::string(topic_size, 'g')};
         auto change = pool.prepare({{.peer = peer(71), .generation = 1, .args = {.subject = topic}}});
         BOOST_REQUIRE(change);
         pool.commit(std::move(*change));
         const auto batch = pool.acquire(peer(71), 1);
         BOOST_REQUIRE(batch);
         const auto wire = pubsub::codec::encode(queue::rpc(*batch), config);
         BOOST_TEST(forge::multiformats::varint_decode(wire).value == payload);
         BOOST_TEST(wire.size() == payload + prefix.size);
         BOOST_TEST(pool.bytes() > wire.size()); // Durable memory charge retains framing and key ownership.
         pool.finish(batch, true);
         BOOST_TEST(pool.bytes() == 0U);
      }
   }
   auto config = manual_options();
   config.limits.max_rpc_size = 64;
   auto cursor = pubsub::codec::gossip_cursor{};
   const auto first = std::vector<std::uint8_t>(32, 'a');
   const auto second = std::vector<std::uint8_t>(32, 'b');
   const auto skipped = std::vector<std::uint8_t>(100, 'x');
   const auto gossip = pubsub::control{.have = {{.subject = {std::string(80, 'x')}, .message_ids = {first}},
       {.subject = {"t"}, .message_ids = {skipped, first, second}}},
       .want = {{.message_ids = {skipped, first}}, {.message_ids = {second}}}};
   auto have = std::vector<std::vector<std::uint8_t>>{};
   auto want = std::vector<std::vector<std::uint8_t>>{};
   auto framed = std::size_t{};
   while (const auto chunk = pubsub::codec::next_gossip(gossip, cursor, config)) {
      const auto frame = pubsub::codec::encode(chunk->value, config);
      framed += frame.size();
      BOOST_TEST(forge::multiformats::varint_decode(frame).value <= 64U);
      const auto decoded = pubsub::codec::decode(frame, config);
      for (const auto& entry : decoded.control_value->have) {
         BOOST_CHECK(entry.subject == pubsub::topic{"t"});
         have.insert(have.end(), entry.message_ids.begin(), entry.message_ids.end());
      }
      for (const auto& entry : decoded.control_value->want) { want.insert(want.end(), entry.message_ids.begin(), entry.message_ids.end()); }
   }
   BOOST_CHECK(have == (std::vector<std::vector<std::uint8_t>>{first, second}));
   BOOST_CHECK(want == have);
   auto sized = std::optional<std::size_t>{};
   auto calls = std::size_t{};
   {
      auto allocation = forge::tests::p2p::pubsub_control_allocation{};
      sized = pubsub::codec::gossip_wire_size(gossip, config);
      calls = allocation.calls();
   }
   BOOST_REQUIRE(sized);
   BOOST_TEST(*sized == framed);
   BOOST_TEST(calls == 0U);
   auto pool = queue{config.limits};
   auto commands = std::vector<queue::command>{};
   for (auto index = 0U; index < 11U; ++index) {
      commands.push_back({.peer = peer(72), .generation = 1, .args = {.subject = {"g" + std::to_string(index)}}});
   }
   commands.push_back({.peer = peer(72), .generation = 1, .operation = queue::kind::prune,
       .args = {.subject = {"zzz-prune"}}});
   config.limits.max_rpc_size = 1024;
   pool = queue{config.limits};
   auto change = pool.prepare(std::move(commands));
   BOOST_REQUIRE(change);
   pool.commit(std::move(*change));
   const auto batch = pool.acquire(peer(72), 1, config.limits.max_outbound_queue_bytes);
   BOOST_REQUIRE(batch); // Unadmitted gossip cannot hide durable controls.
   BOOST_TEST(batch->ephemeral_bytes == 0U);
   const auto rpc = queue::rpc(*batch);
   BOOST_TEST(rpc.control_value->grafts.size() == 10U);
   BOOST_TEST(rpc.control_value->prunes.size() == 1U);
   pool.finish(batch, true);
   BOOST_TEST(pool.size() == 1U);
   const auto next = pool.acquire(peer(72), 1);
   BOOST_REQUIRE(next);
   BOOST_TEST(queue::rpc(*next).control_value->grafts.size() == 1U);
   pool.finish(next, true);
   BOOST_TEST(pool.bytes() == 0U);
}

void node_session_fixture::gossip_cursor_bounds() {
   auto config = manual_options();
   config.limits.max_rpc_size = 64;
   const auto id = std::vector<std::uint8_t>(32, 'x');
   const auto input = pubsub::control{.have = {{.subject = {"t"}, .message_ids = {id}}},
       .want = {{.message_ids = {id}}}};
   for (auto cursor : {pubsub::codec::gossip_cursor{.have = 2},
                       pubsub::codec::gossip_cursor{.want = 2},
                       pubsub::codec::gossip_cursor{.id = 2},
                       pubsub::codec::gossip_cursor{.have = 1, .id = 2},
                       pubsub::codec::gossip_cursor{.have = 1, .want = 1, .id = 1},
                       pubsub::codec::gossip_cursor{.want = 1}}) {
      BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::next_gossip(input, cursor, config)), exceptions::invalid_options);
   }
   auto have = pubsub::control{.have = {{.subject = {"t"}, .message_ids = {id, id}}}};
   auto cursor = pubsub::codec::gossip_cursor{};
   BOOST_REQUIRE(pubsub::codec::next_gossip(have, cursor, config));
   BOOST_TEST(cursor.id == 1U);
   have.have.front().message_ids.clear();
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::next_gossip(have, cursor, config)), exceptions::invalid_options);
   auto want = pubsub::control{.want = {{.message_ids = {id, id}}}};
   cursor = {};
   BOOST_REQUIRE(pubsub::codec::next_gossip(want, cursor, config));
   BOOST_TEST(cursor.id == 1U);
   want.want.front().message_ids.clear();
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::next_gossip(want, cursor, config)), exceptions::invalid_options);
   have = pubsub::control{.have = {{.subject = {"t"}, .message_ids = {id}}, {.subject = {"u"}, .message_ids = {id}}}};
   cursor = {};
   BOOST_REQUIRE(pubsub::codec::next_gossip(have, cursor, config));
   BOOST_TEST(cursor.have == 1U);
   have.have.clear();
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::next_gossip(have, cursor, config)), exceptions::invalid_options);
   want = pubsub::control{.want = {{.message_ids = {id}}, {.message_ids = {id}}}};
   cursor = {};
   BOOST_REQUIRE(pubsub::codec::next_gossip(want, cursor, config));
   BOOST_TEST(cursor.want == 1U);
   want.want.clear();
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::next_gossip(want, cursor, config)), exceptions::invalid_options);
}

void node_session_fixture::native_gossip_chunks(bool legacy) {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.preferred = legacy ? pubsub::version::v1_0 : pubsub::version::v1_1;
   config.signatures = pubsub::signature_policy::strict_no_sign;
   config.peer_exchange = false;
   config.limits.max_rpc_size = 64;
   auto& source = fixture.add("gossip-chunks-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& target = fixture.add("gossip-chunks-target", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   const auto self = source.impl_;
   const auto peer = target.local_peer();
   const auto protocol = pubsub::codec::protocol(config.preferred);
   const auto retired = pubsub::topic{"p"};
   const auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto reserved = false;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      release->notify();
      if (reserved) {
         self->release_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
         reserved = false;
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(retired, accept));
      static_cast<void>(co_await target.async_subscribe(retired, accept));
   }());
   const auto first = fixture.publish(source, "a");
   const auto second = fixture.publish(source, "b");
   const auto ids = std::vector<std::vector<std::uint8_t>>{
       pubsub::codec::message_id(first, config), pubsub::codec::message_id(second, config)};
   BOOST_REQUIRE(ids[0].size() == 32U && ids[1].size() == 32U);
   BOOST_CHECK(ids[0] != ids[1]);
   auto read = std::make_shared<std::promise<std::pair<node::session_info, std::vector<pubsub::rpc>>>>();
   auto received = read->get_future();
   target.register_protocol_handler(protocol,
       [read, release, epoch, config, protocol, owner = &target](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto reported = false;
          try {
             auto buffer = std::vector<std::uint8_t>{};
             auto values = std::vector<pubsub::rpc>{};
             auto advertised = std::vector<std::vector<std::uint8_t>>{};
             while (advertised.size() < 2U) {
                const auto frame = co_await async_read_length_delimited(incoming.stream, buffer, config.limits.max_rpc_size);
                auto value = pubsub::codec::decode(frame, config);
                if (value.control_value) {
                   for (const auto& have : value.control_value->have) {
                      advertised.insert(advertised.end(), have.message_ids.begin(), have.message_ids.end());
                   }
                }
                values.push_back(std::move(value));
             }
             // Reply on a distinct real inbound stream: the publisher's outbound
             // stream has no reverse RPC reader. Request/serve one ID at a time.
             auto reply = co_await owner->async_open_protocol_stream(incoming.session.remote_peer, protocol,
                 node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false});
             for (const auto& id : advertised) {
                const auto request = pubsub::codec::encode(pubsub::rpc{
                    .control_value = pubsub::control{.want = {{.message_ids = {id}}}}}, config);
                co_await reply.async_write(request);
                auto delivered = false;
                while (!delivered) {
                   const auto frame = co_await async_read_length_delimited(incoming.stream, buffer, config.limits.max_rpc_size);
                   auto value = pubsub::codec::decode(frame, config);
                   delivered = !value.messages.empty();
                   values.push_back(std::move(value));
                }
             }
             co_await reply.async_close();
             read->set_value({incoming.session, std::move(values)});
             reported = true;
             co_await release->async_wait(epoch);
          } catch (...) {
             if (!reported) { read->set_exception(std::current_exception()); }
             throw;
          }
       });
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto topics = self->pubsub_value.peer_topics.find(peer);
      const auto out = self->pubsub_value.outbound.find(peer);
      return topics != self->pubsub_value.peer_topics.end() && topics->second.size() == 2U &&
          out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0U;
   }));
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.mesh.at(retired.value).contains(peer) &&
          self->pubsub_value.controls->bytes() == 0U && self->pubsub_value.outbound_budget.total() == 0U;
   }));
   const auto failures = source.peers().find(peer)->failures;
   auto owner = std::pair<std::uint64_t, std::uint64_t>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto session = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(session);
      BOOST_CHECK(session->authentication == peer_authentication::noise);
      const auto& out = self->pubsub_value.outbound.at(peer);
      owner = {out.session_id, out.generation};
   }
   self->reserve_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
   reserved = true;
   run(fixture.runtime, source.async_unsubscribe(retired));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->pubsub_value.controls->size() != 1U) { return false; }
      // Synchronize the prior failed dispatch using the queue's ordinary lease
      // contract. Settling as unwritten preserves the same pending revision.
      const auto lease = self->pubsub_value.controls->acquire(peer, self->pubsub_value.peers.at(peer).generation);
      if (!lease) { return false; }
      self->pubsub_value.controls->finish(lease, false);
      return true;
   }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.controls->size() == 1U);
   }
   self->release_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
   reserved = false;
   // This bounded dispatcher probe uses only real locally published cache IDs;
   // the pending PRUNE was produced by public unsubscribe, not private mesh edits.
   const auto gossip = pubsub::control{.have = {{.subject = {std::string(80, 'x')}, .message_ids = {ids[0]}},
       {.subject = fixture.topic, .message_ids = {std::vector<std::uint8_t>(100, 'x'), ids[0], ids[1]}}}};
   self->flush_pubsub_controls(peer, {{peer, gossip}});
   BOOST_REQUIRE(received.wait_for(3s) == std::future_status::ready);
   const auto result = received.get();
   {
      const auto lock = std::scoped_lock{target.impl_->mutex};
      const auto session = target.impl_->sessions.find(result.first.id);
      BOOST_REQUIRE(session != target.impl_->sessions.end());
      BOOST_CHECK(session->second->authentication == peer_authentication::noise);
   }
   auto advertised = std::vector<std::vector<std::uint8_t>>{};
   auto served = std::vector<std::vector<std::uint8_t>>{};
   auto prunes = std::size_t{};
   for (const auto& value : result.second) {
      BOOST_TEST(forge::multiformats::varint_decode(pubsub::codec::encode(value, config)).value <= 64U);
      if (value.control_value) {
         for (const auto& prune : value.control_value->prunes) {
            BOOST_CHECK(prune.subject == retired);
            BOOST_CHECK(advertised.empty());
            BOOST_TEST(prune.peers.size() == 0U);
            if (legacy) { BOOST_TEST(prune.backoff.count() == 0); }
            else { BOOST_TEST(prune.backoff.count() == config.limits.unsubscribe_backoff.count()); }
            ++prunes;
         }
         for (const auto& have : value.control_value->have) {
            BOOST_CHECK(have.subject == fixture.topic);
            BOOST_TEST(have.message_ids.size() == 1U);
            advertised.insert(advertised.end(), have.message_ids.begin(), have.message_ids.end());
         }
      }
      for (const auto& message : value.messages) { served.push_back(pubsub::codec::message_id(message, config)); }
   }
   BOOST_CHECK(advertised == ids);
   BOOST_CHECK(served == ids);
   BOOST_TEST(prunes == 1U);
   auto requested = std::vector<std::vector<std::uint8_t>>{};
   for (const auto& receipt : fixture.receipts(source)) {
      if (receipt.peer != peer || receipt.frame.empty()) { continue; }
      BOOST_CHECK(receipt.protocol == protocol);
      BOOST_TEST(forge::multiformats::varint_decode(receipt.frame).value <= 64U);
      const auto value = pubsub::codec::decode(receipt.frame, config);
      if (receipt.kind == pubsub::trace_kind::rpc_read && value.control_value) {
         for (const auto& want : value.control_value->want) {
            requested.insert(requested.end(), want.message_ids.begin(), want.message_ids.end());
         }
      }
      if (receipt.kind == pubsub::trace_kind::rpc_write) {
         BOOST_TEST(receipt.session == owner.first);
         BOOST_TEST(receipt.generation == owner.second);
      }
   }
   BOOST_CHECK(requested == ids);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->bytes() == 0U;
   }));
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   BOOST_TEST(source.pubsub_snapshot().trace_failures == 0U);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_iwant_chunk_failure() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.signatures = pubsub::signature_policy::lax_sign;
   config.scoring = pubsub::scoring_params{.behaviour_penalty_weight = -1.0};
   config.limits.max_rpc_size = 64;
   auto weak = std::make_shared<std::weak_ptr<node::impl>>();
   auto armed = std::make_shared<std::atomic_bool>(true);
   auto held = std::make_shared<std::atomic_size_t>();
   config.tracer = [weak, armed, held](const pubsub::trace_event& event) {
      if (event.kind != pubsub::trace_kind::rpc_write || event.framed_rpc.empty()) { return; }
      const auto owner = weak->lock();
      if (!owner) { return; }
      const auto rpc = pubsub::codec::decode(event.framed_rpc, owner->options.limits.pubsub);
      if (!rpc.control_value || rpc.control_value->want.empty() || !armed->exchange(false)) { return; }
      auto remaining = std::size_t{};
      {
         const auto lock = std::scoped_lock{owner->mutex};
         remaining = owner->options.limits.pubsub.limits.max_outbound_queue_bytes -
             owner->pubsub_value.outbound_budget.total();
      }
      owner->reserve_pubsub_outbound_bytes(event.peer, remaining);
      held->store(remaining);
   };
   auto& source = fixture.add("iwant-chunk-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto remote = manual_options();
   remote.signatures = pubsub::signature_policy::lax_sign;
   auto& target = fixture.add("iwant-chunk-target", remote, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   const auto self = source.impl_;
   *weak = self;
   const auto peer = target.local_peer();
   const auto requested = pubsub::topic{"t"};
   const auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto inbound = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      release->notify();
      armed->store(false);
      if (const auto bytes = held->exchange(0); bytes != 0) { self->release_pubsub_outbound_bytes(peer, bytes); }
      inbound.cancel();
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(requested, accept));
      static_cast<void>(co_await target.async_subscribe(requested, accept));
   }());
   auto first = pubsub::message{};
   auto second = pubsub::message{};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      first = co_await target.async_publish(fixture.topic, {'a'}, pubsub::publish_options{.sign = false});
      second = co_await target.async_publish(requested, {'b'}, pubsub::publish_options{.sign = true});
   }());
   BOOST_REQUIRE(pubsub::codec::verify_message(second, remote));
   BOOST_REQUIRE(second.from);
   BOOST_CHECK(*second.from == target.local_peer());
   BOOST_TEST(second.seqno.size() == 8U);
   const auto first_id = pubsub::codec::message_id(first, remote);
   const auto second_id = pubsub::codec::message_id(second, remote);
   BOOST_REQUIRE(first_id.size() == 32U);
   BOOST_REQUIRE(second_id.size() > first_id.size() && second_id.size() <= 58U);
   // The genuine signed author's ID makes the second request larger. Filling
   // the remaining quota after write one leaves only write one's smaller charge.
   auto observed = std::make_shared<std::vector<std::vector<std::uint8_t>>>();
   auto mutex = std::make_shared<std::mutex>();
   auto read = std::make_shared<std::promise<void>>();
   auto received = read->get_future();
   target.register_protocol_handler(builtins::meshsub_v11,
       [observed, mutex, read, release, epoch, config](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto reported = false;
          try {
             auto buffer = std::vector<std::uint8_t>{};
             auto count = std::size_t{};
             while (count < 2U) {
                const auto frame = co_await async_read_length_delimited(incoming.stream, buffer, config.limits.max_rpc_size);
                const auto rpc = pubsub::codec::decode(frame, config);
                if (!rpc.control_value) { continue; }
                const auto lock = std::scoped_lock{*mutex};
                for (const auto& want : rpc.control_value->want) {
                   observed->insert(observed->end(), want.message_ids.begin(), want.message_ids.end());
                   count += want.message_ids.size();
                }
             }
             read->set_value();
             reported = true;
             co_await release->async_wait(epoch);
          } catch (...) {
             if (!reported) { read->set_exception(std::current_exception()); }
             throw;
          }
       });
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto out = self->pubsub_value.outbound.find(peer);
      const auto topics = self->pubsub_value.peer_topics.find(peer);
      return out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          topics != self->pubsub_value.peer_topics.end() && topics->second.contains(fixture.topic.value) &&
          topics->second.contains(requested.value) &&
          self->pubsub_value.outbound_budget.total() == 0U;
   }));
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto session = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(session);
      BOOST_CHECK(session->authentication == peer_authentication::noise);
      generation = self->pubsub_value.peers.at(peer).generation;
   }
   const auto failures = source.peers().find(peer)->failures;
   const auto rejected = source.metrics().backpressure_rejections;
   self->flush_pubsub_controls(peer, {{peer, pubsub::control{
       .want = {{.message_ids = {first_id}}, {.message_ids = {second_id}}}}}});
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return held->load() != 0U && self->pubsub_value.controls->bytes() == 0U &&
          self->pubsub_value.router->pending() == 1U;
   }));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{*mutex};
      return *observed == (std::vector<std::vector<std::uint8_t>>{first_id});
   }));
   BOOST_TEST(source.metrics().backpressure_rejections == rejected + 1U);
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(source.pubsub_snapshot().trace_failures == 0U);
   run(fixture.runtime, self->pubsub_heartbeat_once()); // Ephemeral second IWANT is not retained for retry.
   {
      const auto lock = std::scoped_lock{*mutex};
      BOOST_CHECK(*observed == (std::vector<std::vector<std::uint8_t>>{first_id}));
   }
   const auto bytes = held->exchange(0);
   self->release_pubsub_outbound_bytes(peer, bytes);
   // Actual verified foreign arrival settles the only committed promise. The
   // unsent second chunk must not leave a staged token that survives fulfillment.
   inbound = fixture.open(target, source);
   run(fixture.runtime, inbound.async_write(pubsub::codec::encode(pubsub::rpc{.messages = {first}}, config)));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.router->pending() == 0U && self->metrics_value.pubsub_messages_delivered == 1U;
   }));
   // The other production producer (direct IHAVE reply) can freshly stage the
   // formerly unsent ID after freeing capacity; no old ephemeral intent is retried.
   run(fixture.runtime, inbound.async_write(pubsub::codec::encode(pubsub::rpc{
       .control_value = pubsub::control{.have = {{.subject = requested, .message_ids = {second_id}}}}}, config)));
   BOOST_REQUIRE(received.wait_for(3s) == std::future_status::ready);
   received.get();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->bytes() == 0U && self->pubsub_value.router->pending() == 1U;
   }));
   {
      const auto lock = std::scoped_lock{*mutex};
      BOOST_CHECK(*observed == (std::vector<std::vector<std::uint8_t>>{first_id, second_id}));
   }
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(source.pubsub_snapshot().trace_failures == 0U);
   const auto scores = source.pubsub_scores();
   const auto score = std::ranges::find(scores.peers, peer, &pubsub::peer_score_snapshot::peer);
   BOOST_REQUIRE(score != scores.peers.end());
   BOOST_TEST(score->behaviour_penalty == 0.0);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == generation);
   }
   shutdown.join();
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
   }
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_iwant_origin_retirement() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.signatures = pubsub::signature_policy::strict_no_sign;
   config.peer_exchange = false;
   config.scoring = pubsub::scoring_params{.behaviour_penalty_weight = -1.0};
   auto& source = fixture.add("iwant-origin-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& target = fixture.add("iwant-origin-target", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   const auto self = source.impl_;
   const auto peer = target.local_peer();
   const auto leave = pubsub::topic{"p"};
   auto incoming = stream{};
   auto ticket = forge::asio::gate::ticket{};
   auto reserved = false;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      ticket.release();
      incoming.request_cancel();
      if (reserved) {
         self->release_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
         reserved = false;
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(leave, accept));
      static_cast<void>(co_await target.async_subscribe(leave, accept));
   }());
   const auto published = fixture.publish(target, "foreign-cache-before-iwant-owner-retirement");
   const auto id = pubsub::codec::message_id(published, config);
   const auto have = pubsub::rpc{.control_value = pubsub::control{
       .have = {{.subject = fixture.topic, .message_ids = {id}}}}};
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto out = self->pubsub_value.outbound.find(peer);
      return self->pubsub_value.peer_topics.contains(peer) && self->pubsub_value.peer_topics.at(peer).size() == 2U &&
          out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0U;
   }));
   auto old = std::shared_ptr<node::impl::session_state>{};
   auto remote_old = std::shared_ptr<node::impl::session_state>{};
   auto gate = std::shared_ptr<forge::asio::gate>{};
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      old = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(old);
      BOOST_CHECK(old->authentication == peer_authentication::noise);
      gate = self->pubsub_value.outbound.at(peer).write_gate;
      generation = self->pubsub_value.peers.at(peer).generation;
   }
   {
      const auto lock = std::scoped_lock{target.impl_->mutex};
      remote_old = target.impl_->session_for_path_locked(source.local_peer(), path::kind::direct, std::nullopt);
      BOOST_REQUIRE(remote_old);
      BOOST_CHECK(remote_old->authentication == peer_authentication::noise);
   }
   auto acquire = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquire.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   ticket = acquire.get();
   incoming = fixture.open(target, source);
   fixture.send(incoming, have);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.router->pending() == 1U && self->pubsub_value.controls->bytes() > 0U &&
          self->pubsub_value.outbound_budget.total() > 0U;
   }));
   auto read = false;
   for (const auto& receipt : fixture.receipts(source)) {
      if (receipt.kind != pubsub::trace_kind::rpc_read || receipt.frame.empty()) { continue; }
      const auto value = pubsub::codec::decode(receipt.frame, config);
      if (value.control_value && !value.control_value->have.empty()) {
         BOOST_TEST(receipt.session == old->id);
         BOOST_CHECK(value.control_value->have.front().message_ids == std::vector<std::vector<std::uint8_t>>{id});
         read = true;
      }
   }
   BOOST_REQUIRE(read);
   self->request_cancel_session(old);
   target.impl_->request_cancel_session(remote_old);
   ticket.release();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return old->closed && !self->sessions.contains(old->id) && !self->retiring_sessions.contains(old->id) &&
          self->pubsub_value.router->pending() == 0U && self->pubsub_value.controls->bytes() == 0U &&
          self->pubsub_value.outbound_budget.total() == 0U;
   }));
   BOOST_REQUIRE(fixture.wait([&] { return target.diagnostics().metrics.active_sessions == 0U; }));
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{target.impl_->mutex};
      return target.impl_->pubsub_value.peer_topics.contains(source.local_peer()) &&
          target.impl_->pubsub_value.peer_topics.at(source.local_peer()).size() == 2U;
   }));
   run(fixture.runtime, target.impl_->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto out = self->pubsub_value.outbound.find(peer);
      return self->pubsub_value.mesh.at(leave.value).contains(peer) &&
          self->pubsub_value.mesh.at(fixture.topic.value).contains(peer) &&
          out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          self->pubsub_value.controls->bytes() == 0U && self->pubsub_value.outbound_budget.total() == 0U;
   }));
   auto current = std::shared_ptr<node::impl::session_state>{};
   auto current_generation = std::uint64_t{};
   auto requested = std::size_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      current = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(current);
      BOOST_TEST(current->id != old->id);
      BOOST_CHECK(current->authentication == peer_authentication::noise);
      current_generation = self->pubsub_value.peers.at(peer).generation;
      BOOST_TEST(current_generation > generation);
      requested = self->pubsub_value.peers.at(peer).requested;
   }
   self->reserve_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
   reserved = true;
   run(fixture.runtime, source.async_unsubscribe(leave));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->pubsub_value.controls->size() != 1U) { return false; }
      const auto lease = self->pubsub_value.controls->acquire(peer, current_generation);
      if (!lease) { return false; }
      self->pubsub_value.controls->finish(lease, false);
      return true;
   }));
   const auto failures = source.peers().find(peer)->failures;
   const auto before = source.pubsub_snapshot();
   self->release_pubsub_outbound_bytes(peer, config.limits.max_outbound_queue_bytes);
   reserved = false;
   // Resume the deferred dispatch entry derived from the real G1 IHAVE. It must
   // retain its original physical owner, even though G2 now owns the peer lifetime.
   self->flush_pubsub_controls(peer, {{peer, pubsub::control{.want = {{.message_ids = {id}}}}}}, old);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->size() == 0U && self->pubsub_value.controls->bytes() == 0U &&
          self->pubsub_value.outbound_budget.total() == 0U;
   }));
   BOOST_TEST(controls(fixture, source, peer, false) == 1U); // Independent current PRUNE still writes.
   auto wants = std::size_t{};
   for (const auto& receipt : fixture.receipts(source)) {
      if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.frame.empty()) { continue; }
      const auto value = pubsub::codec::decode(receipt.frame, config);
      if (value.control_value) {
         for (const auto& want : value.control_value->want) { wants += want.message_ids.size(); }
         if (!value.control_value->prunes.empty()) { BOOST_TEST(receipt.session == current->id); }
      }
   }
   BOOST_TEST(wants == 0U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->sessions.at(current->id) == current);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == current_generation);
      BOOST_TEST(self->pubsub_value.peers.at(peer).requested == requested);
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
      BOOST_CHECK(self->pubsub_value.mesh.at(fixture.topic.value).contains(peer));
   }
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == before.invalid_messages);
   BOOST_TEST(source.pubsub_snapshot().messages_received == before.messages_received);
   BOOST_TEST(source.pubsub_snapshot().messages_delivered == before.messages_delivered);
   const auto scores = source.pubsub_scores();
   const auto score = std::ranges::find(scores.peers, peer, &pubsub::peer_score_snapshot::peer);
   BOOST_REQUIRE(score != scores.peers.end());
   BOOST_TEST(score->behaviour_penalty == 0.0);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_cached_message_frames() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.signatures = pubsub::signature_policy::strict_no_sign;
   config.peer_exchange = false;
   config.limits.max_rpc_size = 128;
   auto& source = fixture.add("cached-frames-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& target = fixture.add("cached-frames-target", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   const auto peer = target.local_peer();
   const auto topic = pubsub::topic{"t"};
   const auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto request = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      release->notify();
      request.request_cancel();
   }};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(topic, accept));
      static_cast<void>(co_await target.async_subscribe(topic, accept));
   }());
   auto messages = std::vector<pubsub::message>{};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      messages.push_back(co_await source.async_publish(topic, std::vector<std::uint8_t>(80, 'a')));
      messages.push_back(co_await source.async_publish(topic, std::vector<std::uint8_t>(80, 'b')));
   }());
   const auto ids = std::vector<std::vector<std::uint8_t>>{
       pubsub::codec::message_id(messages[0], config), pubsub::codec::message_id(messages[1], config)};
   BOOST_CHECK(ids[0] != ids[1]);
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::encode(pubsub::rpc{.messages = messages}, config)),
                     exceptions::invalid_options);
   for (const auto& message : messages) {
      BOOST_TEST(forge::multiformats::varint_decode(pubsub::codec::encode(pubsub::rpc{.messages = {message}}, config)).value <= 128U);
   }
   auto read = std::make_shared<std::promise<std::pair<node::session_info, std::vector<pubsub::message>>>>();
   auto received = read->get_future();
   target.register_protocol_handler(builtins::meshsub_v11,
       [read, release, epoch, config](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto reported = false;
          try {
             auto buffer = std::vector<std::uint8_t>{};
             auto messages = std::vector<pubsub::message>{};
             while (messages.size() < 2U) {
                const auto frame = co_await async_read_length_delimited(incoming.stream, buffer, config.limits.max_rpc_size);
                auto value = pubsub::codec::decode(frame, config);
                if (value.messages.empty()) { continue; }
                if (value.messages.size() != 1U) {
                   FORGE_THROW_EXCEPTION(exceptions::protocol_error, "cached reply must contain one bounded MESSAGE");
                }
                messages.push_back(std::move(value.messages.front()));
             }
             read->set_value({incoming.session, std::move(messages)});
             reported = true;
             co_await release->async_wait(epoch);
          } catch (...) {
             if (!reported) { read->set_exception(std::current_exception()); }
             throw;
          }
       });
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      const auto out = source.impl_->pubsub_value.outbound.find(peer);
      return source.impl_->pubsub_value.peer_topics.contains(peer) &&
          source.impl_->pubsub_value.peer_topics.at(peer).contains(topic.value) &&
          out != source.impl_->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          source.impl_->pubsub_value.outbound_budget.total() == 0U;
   }));
   const auto failures = source.peers().find(peer)->failures;
   request = fixture.open(target, source);
   run(fixture.runtime, request.async_write(pubsub::codec::encode(pubsub::rpc{
       .control_value = pubsub::control{.want = {{.message_ids = ids}}}}, config)));
   BOOST_REQUIRE(received.wait_for(3s) == std::future_status::ready);
   const auto result = received.get();
   BOOST_CHECK(result.first.remote_peer == source.local_peer());
   {
      const auto lock = std::scoped_lock{target.impl_->mutex};
      const auto owner = target.impl_->sessions.find(result.first.id);
      BOOST_REQUIRE(owner != target.impl_->sessions.end());
      BOOST_CHECK(owner->second->authentication == peer_authentication::noise);
   }
   auto served = std::vector<std::vector<std::uint8_t>>{};
   for (const auto& message : result.second) {
      served.push_back(pubsub::codec::message_id(message, config));
      BOOST_CHECK(message.subject == topic);
      BOOST_TEST(message.data.size() == 80U);
      BOOST_CHECK(!message.from);
      BOOST_CHECK(message.seqno.empty() && message.signature.empty() && message.key.empty());
   }
   BOOST_CHECK(served == ids);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      return source.impl_->pubsub_value.outbound_budget.total() == 0U;
   }));
   auto replies = std::size_t{};
   auto requested = std::vector<std::vector<std::uint8_t>>{};
   for (const auto& receipt : fixture.receipts(source)) {
      if (receipt.peer != peer || receipt.frame.empty()) { continue; }
      const auto value = pubsub::codec::decode(receipt.frame, config);
      BOOST_TEST(forge::multiformats::varint_decode(receipt.frame).value <= 128U);
      if (receipt.kind == pubsub::trace_kind::rpc_write && !value.messages.empty()) {
         BOOST_TEST(value.messages.size() == 1U);
         ++replies;
      }
      if (receipt.kind == pubsub::trace_kind::rpc_read && value.control_value) {
         for (const auto& want : value.control_value->want) {
            requested.insert(requested.end(), want.message_ids.begin(), want.message_ids.end());
         }
      }
   }
   BOOST_CHECK(requested == ids);
   BOOST_TEST(replies == 2U);
   BOOST_TEST(source.peers().find(peer)->failures == failures);
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(source.pubsub_snapshot().trace_failures == 0U);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_graft_batches(bool inbound) {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   if (!inbound) {
      config.limits.heartbeat_initial_delay = 1s;
      config.limits.heartbeat_interval = 100ms;
   }
   auto& source = fixture.add("graft-batch-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& target = fixture.add("graft-batch-target", manual_options(), {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto channel = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] { channel.cancel(); }};
   auto grafts = std::vector<pubsub::control::graft>{};
   for (auto index = 0U; index < 11U; ++index) {
      const auto topic = pubsub::topic{"forge.graft.batch." + std::to_string(index)};
      run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
         const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
            co_return pubsub::validation_result::accept;
         };
         static_cast<void>(co_await source.async_subscribe(topic, accept));
         static_cast<void>(co_await target.async_subscribe(topic, accept));
      }());
      grafts.push_back({.subject = topic});
   }
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      return source.impl_->pubsub_value.peer_topics.contains(target.local_peer()) &&
          source.impl_->pubsub_value.peer_topics.at(target.local_peer()).size() == 11U;
   }));
   if (inbound) {
      channel = fixture.open(source, target);
      fixture.send(channel, pubsub::rpc{.control_value = pubsub::control{.grafts = grafts}});
   }
   BOOST_REQUIRE(fixture.wait([&] { return target.pubsub_snapshot().mesh_edges == 11U; }));
   auto sizes = std::vector<std::size_t>{};
   auto topics = std::set<std::string>{};
   const auto kind = inbound ? pubsub::trace_kind::rpc_read : pubsub::trace_kind::rpc_write;
   const auto& owner = inbound ? target : source;
   BOOST_REQUIRE(fixture.wait([&] {
      sizes.clear();
      topics.clear();
      for (const auto& event : fixture.receipts(owner)) {
         if (event.kind != kind || event.frame.empty()) { continue; }
         const auto rpc = pubsub::codec::decode(event.frame, config);
         if (!rpc.control_value || rpc.control_value->grafts.empty()) { continue; }
         sizes.push_back(rpc.control_value->grafts.size());
         for (const auto& graft : rpc.control_value->grafts) { topics.insert(graft.subject.value); }
      }
      return topics.size() == 11U;
   }));
   BOOST_CHECK(sizes == (inbound ? std::vector<std::size_t>{11} : std::vector<std::size_t>{10, 1}));
   BOOST_TEST(target.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_CASE(control_native_heartbeat_quota_retry_is_neutral) { forge::net::p2p::node_session_fixture::native_retry(false); }
BOOST_AUTO_TEST_CASE(control_native_rejected_graft_quota_retry_preserves_backoff) { forge::net::p2p::node_session_fixture::native_retry(true); }
BOOST_AUTO_TEST_CASE(control_native_v10_prune_retry_preserves_legacy_wire) { forge::net::p2p::node_session_fixture::native_retry(true, true); }
BOOST_AUTO_TEST_CASE(control_native_unsubscribe_batch_refusal_preserves_all_state) { forge::net::p2p::node_session_fixture::native_unsubscribe_rollback(); }
BOOST_AUTO_TEST_CASE(control_native_unsubscribe_after_stop_is_local_and_retains_active_callback_join) { forge::net::p2p::node_session_fixture::native_unsubscribe_after_stop(); }
BOOST_AUTO_TEST_CASE(control_owner_preparation_allocation_rollback_preserves_pending_mesh_and_backoff) { forge::net::p2p::node_session_fixture::control_preparation_allocation_rollback(); }
BOOST_AUTO_TEST_CASE(control_native_heartbeat_allocation_refusal_resumes_original_loop_and_repairs_mesh) { forge::net::p2p::node_session_fixture::native_heartbeat_allocation_recovery(); }
BOOST_AUTO_TEST_CASE(control_native_neutral_scoring_accepts_owner_wire_topic_and_message_id_bounds) { forge::net::p2p::node_session_fixture::native_neutral_wire_topic(); }
BOOST_AUTO_TEST_CASE(control_native_signed_publish_uses_owner_codec_limits_before_validation_and_delivery) { forge::net::p2p::node_session_fixture::native_owned_codec_topic(true); }
BOOST_AUTO_TEST_CASE(control_native_nosign_publish_hash_uses_owner_codec_limits_before_validation_and_delivery) { forge::net::p2p::node_session_fixture::native_owned_codec_topic(false); }
BOOST_AUTO_TEST_CASE(control_gossip_payload_prefix_bounds_and_prune_after_graft_quota) { forge::net::p2p::node_session_fixture::gossip_payload_bounds(); }
BOOST_AUTO_TEST_CASE(control_gossip_cursor_rejects_out_of_range_and_shrunk_input) { forge::net::p2p::node_session_fixture::gossip_cursor_bounds(); }
BOOST_AUTO_TEST_CASE(control_native_v11_gossip_chunks_skip_oversize_and_serve_real_cache_after_pending_prune) { forge::net::p2p::node_session_fixture::native_gossip_chunks(false); }
BOOST_AUTO_TEST_CASE(control_native_v10_gossip_chunks_skip_oversize_and_serve_real_cache_after_pending_prune) { forge::net::p2p::node_session_fixture::native_gossip_chunks(true); }
BOOST_AUTO_TEST_CASE(control_native_iwant_second_chunk_refusal_keeps_only_written_promise_and_allows_fresh_reply) { forge::net::p2p::node_session_fixture::native_iwant_chunk_failure(); }
BOOST_AUTO_TEST_CASE(control_native_iwant_retired_physical_origin_cannot_stage_on_g2_but_current_prune_flushes) { forge::net::p2p::node_session_fixture::native_iwant_origin_retirement(); }
BOOST_AUTO_TEST_CASE(control_native_iwant_cached_messages_use_individual_bounded_rpc_frames) { forge::net::p2p::node_session_fixture::native_cached_message_frames(); }
BOOST_AUTO_TEST_CASE(control_native_one_rpc_all_eleven_inbound_grafts_are_processed) { forge::net::p2p::node_session_fixture::native_graft_batches(true); }
BOOST_AUTO_TEST_CASE(control_native_outbound_graft_quota_converges_naturally_ten_plus_one) { forge::net::p2p::node_session_fixture::native_graft_batches(false); }
