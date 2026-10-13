module;

#include <forge/exceptions/macros.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

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
import forge.net.p2p.identify;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.options;
import forge.net.yamux.session;

#include "../../../libraries/net/p2p/details/node_impl.hxx"
#include "../../../libraries/net/p2p/details/length_delimited.hxx"
#include "../../../libraries/net/p2p/details/pubsub_peer_score.hxx"
#include "../../../libraries/net/p2p/details/pubsub_router.hxx"
#include "../../../libraries/net/p2p/details/resource_stream.hxx"
#include "../pubsub_router_fixture.hxx"
#include "../gossipsub_test_shutdown.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;
using forge::tests::p2p::pubsub_router_fixture;
using forge::tests::p2p::gossipsub_test_shutdown;

void node_session_fixture::native_pre_io_stream_quota() {
   auto fixture = pubsub_router_fixture{};
   // Keep the original router-test profile; only its setup observation moves
   // into the existing friend owner so Identify and Push can actually settle.
   auto common = pubsub::options{};
   common.limits.mesh_n = 2;
   common.limits.mesh_n_low = 1;
   common.limits.mesh_n_high = 4;
   common.limits.mesh_outbound_min = 0;
   common.limits.mesh_score_min = 1;
   common.limits.heartbeat_initial_delay = 40ms;
   common.limits.heartbeat_interval = 80ms;
   common.limits.prune_backoff = 1s;
   common.limits.iwant_followup_time = 160ms;
   common.scoring.emplace();
   auto topic_params = pubsub::topic_score_params{};
   topic_params.invalid_message_deliveries_weight = -100;
   common.scoring->topics.emplace(fixture.topic, topic_params);
   common.scoring->retain_score = 5s;
   auto config = common;
   config.limits.heartbeat_initial_delay = 30s;
   auto& params = config.scoring->topics.at(fixture.topic);
   params.mesh_failure_penalty_weight = -1;
   params.mesh_message_deliveries_activation = 1s;
   params.mesh_message_deliveries_threshold = 1;
   auto resources = resource_manager::limits{};
   resources.peer.max_outbound_streams = 1;
   auto& owner = fixture.add("pubsub-quota-owner", std::move(config), resources);
   auto& remote = fixture.add("pubsub-quota-peer", std::move(common));
   const auto owner_peer = owner.local_peer();
   const auto remote_peer = remote.local_peer();
   auto held = stream{};
   auto input = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, [&] {
      held.request_cancel();
      input.request_cancel();
   }};
   auto stage = "connect";
   struct observation {
      std::uint64_t session = 0;
      bool open = false;
      bool identified = false;
      bool identify_completed = false;
      bool push_supported = false;
      bool push_running = false;
      std::uint64_t attempted_generation = 0;
      std::uint64_t generation = 0;
      resource_manager::snapshot resources;
   };
   auto observed = std::array<observation, 2>{};
   const auto observe_locked = [](node& value, const peer_id& peer) {
      auto out = observation{};
      const auto session = value.impl_->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      if (session) {
         out.session = session->id;
         out.open = !session->closed;
         out.identified = session->info.identify_state == identify::state::identified;
         out.identify_completed = session->identify_completed;
         out.push_supported = session->identify_push_supported;
         out.attempted_generation = session->identify_push_attempted_generation;
      }
      out.push_running = value.impl_->identify_push_value.coordinator_running;
      out.generation = value.impl_->identify_push_value.generation;
      out.resources = value.impl_->resources.current();
      return out;
   };
   const auto settled = [](const observation& value) {
      const auto no_streams = [](const resource_manager::scope_totals& totals) {
         return totals.inbound_streams == 0U && totals.outbound_streams == 0U;
      };
      return value.open && value.identified && value.identify_completed && value.push_supported && !value.push_running &&
          value.attempted_generation >= value.generation &&
          no_streams(value.resources.system) && no_streams(value.resources.streams) &&
          no_streams(value.resources.transient);
   };
   const auto observe_both = [&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex, remote.impl_->mutex};
      observed = {observe_locked(owner, remote_peer), observe_locked(remote, owner_peer)};
      return settled(observed[0]) && settled(observed[1]);
   };
   const auto diagnose = [&](const char* snapshot) {
      for (auto index = std::size_t{}; index < observed.size(); ++index) {
         const auto& value = observed[index];
         const auto& peer = index == 0 ? remote_peer : owner_peer;
         std::fprintf(stderr,
             "pubsub-quota stage=%s snapshot=%s owner=%s peer=%s session=%llu open=%d identified=%d completed=%d "
             "push-supported=%d push-running=%d attempted-generation=%llu current-generation=%llu "
             "system-in=%zu system-out=%zu streams-in=%zu streams-out=%zu transient-in=%zu transient-out=%zu "
             "denied-streams=%llu\n",
             stage, snapshot, index == 0 ? "owner" : "remote", peer.to_string().c_str(),
             static_cast<unsigned long long>(value.session), value.open, value.identified, value.identify_completed,
             value.push_supported, value.push_running, static_cast<unsigned long long>(value.attempted_generation),
             static_cast<unsigned long long>(value.generation), value.resources.system.inbound_streams,
             value.resources.system.outbound_streams, value.resources.streams.inbound_streams,
             value.resources.streams.outbound_streams, value.resources.transient.inbound_streams,
             value.resources.transient.outbound_streams, static_cast<unsigned long long>(value.resources.denied_streams));
      }
      std::fflush(stderr);
   };
   const auto score_row = [&]() -> std::optional<pubsub::peer_score_snapshot> {
      for (const auto& row : owner.pubsub_scores().peers) {
         if (row.peer == remote_peer) { return row; }
      }
      return std::nullopt;
   };
   try {
      fixture.connect(remote, owner);
      stage = "both-native-identify-push-quiescent-before-held-open";
      // Observe both native Identify/Push owners under their mutexes, including
      // current stream reservations. No generation rewrite, quota injection,
      // disabled Identify/Push, or failed-open retry.
      BOOST_REQUIRE_MESSAGE(fixture.wait(observe_both), "both nodes did not settle native Identify/Push before quota setup");
      stage = "held-native-open";
      held = fixture.open(owner, remote);
      stage = "inbound-native-open";
      input = fixture.open(remote, owner);
      stage = "subscriptions-and-native-graft";
      fixture.subscribe(owner);
      fixture.subscribe(remote);
      fixture.send(input, pubsub::rpc{.subscriptions = {{.subscribe = true, .subject = fixture.topic}},
          .control_value = pubsub::control{.grafts = {{.subject = fixture.topic}}}});
      BOOST_REQUIRE(fixture.wait([&] {
         const auto row = score_row();
         return row && !row->topics.empty() && row->topics.front().in_mesh && row->topics.front().mesh_deliveries_active;
      }));
      BOOST_REQUIRE_EQUAL(owner.diagnostics().resources.system.outbound_streams, 1U);
      stage = "three-native-pre-io-quota-rejections";
      const auto baseline = owner.metrics().backpressure_rejections;
      for (auto attempt = 0; attempt < 3; ++attempt) {
         auto rejected = false;
         try { static_cast<void>(fixture.publish(owner, "quota-pressure-" + std::to_string(attempt))); }
         catch (const forge::exceptions::base& error) {
            rejected = exceptions::is(error, exceptions::code::backpressure_rejected);
            BOOST_CHECK(rejected);
         }
         BOOST_REQUIRE(rejected);
         const auto row = score_row();
         BOOST_REQUIRE(row);
         BOOST_REQUIRE_EQUAL(row->topics.size(), 1U);
         BOOST_TEST(row->topics.front().in_mesh);
         BOOST_TEST(row->topics.front().mesh_message_deliveries == 0.0);
         BOOST_TEST(row->topics.front().mesh_failure_penalty == 0.0);
         BOOST_TEST(row->topics.front().invalid_message_deliveries == 0.0);
         BOOST_TEST(row->behaviour_penalty == 0.0);
      }
      BOOST_TEST(owner.metrics().backpressure_rejections >= baseline + 3U);
      BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
      BOOST_TEST(std::ranges::none_of(fixture.receipts(owner), [](const auto& event) {
         return event.kind == pubsub::trace_kind::rpc_write;
      })); // No outbound GossipSub stream was opened or written.
      stage = "held-native-close";
      run(fixture.runtime, held.async_close());
      BOOST_REQUIRE(fixture.wait([&] { return owner.diagnostics().resources.system.outbound_streams == 0U; }));
      stage = "native-publication-recovery";
      static_cast<void>(fixture.publish(owner, "quota-recovered"));
      BOOST_REQUIRE(fixture.wait([&] {
         return std::ranges::any_of(fixture.receipts(remote), [](const auto& event) {
            return event.kind == pubsub::trace_kind::delivery && std::ranges::equal(event.data, std::string_view{"quota-recovered"});
         });
      }));
      const auto recovered = score_row();
      BOOST_REQUIRE(recovered);
      BOOST_REQUIRE_EQUAL(recovered->topics.size(), 1U);
      BOOST_TEST(recovered->topics.front().in_mesh);
      stage = "native-input-close";
      run(fixture.runtime, input.async_close());
      stage = "joined-cleanup";
      shutdown.join();
   } catch (...) {
      // Retain the barrier snapshot and collect the actual current generation /
      // reservations too, without calling diagnostics while holding node mutexes.
      diagnose("last-setup-predicate");
      static_cast<void>(observe_both());
      diagnose("current");
      throw;
   }
}

void node_session_fixture::native_gate_supersession() {
   auto fixture = pubsub_router_fixture{};
   const auto samples = std::make_shared<std::atomic_size_t>(0);
   auto config = manual_options();
   config.scoring = pubsub::scoring_params{.app_specific_score = [samples](const peer_id&) {
      return static_cast<double>(++*samples);
   }, .app_specific_weight = 1.0};
   auto& source = fixture.add("control-gate-source", config);
   auto& blocked = fixture.add("control-gate-blocked", manual_options());
   auto& healthy = fixture.add("control-gate-healthy", manual_options());
   const auto self = source.impl_;
   auto ticket = forge::asio::gate::ticket{};
   auto unsubscribe = std::future<void>{};
   auto release = [&] { ticket.release(); };
   auto healthy_shutdown = gossipsub_test_shutdown{fixture.runtime, source, healthy, release};
   auto blocked_shutdown = gossipsub_test_shutdown{fixture.runtime, source, blocked, release, [&](auto deadline) {
      if (unsubscribe.valid() && unsubscribe.wait_until(deadline) != std::future_status::ready) {
         gossipsub_test_shutdown::fail_closed();
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(blocked);
   fixture.subscribe(healthy);
   fixture.connect(source, blocked);
   fixture.connect(source, healthy);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.size() == 2U && self->pubsub_value.outbound.size() == 2U &&
          std::ranges::all_of(self->pubsub_value.outbound, [](const auto& row) { return !row.second.snapshot_pending; });
   }));
   auto gate = std::shared_ptr<forge::asio::gate>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      gate = self->pubsub_value.outbound.at(blocked.local_peer()).write_gate;
   }
   auto acquire = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquire.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   ticket = acquire.get();
   const auto before_samples = samples->load();
   auto tick = boost::asio::co_spawn(fixture.runtime.context(), self->pubsub_heartbeat_once(), boost::asio::use_future);
   const auto nonblocking = tick.wait_for(250ms) == std::future_status::ready;
   if (!nonblocking) { ticket.release(); }
   if (tick.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   tick.get();
   BOOST_REQUIRE(nonblocking);
   BOOST_REQUIRE(fixture.wait([&] { return controls(fixture, source, healthy.local_peer(), true) == 1U; }));
   BOOST_TEST(controls(fixture, source, blocked.local_peer(), true) == 0U);
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_TEST(samples->load() >= before_samples + 4U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.epoch == 2U);
   }
   // The genuine public producer commits a later PRUNE while the older GRAFT waits on the native gate.
   unsubscribe = boost::asio::co_spawn(fixture.runtime.context(), source.async_unsubscribe(fixture.topic), boost::asio::use_future);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return !self->pubsub_value.handlers.contains(fixture.topic.value) && self->pubsub_value.controls->size() >= 2U;
   }));
   ticket.release();
   if (unsubscribe.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   unsubscribe.get();
   BOOST_REQUIRE(fixture.wait([&] {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      return controls(fixture, source, blocked.local_peer(), false) == 1U;
   }));
   BOOST_TEST(controls(fixture, source, blocked.local_peer(), true) == 0U);
   BOOST_TEST(source.pubsub_snapshot().mesh_edges == 0U);
   blocked_shutdown.join();
   healthy_shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_snapshot_supersession() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.limits.max_topic_size = 1024;
   auto& source = fixture.add("control-snapshot-source", config);
   auto& target = fixture.add("control-snapshot-target", config);
   const auto self = source.impl_;
   auto barrier = std::make_shared<forge::asio::notification>();
   const auto epoch = barrier->epoch();
   auto entered = std::make_shared<std::promise<void>>();
   auto accepted = entered->get_future();
   auto read = std::make_shared<std::promise<std::vector<pubsub::rpc>>>();
   auto frames = read->get_future();
   auto unsubscribe = std::future<void>{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [barrier] { barrier->notify(); },
       [&](auto deadline) {
          if (unsubscribe.valid() && unsubscribe.wait_until(deadline) != std::future_status::ready) {
             gossipsub_test_shutdown::fail_closed();
          }
       }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   for (auto i = std::size_t{}; i < 384; ++i) {
      run(fixture.runtime, [&source, i]() -> boost::asio::awaitable<void> {
         static_cast<void>(co_await source.async_subscribe(pubsub::topic{
             "snapshot." + std::to_string(i) + std::string(1000, 'x')},
             [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
                co_return pubsub::validation_result::accept;
             }));
      }());
   }
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(target.local_peer()) &&
          self->pubsub_value.outbound.contains(target.local_peer()) &&
          !self->pubsub_value.outbound.at(target.local_peer()).snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0;
   }));
   target.register_protocol_handler(builtins::meshsub_v11,
       [barrier, epoch, entered, read, config](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          try {
             entered->set_value();
             co_await barrier->async_wait(epoch);
             auto buffer = std::vector<std::uint8_t>{};
             auto values = std::vector<pubsub::rpc>{};
             for (auto i = 0; i < 3; ++i) {
                auto frame = co_await async_read_length_delimited(incoming.stream, buffer, config.limits.max_rpc_size);
                values.push_back(pubsub::codec::decode(frame, config));
             }
             co_await incoming.stream.async_close();
             read->set_value(std::move(values));
          } catch (...) {
             read->set_exception(std::current_exception());
             throw;
          }
       });
   {
      const auto lock = std::scoped_lock{self->mutex};
      // Replace only the idle cached stream before the experiment, never an active data write.
      self->invalidate_pubsub_outbound_locked(target.local_peer());
   }
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_REQUIRE(accepted.wait_for(3s) == std::future_status::ready);
   accepted.get();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto out = self->pubsub_value.outbound.find(target.local_peer());
      return out != self->pubsub_value.outbound.end() && out->second.stream && out->second.snapshot_pending &&
          self->pubsub_value.outbound_budget.total() > forge::net::yamux::options{}.initial_window;
   }));
   // The native Yamux receiver has not read the >256 KiB snapshot, so its write is genuinely suspended.
   unsubscribe = boost::asio::co_spawn(fixture.runtime.context(), source.async_unsubscribe(fixture.topic), boost::asio::use_future);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return !self->pubsub_value.handlers.contains(fixture.topic.value) && self->pubsub_value.controls->size() == 2U;
   }));
   run(fixture.runtime, self->pubsub_heartbeat_once());
   barrier->notify();
   if (unsubscribe.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   unsubscribe.get();
   BOOST_REQUIRE(fixture.wait([&] {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      return controls(fixture, source, target.local_peer(), false) == 1U;
   }));
   BOOST_REQUIRE(frames.wait_for(3s) == std::future_status::ready);
   const auto received = frames.get();
   BOOST_TEST(received.front().subscriptions.size() == 385U);
   BOOST_TEST(controls(fixture, source, target.local_peer(), true) == 0U);
   auto grafts = std::size_t{};
   auto prunes = std::size_t{};
   for (const auto& rpc : received) {
      if (!rpc.control_value) { continue; }
      grafts += rpc.control_value->grafts.size();
      prunes += rpc.control_value->prunes.size();
   }
   BOOST_TEST(grafts == 0U);
   BOOST_TEST(prunes == 1U);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_dispatch_request(bool stop) {
   auto fixture = pubsub_router_fixture{};
   auto& source = fixture.add("control-request-source", manual_options());
   auto& target = fixture.add("control-request-target", manual_options());
   const auto self = source.impl_;
   auto ticket = forge::asio::gate::ticket{};
   auto inbound = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      ticket.release();
      inbound.cancel();
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   // Publish before connecting so only the explicit, independently authenticated
   // inbound MESSAGE below can deliver this signed remote message to the source.
   const auto published = fixture.publish(target, "verified-remote-arrival");
   BOOST_CHECK(pubsub::codec::verify_message(published));
   const auto id = pubsub::codec::message_id(published);
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.outbound.contains(target.local_peer()) &&
          !self->pubsub_value.outbound.at(target.local_peer()).snapshot_pending;
   }));
   auto gate = std::shared_ptr<forge::asio::gate>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      gate = self->pubsub_value.outbound.at(target.local_peer()).write_gate;
   }
   auto acquire = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquire.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   ticket = acquire.get();
   self->flush_pubsub_controls(std::nullopt, {{target.local_peer(), pubsub::control{.want = {{.message_ids = {id}}}}}});
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.router->pending() == 1U;
   }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
   }
   if (stop) {
      // A staged request belongs to the dispatch, not to the heartbeat's stack or a future retry.
      source.request_stop();
      run(fixture.runtime, source.async_stop());
   } else {
      // A distinct native inbound stream reaches signature verification and the
      // production claim while the older outbound IWANT still cannot acquire its gate.
      inbound = fixture.open(target, source);
      fixture.send(inbound, pubsub::rpc{.messages = {published}});
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{self->mutex};
         return self->pubsub_value.router->pending() == 0U && self->pubsub_value.cache.contains(bytes_key(id)) &&
             self->can_serve_pubsub_message_locked(bytes_key(id)) && self->metrics_value.pubsub_messages_delivered == 1U;
      }));
      BOOST_TEST(source.pubsub_snapshot().messages_delivered == 1U);
      BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
      ticket.release();
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{self->mutex};
         return self->pubsub_value.controls->bytes() == 0;
      }));
      auto requests = std::size_t{};
      for (const auto& receipt : fixture.receipts(source)) {
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != target.local_peer() || receipt.frame.empty()) {
            continue;
         }
         const auto rpc = pubsub::codec::decode(receipt.frame, self->options.limits.pubsub);
         if (!rpc.control_value) { continue; }
         for (const auto& want : rpc.control_value->want) {
            if (std::ranges::find(want.message_ids, id) != want.message_ids.end()) { ++requests; }
         }
      }
      BOOST_TEST(requests == 1U);
      run(fixture.runtime, inbound.async_close());
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
      BOOST_CHECK(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
   }
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_control_failure_generation(bool stale) {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.limits.max_topic_size = 1024;
   auto& source = fixture.add("control-failure-source", config);
   auto& target = fixture.add("control-failure-target", config);
   const auto self = source.impl_;
   const auto peer = target.local_peer();
   const auto barrier = std::make_shared<forge::asio::notification>();
   const auto epoch = barrier->epoch();
   const auto entered = std::make_shared<std::promise<void>>();
   auto accepted = entered->get_future();
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [barrier] { barrier->notify(); }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   for (auto i = std::size_t{}; i < 384; ++i) {
      run(fixture.runtime, [&source, i]() -> boost::asio::awaitable<void> {
         static_cast<void>(co_await source.async_subscribe(pubsub::topic{
             "failure.snapshot." + std::to_string(i) + std::string(990, 'x')},
             [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
                co_return pubsub::validation_result::accept;
             }));
      }());
   }
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(peer) && self->pubsub_value.outbound.contains(peer) &&
          !self->pubsub_value.outbound.at(peer).snapshot_pending && self->pubsub_value.outbound_budget.total() == 0;
   }));
   const auto failures = source.peers().find(peer)->failures;
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      generation = self->pubsub_value.peers.at(peer).generation;
      self->invalidate_pubsub_outbound_locked(peer);
   }
   target.register_protocol_handler(builtins::meshsub_v11,
       [barrier, epoch, entered](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          entered->set_value();
          co_await barrier->async_wait(epoch);
          incoming.stream.cancel();
       });
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_REQUIRE(accepted.wait_for(3s) == std::future_status::ready);
   accepted.get();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      // The budget is reserved before negotiation. Inject RESET only after the
      // stream is published, so this tests write failure rather than failed open.
      return self->pubsub_value.outbound.contains(peer) && self->pubsub_value.outbound.at(peer).snapshot_pending &&
          self->pubsub_value.outbound.at(peer).stream &&
          self->pubsub_value.outbound_budget.total() > forge::net::yamux::options{}.initial_window;
   }));
   if (stale) {
      const auto lock = std::scoped_lock{self->mutex};
      // Exercise the real owner lifetime transition deterministically before
      // releasing a lower native RESET; this is not a second network-handshake claim.
      self->forget_pubsub_peer_locked(peer);
      BOOST_REQUIRE(self->connect_pubsub_peer_locked(peer));
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation > generation);
   }
   barrier->notify();
   auto remaining_bytes = std::size_t{};
   auto remaining_controls = std::size_t{};
   auto observed_failures = failures;
   const auto settled = fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      remaining_bytes = self->pubsub_value.outbound_budget.total();
      remaining_controls = self->pubsub_value.controls->bytes();
      observed_failures = self->store.find(peer)->failures;
      return remaining_bytes == 0 && (stale ? remaining_controls == 0 : observed_failures == failures + 1U);
   });
   BOOST_REQUIRE_MESSAGE(settled, "control failure did not settle: bytes=" << remaining_bytes
       << " controls=" << remaining_controls << " failures=" << observed_failures << " baseline=" << failures);
   BOOST_TEST(source.peers().find(peer)->failures == failures + (stale ? 0U : 1U));
   BOOST_TEST(controls(fixture, source, peer, true) == 0U);
   if (!stale) {
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->disconnect_pubsub_peer_locked(peer, std::chrono::steady_clock::now());
         BOOST_CHECK(!self->pubsub_value.peers.at(peer).connected);
         BOOST_TEST(self->pubsub_value.peers.at(peer).generation == generation);
      }
      try { FORGE_THROW_EXCEPTION(exceptions::protocol_error, "same-generation terminal control failure"); }
      catch (const forge::exceptions::base& error) { self->record_pubsub_send_failure(peer, error, generation); }
      BOOST_TEST(source.peers().find(peer)->failures == failures + 2U);
   }
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_blocked_ihave_expiry() {
   auto fixture = pubsub_router_fixture{};
   const auto samples = std::make_shared<std::atomic_size_t>(0);
   auto config = manual_options();
   config.limits.mesh_n = 1;
   config.limits.mesh_n_low = 1;
   config.limits.mesh_n_high = 2;
   config.limits.mesh_outbound_min = 0;
   config.limits.mesh_score_min = 0;
   config.scoring = pubsub::scoring_params{.app_specific_score = [samples](const peer_id&) {
      return static_cast<double>(++*samples);
   }, .app_specific_weight = 1.0};
   auto& source = fixture.add("control-ihave-source", config);
   auto& healthy = fixture.add("control-ihave-healthy", manual_options());
   auto& blocked = fixture.add("control-ihave-blocked", manual_options());
   const auto self = source.impl_;
   auto ticket = forge::asio::gate::ticket{};
   const auto release = [&] { ticket.release(); };
   auto healthy_shutdown = gossipsub_test_shutdown{fixture.runtime, source, healthy, release};
   auto blocked_shutdown = gossipsub_test_shutdown{fixture.runtime, source, blocked, release};
   fixture.subscribe(source);
   fixture.subscribe(healthy);
   fixture.subscribe(blocked);
   fixture.connect(source, healthy);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(healthy.local_peer()) &&
          self->pubsub_value.peer_topics.at(healthy.local_peer()).contains(fixture.topic.value);
   }));
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] { return controls(fixture, source, healthy.local_peer(), true) == 1U; }));
   const auto published = fixture.publish(source, "bounded-gossip-before-native-gate");
   BOOST_CHECK(pubsub::codec::verify_message(published));
   const auto id = pubsub::codec::message_id(published);
   fixture.connect(source, blocked);
   const auto next_topic = pubsub::topic{"forge.control.healthy.next"};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
         co_return pubsub::validation_result::accept;
      };
      static_cast<void>(co_await source.async_subscribe(next_topic, accept));
      static_cast<void>(co_await healthy.async_subscribe(next_topic, accept));
   }());
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(blocked.local_peer()) &&
          self->pubsub_value.peer_topics.at(blocked.local_peer()).contains(fixture.topic.value) &&
          self->pubsub_value.peer_topics.at(healthy.local_peer()).contains(next_topic.value) &&
          self->pubsub_value.outbound.size() == 2U && self->pubsub_value.outbound_budget.total() == 0 &&
          std::ranges::all_of(self->pubsub_value.outbound, [](const auto& row) { return !row.second.snapshot_pending; });
   }));
   auto gate = std::shared_ptr<forge::asio::gate>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      gate = self->pubsub_value.outbound.at(blocked.local_peer()).write_gate;
   }
   auto acquire = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquire.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   ticket = acquire.get();
   const auto before_samples = samples->load();
   const auto failures = source.peers().find(blocked.local_peer())->failures;
   auto tick = boost::asio::co_spawn(fixture.runtime.context(), self->pubsub_heartbeat_once(), boost::asio::use_future);
   const auto nonblocking = tick.wait_for(250ms) == std::future_status::ready;
   if (!nonblocking) { ticket.release(); }
   if (tick.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   tick.get();
   BOOST_REQUIRE(nonblocking);
   BOOST_REQUIRE(fixture.wait([&] {
      if (controls(fixture, source, healthy.local_peer(), true) != 2U) { return false; }
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->size() == 0 && self->pubsub_value.controls->bytes() > 0 &&
          self->pubsub_value.outbound_budget.total() > 0;
   }));
   for (auto i = std::size_t{}; i < config.limits.history_length; ++i) {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
   }
   BOOST_TEST(samples->load() >= before_samples + 2U * (config.limits.history_length + 1U));
   BOOST_TEST(source.pubsub_snapshot().cached_messages == 0U);
   for (const auto& receipt : fixture.receipts(source)) {
      if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != blocked.local_peer() || receipt.frame.empty()) {
         continue;
      }
      const auto rpc = pubsub::codec::decode(receipt.frame, config);
      if (rpc.control_value) { BOOST_TEST(rpc.control_value->have.size() == 0U); }
   }
   ticket.release();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.controls->bytes() == 0 && self->pubsub_value.outbound_budget.total() == 0;
   }));
   for (auto i = 0; i < 3; ++i) { run(fixture.runtime, self->pubsub_heartbeat_once()); }
   auto advertised = std::size_t{};
   for (const auto& receipt : fixture.receipts(source)) {
      if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != blocked.local_peer() || receipt.frame.empty()) {
         continue;
      }
      const auto rpc = pubsub::codec::decode(receipt.frame, config);
      if (!rpc.control_value) { continue; }
      for (const auto& have : rpc.control_value->have) {
         BOOST_CHECK(have.subject == fixture.topic);
         BOOST_REQUIRE(have.message_ids.size() == 1U);
         BOOST_CHECK(have.message_ids.front() == id);
         ++advertised;
      }
   }
   // Donors enqueue before cache shift: the original one-shot IHAVE may finish
   // after expiry, but it is never retained for another heartbeat or retried.
   BOOST_TEST(advertised <= 1U);
   BOOST_TEST(controls(fixture, source, healthy.local_peer(), true) == 2U);
   BOOST_TEST(source.peers().find(blocked.local_peer())->failures == failures);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
      BOOST_TEST(self->pubsub_value.controls->size() == 0U);
   }
   blocked_shutdown.join();
   healthy_shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_direct_publish_failure_generation(bool reconnect) {
   auto fixture = pubsub_router_fixture{};
   auto caller = boost::asio::io_context{};
   auto config = manual_options();
   config.flood_publish = true; // This transport test invalidates the mesh edge before the actual send.
   auto& source = fixture.add("direct-failure-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& target = fixture.add("direct-failure-target", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   const auto self = source.impl_;
   const auto peer = target.local_peer();
   const auto reset = std::make_shared<forge::asio::notification>();
   const auto epoch = reset->epoch();
   const auto snapshot_received = std::make_shared<std::atomic_bool>(false);
   const auto read = std::make_shared<std::promise<std::size_t>>();
   auto body_read = read->get_future();
   auto publication = std::future<pubsub::message>{};
   const auto* stage = "setup";
   const auto diagnose = [&] {
      auto bytes = std::size_t{};
      auto pending = false;
      {
         const auto lock = std::scoped_lock{self->mutex};
         bytes = self->pubsub_value.outbound_budget.total();
         const auto out = self->pubsub_value.outbound.find(peer);
         pending = out != self->pubsub_value.outbound.end() && out->second.snapshot_pending;
      }
      std::fprintf(stderr,
          "direct-publish %s stage=%s snapshot_read=%d snapshot_pending=%d bytes=%zu publication_ready=%d active=%llu,%llu\n",
          reconnect ? "old" : "current", stage, snapshot_received->load(), pending, bytes,
          publication.valid() && publication.wait_for(0s) == std::future_status::ready,
          static_cast<unsigned long long>(source.diagnostics().metrics.active_sessions),
          static_cast<unsigned long long>(target.diagnostics().metrics.active_sessions));
      std::fflush(stderr);
   };
   const auto pump = [&] {
      caller.restart();
      for (auto index = 0; index < 1024; ++index) {
         if (caller.poll_one() == 0) { return; }
      }
      diagnose();
      gossipsub_test_shutdown::fail_closed();
   };
   const auto release = [&] { reset->notify(); };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, release, [&](auto deadline) {
      if (!publication.valid()) { return; }
      const auto remaining = std::max(0ms,
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
      if (!fixture.wait([&] {
         pump();
         return publication.wait_for(0s) == std::future_status::ready;
      }, remaining)) {
         diagnose();
         gossipsub_test_shutdown::fail_closed();
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (!self->pubsub_value.peer_topics.contains(peer) || !self->pubsub_value.outbound.contains(peer) ||
             self->pubsub_value.outbound.at(peer).snapshot_pending || self->pubsub_value.outbound_budget.total() != 0) {
            return false;
         }
      }
      const auto lock = std::scoped_lock{target.impl_->mutex};
      return target.impl_->pubsub_value.peer_topics.contains(source.local_peer()) &&
          target.impl_->pubsub_value.peer_topics.at(source.local_peer()).contains(fixture.topic.value);
   }));
   auto old = std::shared_ptr<node::impl::session_state>{};
   auto target_old = std::shared_ptr<node::impl::session_state>{};
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      old = self->sessions.at(self->pubsub_value.outbound.at(peer).session_id);
      generation = self->pubsub_value.peers.at(peer).generation;
      self->invalidate_pubsub_outbound_locked(peer); // Only the idle stream, before the public send starts.
   }
   {
      const auto lock = std::scoped_lock{target.impl_->mutex};
      target_old = target.impl_->session_for_path_locked(source.local_peer(), path::kind::direct, std::nullopt);
      BOOST_REQUIRE(target_old);
      BOOST_CHECK(target_old->authentication == peer_authentication::noise);
   }
   BOOST_CHECK(old->authentication == peer_authentication::noise);
   target.register_protocol_handler(builtins::meshsub_v11,
       [reset, epoch, read, snapshot_received, config](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto reported = false;
          try {
             auto buffer = std::vector<std::uint8_t>{};
             const auto frame = co_await async_read_length_delimited(incoming.stream, buffer, config.limits.max_rpc_size);
             const auto snapshot = pubsub::codec::decode(frame, config);
             if (snapshot.subscriptions.size() != 1 || !snapshot.messages.empty()) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "direct failure fixture expected subscription snapshot");
             }
             snapshot_received->store(true);
             if (buffer.empty()) { buffer = co_await incoming.stream.async_read(); }
             if (buffer.empty() || buffer.size() > forge::net::yamux::options{}.initial_window) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "direct failure fixture expected a partial native MESSAGE");
             }
             const auto prefix = forge::multiformats::varint_decode(buffer);
             if (prefix.value <= 2U * forge::net::yamux::options{}.initial_window ||
                 prefix.value > config.limits.max_rpc_size || buffer.size() <= prefix.size ||
                 buffer[prefix.size] != 0x12U) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "direct failure fixture did not observe the large native MESSAGE RPC");
             }
             // Read no more than one window of the three-window publication, then hold the real receiver.
             read->set_value(buffer.size());
             reported = true;
             co_await reset->async_wait(epoch);
             incoming.stream.cancel(); // Genuine native RESET, not an injected send exception.
          } catch (...) {
             if (!reported) { read->set_exception(std::current_exception()); }
             throw;
          }
       });
   const auto before = source.peers().find(peer)->failures;
   stage = "native-message-body";
   publication = boost::asio::co_spawn(caller,
       source.async_publish(fixture.topic, std::vector<std::uint8_t>(768U * 1024U, 'p')), boost::asio::use_future);
   BOOST_REQUIRE_MESSAGE(fixture.wait([&] {
      pump();
      return body_read.wait_for(0s) == std::future_status::ready;
   }, 5s), "direct publication did not reach its native receiver");
   BOOST_TEST(body_read.get() > 0U);
   BOOST_REQUIRE(fixture.wait([&] {
      pump();
      const auto lock = std::scoped_lock{self->mutex};
      const auto out = self->pubsub_value.outbound.find(peer);
      return out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          self->pubsub_value.outbound_budget.total() > forge::net::yamux::options{}.initial_window;
   }));
   BOOST_REQUIRE(publication.wait_for(0s) != std::future_status::ready);
   auto failed_stream = std::shared_ptr<stream>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      failed_stream = self->pubsub_value.outbound.at(peer).stream;
   }
   BOOST_REQUIRE(failed_stream);
   // Stop polling only this caller. Native read/reset/retirement workers retain the node executor.
   stage = "caller-paused-native-reset";
   reset->notify();
   BOOST_REQUIRE_MESSAGE(fixture.wait([&] { return !failed_stream->valid(); }),
       "native RESET did not invalidate the actual publication stream");
   BOOST_REQUIRE(publication.wait_for(0s) != std::future_status::ready);
   auto baseline = before;
   if (reconnect) {
      // Cancel the actual authenticated G1 transport; its real accept/retirement path removes the owner.
      // Then perform a second public TCP/Noise/Yamux connection, with no synthetic peer-generation mutation.
      self->request_cancel_session(old);
      target.impl_->request_cancel_session(target_old);
      stage = "native-g1-retirement";
      BOOST_REQUIRE(fixture.wait([&] {
         return source.diagnostics().metrics.active_sessions == 0U && target.diagnostics().metrics.active_sessions == 0U;
      }));
      BOOST_REQUIRE(target.unregister_protocol_handler(builtins::meshsub_v11));
      stage = "native-g2-handshake-subscriptions";
      fixture.connect(source, target);
      BOOST_REQUIRE(fixture.wait([&] {
         {
            const auto lock = std::scoped_lock{self->mutex};
            const auto out = self->pubsub_value.outbound.find(peer);
            const auto topics = self->pubsub_value.peer_topics.find(peer);
            if (self->pubsub_value.peers.at(peer).generation <= generation ||
                out == self->pubsub_value.outbound.end() || out->second.session_id == old->id ||
                out->second.snapshot_pending || topics == self->pubsub_value.peer_topics.end() ||
                !topics->second.contains(fixture.topic.value)) { return false; }
         }
         const auto lock = std::scoped_lock{target.impl_->mutex};
         const auto topics = target.impl_->pubsub_value.peer_topics.find(source.local_peer());
         return topics != target.impl_->pubsub_value.peer_topics.end() && topics->second.contains(fixture.topic.value);
      }));
      baseline = source.peers().find(peer)->failures;
      BOOST_CHECK(publication.wait_for(0s) != std::future_status::ready);
   }
   release();
   stage = "caller-resumed-old-failure";
   if (!fixture.wait([&] {
      pump();
      return publication.wait_for(0s) == std::future_status::ready;
   }, 5s)) {
      diagnose();
      gossipsub_test_shutdown::fail_closed();
   }
   auto failed = false;
   try { static_cast<void>(publication.get()); }
   catch (const forge::exceptions::base& error) {
      failed = true;
      BOOST_CHECK(exceptions::is(error, exceptions::code::closed));
   }
   BOOST_CHECK(failed);
   BOOST_TEST(source.peers().find(peer)->failures == baseline + (reconnect ? 0U : 1U));
   if (reconnect) {
      stage = "healthy-g2-publication";
      static_cast<void>(fixture.publish(source, "healthy-second-authenticated-lifetime"));
      BOOST_REQUIRE(fixture.wait([&] { return target.pubsub_snapshot().messages_delivered == 1U; }));
      BOOST_TEST(source.peers().find(peer)->failures == baseline);
   }
   stage = "joined-cleanup";
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_protocol_open_failure_generation(bool reconnect) {
   auto fixture = pubsub_router_fixture{};
   auto caller = boost::asio::io_context{};
   auto& source = fixture.add("protocol-failure-source", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   auto& target = fixture.add("protocol-failure-target", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   const auto self = source.impl_;
   const auto peer = target.local_peer();
   auto opened = std::future<stream>{};
   auto phase = detail::stream_open_phase::admission;
   const auto* stage = "setup";
   const auto diagnose = [&] {
      std::fprintf(stderr, "protocol-open %s stage=%s phase=%u ready=%d active=%llu,%llu\n",
          reconnect ? "old" : "current-timeout", stage, static_cast<unsigned>(phase),
          opened.valid() && opened.wait_for(0s) == std::future_status::ready,
          static_cast<unsigned long long>(source.diagnostics().metrics.active_sessions),
          static_cast<unsigned long long>(target.diagnostics().metrics.active_sessions));
      std::fflush(stderr);
   };
   const auto pump = [&] {
      caller.restart();
      for (auto index = 0; index < 1024; ++index) {
         if (caller.poll_one() == 0) { return; }
      }
      diagnose();
      gossipsub_test_shutdown::fail_closed();
   };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, {}, [&](auto deadline) {
      if (!opened.valid()) { return; }
      const auto remaining = std::max(0ms,
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
      if (!fixture.wait([&] {
         pump();
         return opened.wait_for(0s) == std::future_status::ready;
      }, remaining)) {
         diagnose();
         gossipsub_test_shutdown::fail_closed();
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto out = self->pubsub_value.outbound.find(peer);
      const auto topics = self->pubsub_value.peer_topics.find(peer);
      return out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0U && topics != self->pubsub_value.peer_topics.end() &&
          topics->second.contains(fixture.topic.value);
   }));
   run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      return source.pubsub_snapshot().mesh_edges == 1U && target.pubsub_snapshot().mesh_edges == 1U;
   }));
   auto old = std::shared_ptr<node::impl::session_state>{};
   auto target_old = std::shared_ptr<node::impl::session_state>{};
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      old = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(old);
      generation = self->pubsub_value.peers.at(peer).generation;
   }
   {
      const auto lock = std::scoped_lock{target.impl_->mutex};
      target_old = target.impl_->session_for_path_locked(source.local_peer(), path::kind::direct, std::nullopt);
      BOOST_REQUIRE(target_old);
   }
   BOOST_CHECK(old->authentication == peer_authentication::noise);
   BOOST_CHECK(target_old->authentication == peer_authentication::noise);
   BOOST_REQUIRE(old->direct_endpoint);
   const auto concrete = old->direct_endpoint->to_multiaddr();
   BOOST_REQUIRE(std::ranges::any_of(old->direct_roots, [&](const auto& root) {
      return root.components() == concrete.components();
   }));
   auto inbound_before = target.diagnostics().resources.streams.inbound_streams;
   auto outbound_before = source.diagnostics().resources.streams.outbound_streams;
   const auto observe = [&](const char* checkpoint, node& owner, const peer_id& remote) {
      const auto snapshot = owner.diagnostics();
      auto active = std::size_t{};
      auto retiring = std::size_t{};
      auto inbound = std::size_t{};
      auto outbound_session = std::uint64_t{};
      auto outbound_generation = std::uint64_t{};
      auto snapshot_pending = false;
      auto outbound_protocol = std::string{};
      {
         const auto lock = std::scoped_lock{owner.impl_->mutex};
         active = std::ranges::count_if(owner.impl_->sessions, [&](const auto& row) {
            return row.second->info.remote_peer == remote;
         });
         retiring = std::ranges::count_if(owner.impl_->retiring_sessions, [&](const auto& row) {
            return row.second->info.remote_peer == remote;
         });
         const auto incoming = owner.impl_->pubsub_value.inbound.find(remote);
         if (incoming != owner.impl_->pubsub_value.inbound.end()) { inbound = incoming->second.size(); }
         const auto outgoing = owner.impl_->pubsub_value.outbound.find(remote);
         if (outgoing != owner.impl_->pubsub_value.outbound.end()) {
            outbound_session = outgoing->second.session_id;
            outbound_generation = outgoing->second.generation;
            snapshot_pending = outgoing->second.snapshot_pending;
            outbound_protocol = outgoing->second.protocol.value;
         }
      }
      std::fprintf(stderr,
          "protocol-open %s checkpoint=%s owner=%s peer=%s protocol=%s phase=%u "
          "baseline-source-out=%zu baseline-target-in=%zu active=%zu retiring=%zu source-old=%llu source-old-closed=%d "
          "pubsub-in=%zu pubsub-out-session=%llu generation=%llu snapshot-pending=%d pubsub-protocol=%s\n",
          reconnect ? "old" : "current-timeout", checkpoint, &owner == &source ? "source" : "target",
          remote.to_string().c_str(), builtins::ping.value.c_str(), static_cast<unsigned>(phase),
          outbound_before, inbound_before, active, retiring, static_cast<unsigned long long>(old->id),
          old->closed.load(), inbound, static_cast<unsigned long long>(outbound_session),
          static_cast<unsigned long long>(outbound_generation), snapshot_pending, outbound_protocol.c_str());
      const auto scopes = std::array<std::pair<const char*, resource_manager::scope_totals>, 5>{
          std::pair{"streams", snapshot.resources.streams},
          std::pair{"transient", snapshot.resources.transient},
          std::pair{"peers-aggregate", snapshot.resources.peers},
          std::pair{"protocols-aggregate", snapshot.resources.protocols},
          std::pair{"protocol-peers-aggregate", snapshot.resources.protocol_peers},
      };
      for (const auto& [name, totals] : scopes) {
         std::fprintf(stderr, "  scope=%s in=%zu out=%zu memory=%llu fd=%zu connections-in=%zu connections-out=%zu\n",
             name, totals.inbound_streams, totals.outbound_streams, static_cast<unsigned long long>(totals.memory),
             totals.file_descriptors, totals.inbound_connections, totals.outbound_connections);
      }
      std::fflush(stderr);
   };
   const auto observations = [&](const char* checkpoint) {
      observe(checkpoint, source, peer);
      observe(checkpoint, target, source.local_peer());
   };
   const auto quiescent = [&](node& owner, const peer_id& remote, std::size_t held_open) {
      const auto snapshot = owner.diagnostics().resources;
      if (snapshot.streams.inbound_streams != 1U || snapshot.streams.outbound_streams != 1U + held_open ||
          snapshot.transient.inbound_streams != 0U || snapshot.transient.outbound_streams != held_open) {
         return false;
      }
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      const auto session = owner.impl_->session_for_path_locked(remote, path::kind::direct, std::nullopt);
      const auto out = owner.impl_->pubsub_value.outbound.find(remote);
      const auto in = owner.impl_->pubsub_value.inbound.find(remote);
      const auto topics = owner.impl_->pubsub_value.peer_topics.find(remote);
      return session && !session->closed && session->identify_completed &&
          !owner.impl_->identify_push_value.coordinator_running &&
          (!session->identify_push_supported ||
              session->identify_push_attempted_generation >= owner.impl_->identify_push_value.generation) &&
          out != owner.impl_->pubsub_value.outbound.end() && out->second.session_id == session->id &&
          out->second.stream && out->second.stream->valid() && !out->second.snapshot_pending &&
          owner.impl_->pubsub_value.outbound_budget.total() == 0U &&
          in != owner.impl_->pubsub_value.inbound.end() && in->second.size() == 1U &&
          in->second.begin()->second.session_id == session->id && topics != owner.impl_->pubsub_value.peer_topics.end() &&
          topics->second.contains(fixture.topic.value);
   };
   stage = "setup-pubsub-quiescence";
   const auto setup_ready = fixture.wait([&] {
      return quiescent(source, peer, 0U) && quiescent(target, source.local_peer(), 0U);
   });
   if (!setup_ready) { diagnose(); observations(stage); }
   BOOST_REQUIRE(setup_ready);
   inbound_before = target.diagnostics().resources.streams.inbound_streams;
   outbound_before = source.diagnostics().resources.streams.outbound_streams;
   BOOST_TEST(inbound_before == 1U);
   BOOST_TEST(outbound_before == 1U);
   const auto timeout = reconnect ? 5s : 500ms;
   stage = "native-syn";
   opened = boost::asio::co_spawn(caller,
       self->open_protocol_on_direct_session(peer, builtins::ping, old, timeout, {}, &phase), boost::asio::use_future);
   BOOST_REQUIRE_MESSAGE(fixture.wait([&] {
      caller.restart();
      static_cast<void>(caller.poll_one());
      return phase == detail::stream_open_phase::native_open;
   }), "protocol open did not enter the native transport");
   // native_open precedes the lower body, so drive the caller until the real
   // remote Yamux accept owns an inbound reservation, not merely until that phase.
   BOOST_REQUIRE_MESSAGE(fixture.wait([&] {
      caller.restart();
      static_cast<void>(caller.poll_one());
      return target.diagnostics().resources.streams.inbound_streams > inbound_before;
   }), "protocol open did not create a real remote inbound stream");
   BOOST_REQUIRE(opened.wait_for(0s) != std::future_status::ready);
   auto current = old;
   if (reconnect) {
      stage = "native-g1-retirement";
      self->request_cancel_session(old);
      target.impl_->request_cancel_session(target_old);
      BOOST_REQUIRE(fixture.wait([&] {
         return source.diagnostics().metrics.active_sessions == 0U && target.diagnostics().metrics.active_sessions == 0U;
      }));
      stage = "native-g2-handshake";
      fixture.connect(source, target);
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{self->mutex};
         const auto topics = self->pubsub_value.peer_topics.find(peer);
         const auto out = self->pubsub_value.outbound.find(peer);
         return self->pubsub_value.peers.at(peer).generation > generation &&
             topics != self->pubsub_value.peer_topics.end() && topics->second.contains(fixture.topic.value) &&
             out != self->pubsub_value.outbound.end() && !out->second.snapshot_pending;
      }));
      {
         const auto lock = std::scoped_lock{self->mutex};
         current = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
         BOOST_REQUIRE(current);
         BOOST_TEST(current->id != old->id);
         BOOST_CHECK(current->authentication == peer_authentication::noise);
         generation = self->pubsub_value.peers.at(peer).generation;
      }
      run(fixture.runtime, self->pubsub_heartbeat_once());
      BOOST_REQUIRE(fixture.wait([&] { return source.pubsub_snapshot().mesh_edges == 1U; }));
      // G1's suspended open still owns one unbound outbound reservation. Settle
      // G2 Identify/Push without waiting for that caller-owned reservation to vanish.
      stage = "g2-independent-protocol-quiescence";
      const auto reconnected_ready = fixture.wait([&] {
         return quiescent(source, peer, 1U) && quiescent(target, source.local_peer(), 0U);
      });
      if (!reconnected_ready) { diagnose(); observations(stage); }
      BOOST_REQUIRE(reconnected_ready);
   } else {
      stage = "native-deadline-while-caller-paused";
      const auto elapsed = std::chrono::steady_clock::now() + timeout;
      BOOST_REQUIRE(fixture.wait([&] { return std::chrono::steady_clock::now() >= elapsed; }, 2s));
   }
   const auto before = source.peers().find(peer);
   BOOST_REQUIRE(before);
   const auto direct_failures = source.metrics().direct_failures;
   const auto pubsub_before = source.pubsub_snapshot();
   stage = "caller-resume-old-outcome";
   if (!fixture.wait([&] {
      pump();
      return opened.wait_for(0s) == std::future_status::ready;
   })) {
      diagnose();
      observations(stage);
      gossipsub_test_shutdown::fail_closed();
   }
   auto failed = false;
   try { static_cast<void>(opened.get()); }
   catch (const forge::exceptions::base& error) {
      failed = true;
      const auto expected = reconnect ? exceptions::code::canceled : exceptions::code::timeout;
      if (!exceptions::is(error, expected)) {
         const auto code = exceptions::code_of(error);
         std::fprintf(stderr,
             "protocol-open %s exception category=%s code=%d p2p-code=%d phase=%u expected=%u what=%s\n",
             reconnect ? "old" : "current-timeout", error.code().category().name(), error.code().value(),
             code ? static_cast<int>(*code) : -1, static_cast<unsigned>(phase), static_cast<unsigned>(expected),
             error.what());
         std::fflush(stderr);
         observations("unexpected-open-exception");
      }
      BOOST_CHECK(exceptions::is(error, expected));
   }
   if (!failed) { diagnose(); observations("unexpected-open-success"); }
   BOOST_CHECK(failed);
   const auto resources_returned = fixture.wait([&] {
      return source.diagnostics().resources.streams.outbound_streams == outbound_before;
   });
   if (!resources_returned) { diagnose(); observations("reservation-wait-expired"); }
   BOOST_REQUIRE(resources_returned);
   const auto after = source.peers().find(peer);
   BOOST_REQUIRE(after);
   BOOST_TEST(after->failures == before->failures + (reconnect ? 0U : 1U));
   BOOST_TEST(source.metrics().direct_failures == direct_failures + (reconnect ? 0U : 1U));
   for (const auto& endpoint : before->endpoints) {
      const auto found = std::ranges::find_if(after->endpoints, [&](const auto& row) {
         return row.address.components() == endpoint.address.components() &&
             row.kind == endpoint.kind && row.relay_peer == endpoint.relay_peer;
      });
      BOOST_REQUIRE(found != after->endpoints.end());
      const auto attributed = !reconnect && endpoint.kind == path::kind::direct &&
          endpoint.address.components() == concrete.components();
      BOOST_TEST(found->failures == endpoint.failures + (attributed ? 1U : 0U));
      if (reconnect) { BOOST_CHECK(found->backoff_until == endpoint.backoff_until); }
   }
   if (reconnect) { BOOST_TEST(after->score == before->score); }
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->sessions.at(current->id) == current);
      BOOST_CHECK(!current->closed);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == generation);
      BOOST_CHECK(self->pubsub_value.mesh.at(fixture.topic.value).contains(peer));
      BOOST_CHECK(self->protocol_open_deadlines.empty());
   }
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == pubsub_before.invalid_messages);
   BOOST_TEST(source.pubsub_snapshot().mesh_edges == pubsub_before.mesh_edges);
   stage = "healthy-publication";
   static_cast<void>(fixture.publish(source, "healthy-after-protocol-open-outcome"));
   BOOST_REQUIRE(fixture.wait([&] { return target.pubsub_snapshot().messages_delivered == 1U; }));
   stage = "joined-cleanup";
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_retired_inbound() {
   auto fixture = pubsub_router_fixture{};
   auto& source = fixture.add("late-inbound-source", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   auto& target = fixture.add("late-inbound-target", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   const auto self = target.impl_;
   const auto peer = source.local_peer();
   const auto barrier = std::make_shared<forge::asio::notification>();
   const auto epoch = barrier->epoch();
   const auto entered = std::make_shared<std::promise<std::uint64_t>>();
   auto accepted = entered->get_future();
   const auto done = std::make_shared<std::promise<void>>();
   auto handled = done->get_future();
   auto incoming = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] {
      barrier->notify();
      incoming.request_cancel();
   }};
   fixture.subscribe(source);
   fixture.subscribe(target);
   fixture.connect(source, target);
   BOOST_REQUIRE(fixture.wait([&] {
      {
         const auto lock = std::scoped_lock{source.impl_->mutex};
         const auto out = source.impl_->pubsub_value.outbound.find(target.local_peer());
         if (out == source.impl_->pubsub_value.outbound.end() || out->second.snapshot_pending ||
             source.impl_->pubsub_value.outbound_budget.total() != 0) { return false; }
      }
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.contains(peer) &&
          self->pubsub_value.peer_topics.at(peer).contains(fixture.topic.value);
   }));
   target.register_protocol_handler(builtins::meshsub_v11,
       [weak = std::weak_ptr<node::impl>{self}, barrier, epoch, entered, done]
       (node::incoming_protocol_stream value) -> boost::asio::awaitable<void> {
          try {
             const auto owner = weak.lock();
             if (!owner) { FORGE_THROW_EXCEPTION(exceptions::closed, "late inbound fixture owner closed"); }
             auto session = std::shared_ptr<node::impl::session_state>{};
             {
                const auto lock = std::scoped_lock{owner->mutex};
                session = owner->sessions.at(value.session.id);
             }
             entered->set_value(session->id);
             co_await barrier->async_wait(epoch);
             co_await owner->handle_pubsub(session, std::move(value.stream), value.protocol);
             done->set_value();
          } catch (...) {
             done->set_exception(std::current_exception());
             throw;
          }
       });
   incoming = fixture.open(source, target); // Real negotiated authenticated stream, delayed before handler admission.
   BOOST_REQUIRE(accepted.wait_for(5s) == std::future_status::ready);
   const auto id = accepted.get();
   auto retired = std::shared_ptr<node::impl::session_state>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      retired = self->sessions.at(id);
      BOOST_CHECK(retired->authentication == peer_authentication::noise);
   }
   self->request_cancel_session(retired);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return !self->sessions.contains(id) && self->pubsub_value.peers.contains(peer) &&
          !self->pubsub_value.peers.at(peer).connected;
   }));
   auto lifetime = std::uint64_t{};
   auto next_inbound = std::uint64_t{};
   auto deadline = std::chrono::steady_clock::time_point{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      lifetime = self->pubsub_value.peers.at(peer).generation;
      deadline = self->pubsub_value.peers.at(peer).retain_until;
      next_inbound = self->pubsub_value.next_inbound_generation;
   }
   const auto before = target.pubsub_scores();
   BOOST_TEST(before.connected_peers == 0U);
   BOOST_TEST(before.retained_peers == 1U);
   const auto backpressure = target.diagnostics().metrics.backpressure_rejections;
   barrier->notify();
   BOOST_REQUIRE(handled.wait_for(5s) == std::future_status::ready);
   handled.get();
   const auto after = target.pubsub_scores();
   BOOST_TEST(after.connected_peers == 0U);
   BOOST_TEST(after.retained_peers == 1U);
   BOOST_TEST(after.capacity_rejections == before.capacity_rejections);
   BOOST_TEST(target.diagnostics().metrics.backpressure_rejections == backpressure);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->pubsub_value.peers.at(peer).connected);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == lifetime);
      BOOST_CHECK(self->pubsub_value.peers.at(peer).retain_until == deadline);
      BOOST_TEST(self->pubsub_value.next_inbound_generation == next_inbound);
      BOOST_CHECK(!self->pubsub_value.inbound.contains(peer));
      const auto retained = self->pubsub_value.scoring->inspect(peer, deadline);
      BOOST_REQUIRE(retained);
      BOOST_REQUIRE(retained->retain_until);
      BOOST_CHECK(*retained->retain_until == deadline);
      const auto expired = *retained->retain_until + std::chrono::steady_clock::duration{1};
      self->pubsub_value.scoring->tick(expired); // Donors retain through the deadline, expire strictly after it.
      BOOST_TEST(self->pubsub_value.scoring->snapshot(expired).retained_peers == 0U);
   }
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(target.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_retired_announce() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.scoring = pubsub::scoring_params{};
   config.scoring->topics.emplace(fixture.topic, pubsub::topic_score_params{});
   config.scoring->limits.max_connected_peers = 1;
   config.scoring->limits.max_retained_peers = 2;
   config.scoring->retain_score = 60s;
   auto& owner = fixture.add("late-announce-owner", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& remote = fixture.add("late-announce-remote", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   const auto self = owner.impl_;
   const auto peer = remote.local_peer();
   const auto barrier = std::make_shared<forge::asio::notification>();
   const auto epoch = barrier->epoch();
   const auto entered = std::make_shared<std::promise<void>>();
   auto started = entered->get_future();
   const auto done = std::make_shared<std::promise<void>>();
   auto completed = done->get_future();
   auto launched = false;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, [&] { barrier->notify(); },
       [&](auto deadline) {
          if (launched && completed.valid() && completed.wait_until(deadline) != std::future_status::ready) {
             gossipsub_test_shutdown::fail_closed();
          }
       }};
   fixture.subscribe(owner, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      co_return pubsub::validation_result::reject;
   });
   fixture.subscribe(remote);
   fixture.connect(owner, remote);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto session = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      return session && session->identify_completed && self->pubsub_value.peer_topics.contains(peer) &&
          self->pubsub_value.peer_topics.at(peer).contains(fixture.topic.value) &&
          self->pubsub_value.outbound.contains(peer) && !self->pubsub_value.outbound.at(peer).snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0;
   }));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{remote.impl_->mutex};
      const auto topics = remote.impl_->pubsub_value.peer_topics.find(owner.local_peer());
      return topics != remote.impl_->pubsub_value.peer_topics.end() && topics->second.contains(fixture.topic.value);
   }));
   run(fixture.runtime, remote.impl_->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      return remote.pubsub_snapshot().mesh_edges == 1U && owner.pubsub_snapshot().mesh_edges == 1U;
   }));
   static_cast<void>(fixture.publish(remote, "native-rejection-retained-before-late-announce"));
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().invalid_messages == 1U; }));
   auto retired = std::shared_ptr<node::impl::session_state>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      retired = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(retired);
      BOOST_CHECK(retired->authentication == peer_authentication::noise);
      BOOST_TEST(self->pubsub_score_locked(peer) < 0.0);
   }
   // Delay only this fixture's tracked announcement; transport and retirement are native.
   launched = self->launch_tracked([weak = std::weak_ptr<node::impl>{self}, peer, barrier, epoch, entered, done]()
       -> boost::asio::awaitable<void> {
      try {
         entered->set_value();
         co_await barrier->async_wait(epoch);
         const auto active = weak.lock();
         if (!active) { FORGE_THROW_EXCEPTION(exceptions::closed, "late announce fixture owner closed"); }
         co_await active->announce_pubsub_subscriptions(peer);
         done->set_value();
      } catch (...) {
         done->set_exception(std::current_exception());
         throw;
      }
   });
   BOOST_REQUIRE(launched);
   BOOST_REQUIRE(started.wait_for(5s) == std::future_status::ready);
   started.get();
   self->request_cancel_session(retired);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return !self->sessions.contains(retired->id) && !self->retiring_sessions.contains(retired->id) &&
          self->pubsub_value.peers.contains(peer) && !self->pubsub_value.peers.at(peer).connected;
   }));
   auto record = owner.peers().find(peer);
   BOOST_REQUIRE(record);
   BOOST_REQUIRE(!record->endpoints.empty());
   record->endpoints.clear();
   owner.peers().upsert(std::move(*record)); // Public route withdrawal, not a connection/score-state substitute.
   BOOST_REQUIRE(owner.peers().find(peer)->endpoints.empty());
   auto lifetime = std::uint64_t{};
   auto next_lifetime = std::uint64_t{};
   auto deadline = std::chrono::steady_clock::time_point{};
   auto score = double{};
   auto observed = std::chrono::steady_clock::now();
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->stopped);
      lifetime = self->pubsub_value.peers.at(peer).generation;
      next_lifetime = self->pubsub_value.next_peer_generation;
      deadline = self->pubsub_value.peers.at(peer).retain_until;
      score = self->pubsub_value.scoring->score(peer, observed);
   }
   BOOST_TEST(score < 0.0);
   const auto before = owner.pubsub_scores();
   const auto backpressure = owner.diagnostics().metrics.backpressure_rejections;
   const auto failures = owner.peers().find(peer)->failures;
   const auto receipts = fixture.receipts(owner).size();
   BOOST_TEST(before.connected_peers == 0U);
   BOOST_TEST(before.retained_peers == 1U);
   barrier->notify();
   if (completed.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   completed.get();
   const auto after = owner.pubsub_scores();
   BOOST_TEST(after.connected_peers == 0U);
   BOOST_TEST(after.retained_peers == 1U);
   BOOST_TEST(after.capacity_rejections == before.capacity_rejections);
   BOOST_TEST(owner.diagnostics().metrics.backpressure_rejections == backpressure);
   BOOST_TEST(owner.peers().find(peer)->failures == failures);
   BOOST_TEST(fixture.receipts(owner).size() == receipts);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->stopped);
      BOOST_CHECK(!self->pubsub_value.peers.at(peer).connected);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == lifetime);
      BOOST_TEST(self->pubsub_value.next_peer_generation == next_lifetime);
      BOOST_CHECK(self->pubsub_value.peers.at(peer).retain_until == deadline);
      BOOST_CHECK(!self->pubsub_value.outbound.contains(peer));
      BOOST_TEST(self->pubsub_value.outbound_budget.total() == 0U);
      BOOST_TEST(self->pubsub_value.scoring->score(peer, observed) == score);
      const auto retained = self->pubsub_value.scoring->inspect(peer, deadline);
      BOOST_REQUIRE(retained);
      BOOST_CHECK(!retained->connected);
      BOOST_TEST(retained->value == score);
      BOOST_REQUIRE(retained->retain_until);
      BOOST_CHECK(*retained->retain_until == deadline);
      const auto expired = *retained->retain_until + std::chrono::steady_clock::duration{1};
      self->pubsub_value.scoring->tick(expired);
      BOOST_TEST(self->pubsub_value.scoring->snapshot(expired).retained_peers == 0U);
   }
   shutdown.join();
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(remote.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_announce_capacity_retry() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.scoring = pubsub::scoring_params{};
   config.scoring->limits.max_connected_peers = 1;
   config.scoring->limits.max_retained_peers = 2;
   auto& owner = fixture.add("announce-capacity-owner", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& occupied = fixture.add("announce-capacity-occupied", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   auto& waiting = fixture.add("announce-capacity-waiting", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   const auto self = owner.impl_;
   const auto peer = waiting.local_peer();
   auto first_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, occupied};
   auto second_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, waiting};
   fixture.subscribe(owner);
   fixture.connect(owner, occupied);
   fixture.connect(owner, waiting);
   auto retired = std::shared_ptr<node::impl::session_state>{};
   auto live = std::shared_ptr<node::impl::session_state>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      retired = self->session_for_path_locked(occupied.local_peer(), path::kind::direct, std::nullopt);
      live = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(retired);
      BOOST_REQUIRE(live);
      BOOST_CHECK(live->authentication == peer_authentication::noise);
      BOOST_CHECK(self->sessions.at(live->id) == live);
      BOOST_CHECK(!self->pubsub_value.peers.contains(peer));
      BOOST_CHECK(!self->pubsub_value.outbound.contains(peer));
   }
   const auto full = owner.pubsub_scores();
   BOOST_TEST(full.connected_peers == 1U);
   BOOST_TEST(full.capacity_rejections > 0U);
   run(fixture.runtime, self->announce_pubsub_subscriptions(peer));
   BOOST_TEST(owner.pubsub_scores().capacity_rejections == full.capacity_rejections + 1U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->pubsub_value.peers.contains(peer));
      BOOST_CHECK(!self->pubsub_value.outbound.contains(peer));
      BOOST_CHECK(self->sessions.at(live->id) == live);
   }
   self->request_cancel_session(retired);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return !self->sessions.contains(retired->id) && !self->retiring_sessions.contains(retired->id) &&
          self->pubsub_value.peers.contains(occupied.local_peer()) &&
          !self->pubsub_value.peers.at(occupied.local_peer()).connected;
   }));
   const auto available = owner.pubsub_scores();
   BOOST_TEST(available.connected_peers == 0U);
   BOOST_TEST(available.retained_peers == 1U);
   run(fixture.runtime, self->announce_pubsub_subscriptions(peer));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{waiting.impl_->mutex};
      return waiting.impl_->pubsub_value.peer_topics.contains(owner.local_peer()) &&
          waiting.impl_->pubsub_value.peer_topics.at(owner.local_peer()).contains(fixture.topic.value);
   }));
   const auto admitted = owner.pubsub_scores();
   BOOST_TEST(admitted.connected_peers == 1U);
   BOOST_TEST(admitted.retained_peers == 1U);
   BOOST_TEST(admitted.capacity_rejections == available.capacity_rejections);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->stopped);
      BOOST_CHECK(!live->closed);
      BOOST_CHECK(self->sessions.at(live->id) == live);
      BOOST_CHECK(self->pubsub_value.peers.at(peer).connected);
      BOOST_TEST(self->pubsub_value.outbound.at(peer).session_id == live->id);
      BOOST_CHECK(!self->pubsub_value.outbound.at(peer).snapshot_pending);
      BOOST_TEST(self->pubsub_value.outbound_budget.total() == 0U);
   }
   second_shutdown.join();
   first_shutdown.join();
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(occupied.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(waiting.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_retired_rpc() {
   auto fixture = pubsub_router_fixture{};
   auto caller = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1,
       .thread_name = "pubsub-rpc-caller"}};
   const auto armed = std::make_shared<std::atomic_bool>(false);
   const auto entered = std::make_shared<std::promise<std::uint64_t>>();
   auto traced = entered->get_future();
   const auto leave = std::make_shared<std::promise<void>>();
   const auto resume = leave->get_future().share();
   auto config = manual_options();
   config.tracer = [armed, entered, resume, executor = caller.context().get_executor(), wire = config](const pubsub::trace_event& event) {
      if (event.kind != pubsub::trace_kind::rpc_read || !armed->load()) { return; }
      const auto value = pubsub::codec::decode(event.framed_rpc, wire);
      if (value.subscriptions.size() != 1 || value.subscriptions.front().subscribe || !armed->exchange(false)) {
         return;
      }
      if (!executor.running_in_this_thread()) {
         try {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "retired RPC barrier escaped its isolated caller executor");
         } catch (...) { entered->set_exception(std::current_exception()); }
         return; // Never block a native executor if the bridge contract regresses.
      }
      entered->set_value(event.session_id);
      if (resume.wait_for(30s) != std::future_status::ready) {
         std::fprintf(stderr, "retired-rpc stage=isolated-caller-read-hold session=%llu stream=%lld\n",
             static_cast<unsigned long long>(event.session_id), static_cast<long long>(event.stream_id));
         std::fflush(stderr);
         gossipsub_test_shutdown::fail_closed();
      }
   };
   auto& source = fixture.add("retired-rpc-source", manual_options(), {}, "/ip4/127.0.0.1/tcp/0",
       node::stream_security::noise);
   auto& owner = fixture.add("retired-rpc-owner", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   const auto self = owner.impl_;
   const auto peer = source.local_peer();
   const auto old_id = std::make_shared<std::atomic_uint64_t>(0);
   const auto done = std::make_shared<std::promise<void>>();
   auto completed = done->get_future();
   auto incoming = stream{};
   const auto* stage = "setup";
   auto released = false;
   auto launched = false;
   const auto release = [&] {
      armed->store(false);
      if (!released) { released = true; leave->set_value(); }
      incoming.request_cancel();
   };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, owner, release, [&](auto deadline) {
      if (launched && completed.valid() && completed.wait_until(deadline) != std::future_status::ready) {
         std::fprintf(stderr, "retired-rpc stage=%s old-handler-unjoined active=%llu,%llu\n", stage,
             static_cast<unsigned long long>(source.diagnostics().metrics.active_sessions),
             static_cast<unsigned long long>(owner.diagnostics().metrics.active_sessions));
         std::fflush(stderr);
         gossipsub_test_shutdown::fail_closed();
      }
   }};
   fixture.subscribe(source);
   fixture.subscribe(owner);
   fixture.connect(source, owner);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      return source.impl_->pubsub_value.peer_topics.contains(owner.local_peer()) &&
          source.impl_->pubsub_value.peer_topics.at(owner.local_peer()).contains(fixture.topic.value);
   }));
   run(fixture.runtime, source.impl_->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().mesh_edges == 1U; }));
   auto retired = std::shared_ptr<node::impl::session_state>{};
   auto source_retired = std::shared_ptr<node::impl::session_state>{};
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      retired = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(retired);
      BOOST_CHECK(retired->authentication == peer_authentication::noise);
      old_id->store(retired->id);
      generation = self->pubsub_value.peers.at(peer).generation;
   }
   {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      source_retired = source.impl_->session_for_path_locked(owner.local_peer(), path::kind::direct, std::nullopt);
      BOOST_REQUIRE(source_retired);
      BOOST_CHECK(source_retired->authentication == peer_authentication::noise);
   }
   owner.register_protocol_handler(builtins::meshsub_v11,
       [weak = std::weak_ptr<node::impl>{self}, old_id, done, executor = caller.context().get_executor()](node::incoming_protocol_stream value)
       -> boost::asio::awaitable<void> {
          const auto old = value.session.id == old_id->load();
          try {
             const auto active = weak.lock();
             if (!active) { FORGE_THROW_EXCEPTION(exceptions::closed, "retired RPC fixture owner closed"); }
             auto session = std::shared_ptr<node::impl::session_state>{};
             {
                const auto lock = std::scoped_lock{active->mutex};
                session = active->sessions.at(value.session.id);
             }
             if (old) {
                // Only the real G1 handler's continuation uses this isolated caller. Native session workers
                // already run on the node executor; G2 handlers must not queue behind the held G1 caller.
                co_await boost::asio::co_spawn(executor,
                    active->handle_pubsub(session, std::move(value.stream), value.protocol), boost::asio::use_awaitable);
             } else {
                co_await active->handle_pubsub(session, std::move(value.stream), value.protocol);
             }
             if (old) { done->set_value(); }
          } catch (...) {
             if (old) { done->set_exception(std::current_exception()); }
             throw;
          }
       });
   incoming = fixture.open(source, owner);
   launched = true;
   auto message = pubsub::message{.data = {'o'}, .seqno = {0, 0, 0, 0, 0, 0, 0, 0x73}, .subject = fixture.topic};
   BOOST_REQUIRE(source.impl_->identity.private_key);
   pubsub::codec::sign_message(message, *source.impl_->identity.private_key, config);
   BOOST_TEST(message.seqno.size() == 8U);
   BOOST_REQUIRE(pubsub::codec::verify_message(message, config));
   const auto key = bytes_key(pubsub::codec::message_id(message, config));
   stage = "native-g1-read";
   armed->store(true);
   fixture.send(incoming, pubsub::rpc{.subscriptions = {{.subscribe = false, .subject = fixture.topic}},
       .messages = {message}, .control_value = pubsub::control{.prunes = {{.subject = fixture.topic, .backoff = 60s}}}});
   BOOST_REQUIRE(traced.wait_for(5s) == std::future_status::ready);
   BOOST_TEST(traced.get() == retired->id);
   stage = "native-g1-retirement";
   self->request_cancel_session(retired);
   // Own both native G1 cancellations: held rpc_read is not a remote-EOF completion barrier.
   source.impl_->request_cancel_session(source_retired);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return !self->sessions.contains(retired->id) && self->pubsub_value.peers.contains(peer) &&
          !self->pubsub_value.peers.at(peer).connected;
   }));
   BOOST_REQUIRE(fixture.wait([&] { return source.diagnostics().metrics.active_sessions == 0U; }));
   {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      BOOST_CHECK(source_retired->closed);
      BOOST_CHECK(!source.impl_->sessions.contains(source_retired->id));
   }
   stage = "native-g2-handshake-subscriptions";
   fixture.connect(source, owner); // A fresh TCP/Noise/Yamux handshake, not a synthetic lifetime rewrite.
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{source.impl_->mutex};
      return source.impl_->pubsub_value.peer_topics.contains(owner.local_peer()) &&
          source.impl_->pubsub_value.peer_topics.at(owner.local_peer()).contains(fixture.topic.value);
   }));
   run(fixture.runtime, source.impl_->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().mesh_edges == 1U; }));
   auto current = std::shared_ptr<node::impl::session_state>{};
   auto current_generation = std::uint64_t{};
   auto topics = std::size_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      current = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(current);
      BOOST_TEST(current->id != retired->id);
      BOOST_CHECK(current->authentication == peer_authentication::noise);
      current_generation = self->pubsub_value.peers.at(peer).generation;
      BOOST_TEST(current_generation > generation);
      BOOST_CHECK(self->pubsub_value.peer_topics.at(peer).contains(fixture.topic.value));
      topics = self->pubsub_value.remote_topic_entries;
   }
   const auto before = owner.pubsub_snapshot();
   const auto failures = owner.peers().find(peer)->failures;
   stage = "isolated-caller-resume";
   release();
   if (completed.wait_for(5s) != std::future_status::ready) {
      std::fprintf(stderr, "retired-rpc stage=%s old-handler-unjoined active=%llu,%llu\n", stage,
          static_cast<unsigned long long>(source.diagnostics().metrics.active_sessions),
          static_cast<unsigned long long>(owner.diagnostics().metrics.active_sessions));
      std::fflush(stderr);
      gossipsub_test_shutdown::fail_closed();
   }
   completed.get();
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->sessions.at(current->id) == current);
      BOOST_CHECK(self->pubsub_value.peers.at(peer).connected);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == current_generation);
      BOOST_CHECK(self->pubsub_value.peer_topics.at(peer).contains(fixture.topic.value));
      BOOST_CHECK(self->pubsub_value.mesh.at(fixture.topic.value).contains(peer));
      BOOST_TEST(self->pubsub_value.remote_topic_entries == topics);
      BOOST_CHECK(self->pubsub_value.backoffs.remote_status(fixture.topic.value, peer, std::chrono::steady_clock::now()) ==
          detail::pubsub_backoff::status::none);
      BOOST_CHECK(!self->pubsub_value.cache.contains(key));
   }
   const auto after = owner.pubsub_snapshot();
   BOOST_TEST(after.mesh_edges == before.mesh_edges);
   BOOST_TEST(after.messages_received == before.messages_received);
   BOOST_TEST(after.messages_delivered == before.messages_delivered);
   BOOST_TEST(after.invalid_messages == before.invalid_messages);
   BOOST_TEST(after.control_messages == before.control_messages);
   BOOST_TEST(owner.peers().find(peer)->failures == failures);
   stage = "healthy-g2-publication";
   static_cast<void>(fixture.publish(source, "current-owner-delivers-after-stale-frame"));
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().messages_delivered == 1U; }));
   stage = "joined-cleanup";
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_CASE(control_native_gate_supersession_keeps_healthy_peer_and_score_ticks_live) { forge::net::p2p::node_session_fixture::native_gate_supersession(); }
BOOST_AUTO_TEST_CASE(control_native_snapshot_await_supersession_skips_delayed_graft) { forge::net::p2p::node_session_fixture::native_snapshot_supersession(); }
BOOST_AUTO_TEST_CASE(control_native_dispatch_late_iwant_activation_does_not_resurrect_fulfilled) { forge::net::p2p::node_session_fixture::native_dispatch_request(false); }
BOOST_AUTO_TEST_CASE(control_native_dispatch_stop_aborts_staged_iwant) { forge::net::p2p::node_session_fixture::native_dispatch_request(true); }
BOOST_AUTO_TEST_CASE(control_native_retired_generation_write_failure_does_not_penalize_new_lifetime) { forge::net::p2p::node_session_fixture::native_control_failure_generation(true); }
BOOST_AUTO_TEST_CASE(control_native_current_generation_write_failure_and_terminal_failure_are_attributed) { forge::net::p2p::node_session_fixture::native_control_failure_generation(false); }
BOOST_AUTO_TEST_CASE(control_native_blocked_ihave_keeps_healthy_graft_and_ticks_live_without_gossip_retry) { forge::net::p2p::node_session_fixture::native_blocked_ihave_expiry(); }
BOOST_AUTO_TEST_CASE(control_native_direct_publish_old_failure_does_not_penalize_actual_reconnected_peer) { forge::net::p2p::node_session_fixture::native_direct_publish_failure_generation(true); }
BOOST_AUTO_TEST_CASE(control_native_direct_publish_current_failure_is_attributed_once) { forge::net::p2p::node_session_fixture::native_direct_publish_failure_generation(false); }
BOOST_AUTO_TEST_CASE(control_native_protocol_open_old_failure_does_not_penalize_actual_reconnected_peer) { forge::net::p2p::node_session_fixture::native_protocol_open_failure_generation(true); }
BOOST_AUTO_TEST_CASE(control_native_protocol_open_current_timeout_is_attributed_once) { forge::net::p2p::node_session_fixture::native_protocol_open_failure_generation(false); }
BOOST_AUTO_TEST_CASE(control_native_pre_io_stream_quota_pressure_preserves_mesh_and_sticky_score_then_recovers) { forge::net::p2p::node_session_fixture::native_pre_io_stream_quota(); }
BOOST_AUTO_TEST_CASE(control_native_late_inbound_retired_authenticated_owner_cannot_resurrect_peer) { forge::net::p2p::node_session_fixture::native_retired_inbound(); }
BOOST_AUTO_TEST_CASE(control_native_late_announce_retired_owner_preserves_retention_without_route) { forge::net::p2p::node_session_fixture::native_retired_announce(); }
BOOST_AUTO_TEST_CASE(control_native_live_announce_retries_admission_after_capacity_frees) { forge::net::p2p::node_session_fixture::native_announce_capacity_retry(); }
BOOST_AUTO_TEST_CASE(control_native_retired_rpc_cannot_remove_reconnected_subscription_mesh_or_deliver_message) { forge::net::p2p::node_session_fixture::native_retired_rpc(); }
