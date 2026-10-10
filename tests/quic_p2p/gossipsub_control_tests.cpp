module;

#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;

#include "pubsub_router_fixture.hxx"
#include "gossipsub_validation_tests.hxx"
#include "gossipsub_test_shutdown.hxx"
#include "gossipsub_control_tests.hxx"

namespace forge::tests::p2p::gossipsub_control_tests {

using namespace forge::net::p2p;
using namespace forge::tests::p2p::gossipsub_validation_tests;
using namespace std::chrono_literals;

pubsub::options control_options(const pubsub::topic& topic) {
   auto config = native_options(topic);
   config.limits.mesh_n_low = 1;
   config.limits.heartbeat_initial_delay = 60s;
   config.limits.heartbeat_interval = 100ms;
   config.scoring->behaviour_penalty_threshold = 0;
   return config;
}

std::size_t rpc_count(const pubsub_router_fixture& state, const node& owner, const peer_id& peer,
                     pubsub::trace_kind kind, const std::function<bool(const pubsub::rpc&)>& match) {
   const auto rpcs = read_rpcs(state, owner, peer, kind);
   return static_cast<std::size_t>(std::ranges::count_if(rpcs, match));
}

std::size_t control_count(const pubsub_router_fixture& state, const node& owner, const peer_id& peer,
                         pubsub::trace_kind kind, bool prune) {
   auto count = std::size_t{};
   for (const auto& rpc : read_rpcs(state, owner, peer, kind)) {
      if (!rpc.control_value) { continue; }
      if (prune) {
         count += static_cast<std::size_t>(std::ranges::count_if(rpc.control_value->prunes,
             [&](const auto& entry) { return entry.subject == state.topic; }));
      } else {
         count += static_cast<std::size_t>(std::ranges::count_if(rpc.control_value->grafts,
             [&](const auto& entry) { return entry.subject == state.topic; }));
      }
   }
   return count;
}

std::size_t announcement_count(const pubsub_router_fixture& state, const node& owner, const peer_id& peer) {
   return rpc_count(state, owner, peer, pubsub::trace_kind::rpc_write, [](const auto& rpc) {
      return std::ranges::any_of(rpc.subscriptions, [](const auto& entry) { return entry.subscribe; });
   });
}

std::optional<pubsub_router_receipt> last_prune(const pubsub_router_fixture& state, const node& owner,
                                              const peer_id& peer, pubsub::trace_kind kind) {
   auto result = std::optional<pubsub_router_receipt>{};
   for (const auto& event : state.receipts(owner)) {
      if (event.kind != kind || event.peer != peer || event.frame.empty()) { continue; }
      const auto rpc = pubsub::codec::decode(event.frame);
      if (rpc.control_value && std::ranges::any_of(rpc.control_value->prunes,
          [&](const auto& entry) { return entry.subject == state.topic && entry.backoff.count() > 0; })) {
         BOOST_REQUIRE(event.session != 0 && event.stream >= 0 && event.generation != 0);
         result = event;
      }
   }
   return result;
}

void connect_announced(pubsub_router_fixture& state, node& source, node& target) {
   state.connect(source, target);
   BOOST_REQUIRE(state.wait([&] {
      const auto subscribed = [&](const node& owner, const peer_id& peer) {
         return rpc_count(state, owner, peer, pubsub::trace_kind::rpc_read, [&](const auto& rpc) {
            return std::ranges::any_of(rpc.subscriptions,
                [&](const auto& entry) { return entry.subscribe && entry.subject == state.topic; });
         }) != 0U;
      };
      return authenticated_gossipsub_peer(source, target.local_peer()) &&
             authenticated_gossipsub_peer(target, source.local_peer()) &&
             subscribed(source, target.local_peer()) && subscribed(target, source.local_peer());
   }));
}

void expect_prune(pubsub_router_fixture& state, node& owner, node& remote, std::size_t before,
                  std::chrono::seconds backoff) {
   BOOST_REQUIRE(state.wait([&] {
      return control_count(state, owner, remote.local_peer(), pubsub::trace_kind::rpc_write, true) == before + 1U &&
             control_count(state, remote, owner.local_peer(), pubsub::trace_kind::rpc_read, true) == before + 1U &&
             !gossipsub_mesh_peer(remote, owner.local_peer(), state.topic);
   }));
   const auto written = last_prune(state, owner, remote.local_peer(), pubsub::trace_kind::rpc_write);
   const auto received = last_prune(state, remote, owner.local_peer(), pubsub::trace_kind::rpc_read);
   BOOST_REQUIRE(written && received);
   BOOST_TEST(written->frame == received->frame, boost::test_tools::per_element());
   const auto rpc = pubsub::codec::decode(written->frame);
   BOOST_TEST(std::ranges::any_of(rpc.control_value->prunes, [&](const auto& entry) {
      return entry.subject == state.topic && entry.backoff == backoff;
   }));
}

std::string malformed_diagnostics(const node& owner) {
   const auto snapshot = owner.diagnostics();
   const auto& metrics = snapshot.metrics;
   auto out = std::ostringstream{};
   out << "invalid=" << metrics.pubsub_invalid_messages << " protocol=" << metrics.protocol_rejections
       << " connection=" << metrics.connection_rejections << " active=" << metrics.active_sessions
       << " closed=" << metrics.sessions_closed << " denied_malformed=" << snapshot.resources.denied_malformed
       << " identify=" << snapshot.connections.retained_identify_attempts << " sessions=[";
   for (const auto& session : snapshot.sessions) {
      out << "{id=" << session.id << ",peer=" << session.remote_peer.to_string()
          << ",closed=" << session.closed << ",auth=" << static_cast<int>(session.authentication) << "}";
   }
   out << "]";
   return out.str();
}

void check_behaviour(const node& owner, const peer_id& peer, double penalty) {
   const auto scored = score_of(owner, peer);
   BOOST_TEST(scored.connected);
   BOOST_TEST(scored.behaviour_penalty == penalty);
   BOOST_TEST(scored.behaviour_score == -2.0 * penalty * penalty);
   BOOST_TEST(scored.topic_score == 0.0);
   BOOST_TEST(scored.value == scored.behaviour_score);
   for (const auto& topic : scored.topics) { BOOST_TEST(topic.invalid_message_deliveries == 0.0); }
}

void register_echo(node& owner) {
   owner.register_protocol_handler(builtins::echo, [](node::incoming_protocol_stream incoming)
       -> boost::asio::awaitable<void> {
      const auto payload = co_await incoming.stream.async_read_frame();
      co_await incoming.stream.async_write_frame(payload);
      co_await incoming.stream.async_close();
   });
}

void echo(pubsub_router_fixture& state, node& source, node& target) {
   auto channel = std::make_shared<stream>(forge::asio::blocking::run(state.runtime,
       source.async_open_protocol_stream(target.local_peer(), builtins::echo,
           node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false})));
   const auto authenticated = channel->authentication() != peer_authentication::unverified;
   BOOST_REQUIRE(authenticated);
   const auto payload = std::vector<std::uint8_t>{'l', 'i', 'v', 'e'};
   auto reply = boost::asio::co_spawn(state.runtime.context(), [channel, payload]()
       -> boost::asio::awaitable<std::vector<std::uint8_t>> {
      co_await channel->async_write_frame(payload);
      auto result = co_await channel->async_read_frame();
      co_await channel->async_close();
      co_return result;
   }, boost::asio::use_future);
   if (reply.wait_for(3s) != std::future_status::ready) {
      channel->request_cancel();
      if (reply.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
      BOOST_FAIL("native healthy ECHO did not finish within its deadline");
   }
   BOOST_TEST(reply.get() == payload, boost::test_tools::per_element());
}

void exercise_topic_capacity(bool violate_backoff) {
   auto state = pubsub_router_fixture{};
   auto config = control_options(state.topic);
   config.limits.max_peers_per_topic = 1;
   auto resources = resource_manager::limits{};
   resources.max_malformed_messages_per_peer = 1;
   const auto branch = std::string{violate_backoff ? "violation" : "expiry"};
   auto& server = state.add("graft-capacity-server-" + branch, config, resources);
   auto& occupying = state.add("graft-capacity-occupying-" + branch, control_options(state.topic));
   auto& rejected = state.add("graft-capacity-rejected-" + branch, control_options(state.topic));
   auto occupying_shutdown = gossipsub_test_shutdown{state.runtime, occupying, server};
   auto rejected_shutdown = gossipsub_test_shutdown{state.runtime, rejected, server};
   register_echo(server);
   state.subscribe(server);
   state.subscribe(occupying);
   const auto warmup = pubsub::topic{"forge.graft.capacity.warmup"};
   static_cast<void>(forge::asio::blocking::run(state.runtime, rejected.async_subscribe(warmup,
       [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       })));
   connect_announced(state, occupying, server);
   state.connect(rejected, server);
   BOOST_REQUIRE(state.wait([&] {
      return authenticated_gossipsub_peer(server, rejected.local_peer()) &&
             authenticated_gossipsub_peer(rejected, server.local_peer()) &&
             rpc_count(state, server, rejected.local_peer(), pubsub::trace_kind::rpc_read, [&](const auto& rpc) {
                return std::ranges::any_of(rpc.subscriptions,
                    [&](const auto& entry) { return entry.subscribe && entry.subject == warmup; });
             }) != 0U && announcement_count(state, server, rejected.local_peer()) != 0U;
   }));
   auto occupying_stream = state.open(occupying, server);
   auto rejected_stream = state.open(rejected, server);
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 2U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 0U);
   const auto subscribe = pubsub::rpc{.subscriptions = {{.subscribe = true, .subject = state.topic}}};
   const auto announcements = announcement_count(state, server, rejected.local_peer());
   const auto reverse_announcements = announcement_count(state, rejected, server.local_peer());
   const auto before = server.metrics();
   // Duplicate admitted SUBSCRIBE is idempotent, while a new subscription at the local cap is pressure.
   state.send(occupying_stream, subscribe);
   read_barrier(state, occupying_stream, server, occupying.local_peer());
   BOOST_TEST(server.metrics().backpressure_rejections == before.backpressure_rejections);
   state.send(rejected_stream, subscribe);
   read_barrier(state, rejected_stream, server, rejected.local_peer());
   BOOST_TEST(server.metrics().backpressure_rejections == before.backpressure_rejections + 1U);
   BOOST_TEST(server.metrics().protocol_rejections == before.protocol_rejections);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().active_sessions == before.active_sessions);
   BOOST_TEST(server.pubsub_snapshot().peers == 2U);
   check_behaviour(server, rejected.local_peer(), 0);
   const auto graft = pubsub::rpc{.control_value = pubsub::control{.grafts = {{.subject = state.topic}}}};
   const auto controls = server.pubsub_snapshot().control_messages;
   const auto remote_controls = rejected.pubsub_snapshot().control_messages;
   const auto remote_peers = rejected.pubsub_snapshot().peers;
   state.send(rejected_stream, graft);
   read_barrier(state, rejected_stream, server, rejected.local_peer());
   expect_prune(state, server, rejected, 0);
   BOOST_TEST(server.pubsub_snapshot().control_messages == controls + 1U);
   BOOST_TEST(rejected.pubsub_snapshot().control_messages == remote_controls + 1U);
   BOOST_TEST(server.pubsub_snapshot().peers == 2U);
   BOOST_TEST(rejected.pubsub_snapshot().peers == remote_peers);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(rejected.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(server.metrics().protocol_streams_opened == before.protocol_streams_opened);
   BOOST_TEST(server.metrics().backpressure_rejections == before.backpressure_rejections + 1U);
   check_behaviour(server, rejected.local_peer(), 0);
   if (violate_backoff) {
      state.send(rejected_stream, graft);
      read_barrier(state, rejected_stream, server, rejected.local_peer());
      expect_prune(state, server, rejected, 1);
      check_behaviour(server, rejected.local_peer(), 1);
   }
   BOOST_TEST(announcement_count(state, server, rejected.local_peer()) == announcements);
   BOOST_TEST(announcement_count(state, rejected, server.local_peer()) == reverse_announcements);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().protocol_rejections == before.protocol_rejections);
   BOOST_TEST(server.metrics().active_sessions == before.active_sessions);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   echo(state, rejected, server);
   forge::asio::blocking::run(state.runtime, occupying.async_unsubscribe(state.topic));
   BOOST_REQUIRE(state.wait([&] { return server.pubsub_snapshot().peers == 1U; }));
   // A fresh fixture's no-violation branch proves expiry admission independently of retained P7.
   const auto due = std::chrono::steady_clock::now() + config.limits.prune_backoff;
   BOOST_REQUIRE(state.wait([&] { return std::chrono::steady_clock::now() >= due; }));
   const auto prunes = control_count(state, server, rejected.local_peer(), pubsub::trace_kind::rpc_write, true);
   state.send(rejected_stream, graft);
   read_barrier(state, rejected_stream, server, rejected.local_peer());
   if (violate_backoff) {
      expect_prune(state, server, rejected, prunes);
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
      check_behaviour(server, rejected.local_peer(), 1);
   } else {
      BOOST_REQUIRE(gossipsub_mesh_peer(server, rejected.local_peer(), state.topic));
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);
      BOOST_TEST(control_count(state, server, rejected.local_peer(), pubsub::trace_kind::rpc_write, true) == prunes);
      check_behaviour(server, rejected.local_peer(), 0);
   }
   BOOST_TEST(announcement_count(state, server, rejected.local_peer()) == announcements);
   BOOST_TEST(announcement_count(state, rejected, server.local_peer()) == reverse_announcements);
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages);
   forge::asio::blocking::run(state.runtime, occupying_stream.async_close());
   forge::asio::blocking::run(state.runtime, rejected_stream.async_close());
   rejected_shutdown.join();
   occupying_shutdown.join();
}

void exercise_backoff(bool leave, bool violate_backoff, std::string_view listen_address) {
   auto state = pubsub_router_fixture{};
   auto config = control_options(state.topic);
   config.limits.unsubscribe_backoff = 1s;
   const auto branch = std::string{violate_backoff ? "violation" : "expiry"};
   auto& server = state.add("native-backoff-server-" + branch, config, {}, listen_address);
   auto& client = state.add("native-backoff-client-" + branch, config, {}, listen_address);
   auto shutdown = gossipsub_test_shutdown{state.runtime, server, client};
   state.subscribe(server);
   state.subscribe(client);
   connect_announced(state, client, server);
   auto channel = state.open(client, server);
   const auto graft = pubsub::rpc{.control_value = pubsub::control{.grafts = {{.subject = state.topic}}}};
   if (leave) {
      state.send(channel, graft);
      read_barrier(state, channel, server, client.local_peer());
      BOOST_REQUIRE(gossipsub_mesh_peer(server, client.local_peer(), state.topic));
      const auto message = state.publish(server, "leave-warmup");
      BOOST_REQUIRE(state.wait([&] { return committed(state, client, pubsub::codec::message_id(message),
          pubsub::validation_result::accept) && client.pubsub_snapshot().messages_delivered == 1U; }));
      const auto streams = server.metrics().protocol_streams_opened;
      const auto remote_streams = client.metrics().protocol_streams_opened;
      const auto controls = client.pubsub_snapshot().control_messages;
      forge::asio::blocking::run(state.runtime, server.async_unsubscribe(state.topic));
      expect_prune(state, server, client, 0);
      BOOST_REQUIRE(state.wait([&] { return client.pubsub_snapshot().peers == 0U; }));
      const auto prune_frame = last_prune(state, client, server.local_peer(), pubsub::trace_kind::rpc_read);
      BOOST_REQUIRE(prune_frame);
      auto leave_frame = std::optional<pubsub_router_receipt>{};
      auto leaves = std::size_t{};
      for (const auto& event : state.receipts(client)) {
         if (event.kind != pubsub::trace_kind::rpc_read || event.peer != server.local_peer() || event.frame.empty()) {
            continue;
         }
         const auto rpc = pubsub::codec::decode(event.frame);
         if (std::ranges::any_of(rpc.subscriptions, [&](const auto& entry) {
                return !entry.subscribe && entry.subject == state.topic;
             })) {
            leave_frame = event;
            ++leaves;
         }
      }
      // Separate RPCs are valid, but both must reach the same authenticated stream exactly once.
      BOOST_REQUIRE(leave_frame);
      BOOST_TEST(leaves == 1U);
      BOOST_TEST(leave_frame->session == prune_frame->session);
      BOOST_TEST(leave_frame->stream == prune_frame->stream);
      BOOST_TEST(leave_frame->generation == prune_frame->generation);
      BOOST_TEST(client.pubsub_snapshot().control_messages == controls + 1U);
      BOOST_TEST(client.pubsub_snapshot().mesh_edges == 0U);
      BOOST_TEST(server.pubsub_snapshot().topics == 0U);
      BOOST_TEST(server.pubsub_snapshot().peers == 1U);
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
      BOOST_TEST(server.metrics().protocol_streams_opened == streams);
      BOOST_TEST(client.metrics().protocol_streams_opened == remote_streams);
      state.subscribe(server);
      BOOST_REQUIRE(state.wait([&] { return client.pubsub_snapshot().peers == 1U; }));
   } else {
      const auto controls = server.pubsub_snapshot().control_messages;
      state.send(channel, pubsub::rpc{.control_value = pubsub::control{
          .prunes = {{.subject = state.topic, .backoff = 1s}}}});
      read_barrier(state, channel, server, client.local_peer());
      BOOST_TEST(server.pubsub_snapshot().control_messages == controls + 1U);
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   }
   const auto before = server.metrics();
   const auto peer_count = server.pubsub_snapshot().peers;
   const auto client_peers = client.pubsub_snapshot().peers;
   const auto client_mesh = client.pubsub_snapshot().mesh_edges;
   const auto announcements = announcement_count(state, server, client.local_peer());
   const auto reverse_announcements = announcement_count(state, client, server.local_peer());
   if (violate_backoff) {
      const auto prunes = control_count(state, server, client.local_peer(), pubsub::trace_kind::rpc_write, true);
      const auto controls = server.pubsub_snapshot().control_messages;
      const auto remote_controls = client.pubsub_snapshot().control_messages;
      state.send(channel, graft);
      read_barrier(state, channel, server, client.local_peer());
      expect_prune(state, server, client, prunes);
      BOOST_TEST(server.pubsub_snapshot().control_messages == controls + 1U);
      BOOST_TEST(client.pubsub_snapshot().control_messages == remote_controls + 1U);
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
      BOOST_TEST(server.pubsub_snapshot().peers == peer_count);
      BOOST_TEST(client.pubsub_snapshot().peers == client_peers);
      BOOST_TEST(client.pubsub_snapshot().mesh_edges == client_mesh);
      check_behaviour(server, client.local_peer(), 1);
   }
   const auto due = std::chrono::steady_clock::now() + config.limits.prune_backoff;
   BOOST_REQUIRE(state.wait([&] { return std::chrono::steady_clock::now() >= due; }));
   const auto prunes = control_count(state, server, client.local_peer(), pubsub::trace_kind::rpc_write, true);
   const auto controls = server.pubsub_snapshot().control_messages;
   const auto remote_controls = client.pubsub_snapshot().control_messages;
   state.send(channel, graft);
   read_barrier(state, channel, server, client.local_peer());
   BOOST_TEST(server.pubsub_snapshot().control_messages == controls + 1U);
   if (violate_backoff) {
      expect_prune(state, server, client, prunes);
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
      BOOST_TEST(client.pubsub_snapshot().control_messages == remote_controls + 1U);
      check_behaviour(server, client.local_peer(), 1);
   } else {
      BOOST_REQUIRE(gossipsub_mesh_peer(server, client.local_peer(), state.topic));
      BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);
      BOOST_TEST(client.pubsub_snapshot().control_messages == remote_controls);
      BOOST_TEST(control_count(state, server, client.local_peer(), pubsub::trace_kind::rpc_write, true) == prunes);
      check_behaviour(server, client.local_peer(), 0);
   }
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().protocol_rejections == before.protocol_rejections);
   BOOST_TEST(server.metrics().protocol_streams_opened == before.protocol_streams_opened);
   BOOST_TEST(server.metrics().active_sessions == before.active_sessions);
   BOOST_TEST(announcement_count(state, server, client.local_peer()) == announcements);
   BOOST_TEST(announcement_count(state, client, server.local_peer()) == reverse_announcements);
   forge::asio::blocking::run(state.runtime, channel.async_close());
   shutdown.join();
}

void graft_at_topic_subscription_cap_sends_prune_without_state_or_subscribe_loop() {
   exercise_topic_capacity(true);
   exercise_topic_capacity(false);
}

void received_prune_blocks_immediate_graft_until_backoff_expiry() {
   for (const auto address : {"/ip4/127.0.0.1/tcp/0", "/ip4/127.0.0.1/udp/0/quic-v1"}) {
      exercise_backoff(false, true, address);
      exercise_backoff(false, false, address);
   }
}

void unsubscribe_sends_leave_and_enforces_backoff_until_expiry() {
   for (const auto address : {"/ip4/127.0.0.1/tcp/0", "/ip4/127.0.0.1/udp/0/quic-v1"}) {
      exercise_backoff(true, true, address);
      exercise_backoff(true, false, address);
   }
}

void heartbeat_obeys_sent_and_received_prune_backoff_until_expiry() {
   auto nodes = std::array<std::atomic<node*>, 3>{};
   auto bilateral_at = std::atomic<std::chrono::steady_clock::duration::rep>{0};
   auto state = pubsub_router_fixture{};
   auto config = control_options(state.topic);
   config.limits.mesh_n = config.limits.mesh_n_low = config.limits.mesh_n_high = 1;
   config.limits.mesh_outbound_min = 0;
   config.limits.mesh_score_min = 1;
   config.limits.prune_backoff = 7s;
   config.limits.heartbeat_initial_delay = 2s;
   config.limits.heartbeat_interval = 400ms;
   auto remote_config = config;
   remote_config.limits.mesh_n = 2;
   remote_config.limits.mesh_n_high = 4;
   remote_config.limits.heartbeat_initial_delay = 2'050ms;
   remote_config.limits.heartbeat_interval = 50ms;
   config.scoring->app_specific_score = [&](const peer_id&) {
      const auto observed_before = std::chrono::steady_clock::now().time_since_epoch().count();
      const auto owner = nodes[0].load();
      const auto a = nodes[1].load();
      const auto b = nodes[2].load();
      if (owner && a && b && owner->pubsub_snapshot().mesh_edges == 2U &&
          gossipsub_mesh_peer(*a, owner->local_peer(), state.topic) &&
          gossipsub_mesh_peer(*b, owner->local_peer(), state.topic)) {
         auto unset = std::chrono::steady_clock::duration::rep{0};
         bilateral_at.compare_exchange_strong(unset, observed_before);
      }
      return 0.0;
   };
   const auto address = "/ip4/127.0.0.1/udp/0/quic-v1";
   auto& server = state.add("heartbeat-backoff-server", config, {}, address);
   auto& first = state.add("heartbeat-backoff-first", remote_config, {}, address);
   auto& second = state.add("heartbeat-backoff-second", remote_config, {}, address);
   nodes[0].store(&server);
   nodes[1].store(&first);
   nodes[2].store(&second);
   auto first_shutdown = gossipsub_test_shutdown{state.runtime, first, server};
   auto second_shutdown = gossipsub_test_shutdown{state.runtime, second, server};
   state.subscribe(server);
   state.subscribe(first);
   state.subscribe(second);
   // Native GRAFTs establish both sides before the server's next heartbeat trims the oversized mesh.
   connect_ready(state, server, first);
   connect_announced(state, server, second);
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 2U);
   BOOST_REQUIRE(state.wait([&] {
      return control_count(state, server, first.local_peer(), pubsub::trace_kind::rpc_write, true) +
             control_count(state, server, second.local_peer(), pubsub::trace_kind::rpc_write, true) == 1U;
   }));
   BOOST_REQUIRE(bilateral_at.load() != 0);
   const auto before_prune = std::chrono::steady_clock::time_point{
       std::chrono::steady_clock::duration{bilateral_at.load()}};
   for (const auto remote : {&first, &second}) {
      auto native_graft = false;
      for (const auto source : {remote, &server}) {
         if (remote == &second && source == &server) { continue; }
         const auto& target = source == &server ? *remote : server;
         for (const auto& event : state.receipts(*source)) {
            if (event.kind != pubsub::trace_kind::rpc_write || event.peer != target.local_peer() || event.frame.empty()) {
               continue;
            }
            const auto rpc = pubsub::codec::decode(event.frame);
            if (rpc.control_value && std::ranges::any_of(rpc.control_value->grafts,
                [&](const auto& entry) { return entry.subject == state.topic; })) {
               BOOST_REQUIRE(event.session != 0 && event.stream >= 0 && event.generation != 0);
               const auto received = state.receipts(target);
               native_graft = std::ranges::any_of(received, [&](const auto& receipt) {
                  return receipt.kind == pubsub::trace_kind::rpc_read && receipt.peer == source->local_peer() &&
                         receipt.frame == event.frame && receipt.session != 0 && receipt.stream >= 0 && receipt.generation != 0;
               });
               if (native_graft) { break; }
            }
         }
         if (native_graft) { break; }
      }
      BOOST_REQUIRE(native_graft);
   }
   const auto first_prune = last_prune(state, server, first.local_peer(), pubsub::trace_kind::rpc_write);
   auto* pruned = first_prune ? &first : &second;
   auto* retained = first_prune ? &second : &first;
   expect_prune(state, server, *pruned, 0, config.limits.prune_backoff);
   const auto prune = last_prune(state, server, pruned->local_peer(), pubsub::trace_kind::rpc_write);
   const auto received_prune = last_prune(state, *pruned, server.local_peer(), pubsub::trace_kind::rpc_read);
   BOOST_REQUIRE(prune && received_prune);
   const auto initial_grafts = control_count(state, server, pruned->local_peer(), pubsub::trace_kind::rpc_write, false) +
                               control_count(state, *pruned, server.local_peer(), pubsub::trace_kind::rpc_write, false);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(!gossipsub_mesh_peer(server, pruned->local_peer(), state.topic));
   BOOST_TEST(gossipsub_mesh_peer(server, retained->local_peer(), state.topic));
   const auto sessions = server.metrics().active_sessions;
   BOOST_REQUIRE(sessions == 2U);
   forge::asio::blocking::run(state.runtime, retained->async_stop());
   BOOST_REQUIRE(state.wait([&] {
      return server.metrics().active_sessions == 1U && server.pubsub_snapshot().peers == 1U &&
             !authenticated_gossipsub_peer(server, retained->local_peer());
   }));
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 0U);
   const auto controls = server.pubsub_snapshot().control_messages;
   const auto remote_controls = pruned->pubsub_snapshot().control_messages;
   const auto invalid = server.pubsub_snapshot().invalid_messages;
   const auto local_after = before_prune + config.limits.prune_backoff + 2 * config.limits.heartbeat_interval;
   const auto remote_after = received_prune->observed + config.limits.prune_backoff +
                             2 * remote_config.limits.heartbeat_interval;
   const auto safe_after = std::max(local_after, remote_after);
   const auto report = [&](std::string_view phase) {
      auto out = std::ostringstream{};
      out << "heartbeat/backoff phase=" << phase << " server=" << server.local_peer().to_string()
          << " pruned=" << pruned->local_peer().to_string()
          << " retained=" << retained->local_peer().to_string() << '\n';
      for (const auto owner : {&server, pruned}) {
         const auto peer = owner == &server ? pruned->local_peer() : server.local_peer();
         const auto score = score_of(*owner, peer);
         out << "owner=" << owner->local_peer().to_string() << " penalty=" << score.behaviour_penalty
             << " score=" << score.value << " mesh=" << gossipsub_mesh_peer(*owner, peer, state.topic) << '\n';
         auto shown = std::size_t{};
         for (const auto& event : state.receipts(*owner)) {
            if (event.peer != peer || event.frame.empty() || ++shown > 128U) { continue; }
            const auto rpc = pubsub::codec::decode(event.frame);
            if (!rpc.control_value) { continue; }
            out << "rpc kind=" << static_cast<int>(event.kind)
                << " at_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(event.observed.time_since_epoch()).count()
                << " session=" << event.session << " stream=" << event.stream << " generation=" << event.generation
                << " grafts=" << rpc.control_value->grafts.size() << " prunes=" << rpc.control_value->prunes.size() << '\n';
         }
      }
      std::cerr << out.str();
   };
   BOOST_REQUIRE(state.wait([&] { return std::chrono::steady_clock::now() >= safe_after; }, 8s));
   const auto check_graft_history = [&](const node& owner, const peer_id& peer,
                                       const pubsub_router_receipt& boundary,
                                       std::chrono::steady_clock::time_point eligible_after) {
      for (const auto& event : state.receipts(owner)) {
         if (event.kind != pubsub::trace_kind::rpc_write || event.peer != peer ||
             event.observed < boundary.observed || event.frame.empty()) {
            continue;
         }
         const auto rpc = pubsub::codec::decode(event.frame);
         if (rpc.control_value && std::ranges::any_of(rpc.control_value->grafts,
             [&](const auto& entry) { return entry.subject == state.topic; })) {
            BOOST_CHECK(event.observed >= eligible_after);
         }
      }
   };
   check_graft_history(server, pruned->local_peer(), *prune, local_after);
   check_graft_history(*pruned, server.local_peer(), *received_prune, remote_after);
   // Other legal control frames are not mesh transitions. Inspect their actual
   // GRAFT timestamps, rather than treating a short quiet sample as the gate.
   BOOST_TEST(server.pubsub_snapshot().control_messages >= controls);
   BOOST_TEST(pruned->pubsub_snapshot().control_messages >= remote_controls);
   BOOST_TEST(server.pubsub_snapshot().invalid_messages == invalid);
   if (score_of(server, pruned->local_peer()).behaviour_penalty != 0 ||
       score_of(*pruned, server.local_peer()).behaviour_penalty != 0) { report("before-rejoin"); }
   check_behaviour(server, pruned->local_peer(), 0);
   check_behaviour(*pruned, server.local_peer(), 0);
   const auto restored = state.wait([&] {
      return gossipsub_mesh_peer(server, pruned->local_peer(), state.topic) &&
             gossipsub_mesh_peer(*pruned, server.local_peer(), state.topic);
   }, 8s);
   if (!restored) { report("rejoin-timeout"); }
   BOOST_REQUIRE(restored);
   check_graft_history(server, pruned->local_peer(), *prune, local_after);
   check_graft_history(*pruned, server.local_peer(), *received_prune, remote_after);
   const auto grafts = control_count(state, server, pruned->local_peer(), pubsub::trace_kind::rpc_write, false) +
                       control_count(state, *pruned, server.local_peer(), pubsub::trace_kind::rpc_write, false) - initial_grafts;
   BOOST_TEST(grafts >= 1U);
   BOOST_TEST(grafts <= 2U); // Concurrent native GRAFTs are idempotent, not an ordering assumption.
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(pruned->pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(pruned->pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(server.pubsub_snapshot().invalid_messages == invalid);
   check_behaviour(server, pruned->local_peer(), 0);
   check_behaviour(*pruned, server.local_peer(), 0);
   second_shutdown.join();
   first_shutdown.join();
}

void control_spam_is_penalized_without_stopping_node() {
   auto state = pubsub_router_fixture{};
   auto config = control_options(state.topic);
   config.limits.max_ihave_per_peer = config.limits.max_iwant_per_peer = config.limits.max_graft_per_peer = 1;
   config.limits.max_messages = 1;
   config.limits.gossip_retransmission = 1;
   auto& server = state.add("valid-control-limit-server", config);
   auto& client = state.add("valid-control-limit-client", control_options(state.topic));
   auto shutdown = gossipsub_test_shutdown{state.runtime, server, client};
   register_echo(server);
   state.subscribe(server);
   state.subscribe(client);
   const auto first = state.publish(server, "cached-first");
   const auto second = state.publish(server, "cached-second");
   const auto first_id = pubsub::codec::message_id(first);
   const auto second_id = pubsub::codec::message_id(second);
   connect_announced(state, client, server);
   auto channel = state.open(client, server);
   const auto before = server.metrics();
   const auto first_missing = std::vector<std::uint8_t>{'a'};
   const auto second_missing = std::vector<std::uint8_t>{'b'};
   const auto excess = pubsub::rpc{.control_value = pubsub::control{
       .have = {{.subject = state.topic, .message_ids = {first_missing}},
                {.subject = state.topic, .message_ids = {second_missing}}},
       .want = {{.message_ids = {first_id}}, {.message_ids = {second_id}}},
       .grafts = {{.subject = state.topic}, {.subject = pubsub::topic{"forge.control.excess"}}}}};
   state.send(channel, excess);
   read_barrier(state, channel, server, client.local_peer());
   BOOST_REQUIRE(state.wait([&] { return committed(state, client, first_id, pubsub::validation_result::accept) &&
       client.pubsub_snapshot().messages_delivered == 1U; }));
   state.send(channel, excess);
   read_barrier(state, channel, server, client.local_peer());
   auto wants = std::size_t{};
   auto served = std::size_t{};
   for (const auto& rpc : read_rpcs(state, server, client.local_peer(), pubsub::trace_kind::rpc_write)) {
      if (rpc.control_value) {
         for (const auto& want : rpc.control_value->want) {
            wants += static_cast<std::size_t>(std::ranges::count(want.message_ids, first_missing));
            BOOST_TEST(std::ranges::count(want.message_ids, second_missing) == 0);
         }
      }
      for (const auto& message : rpc.messages) {
         BOOST_TEST(pubsub::codec::message_id(message) == first_id, boost::test_tools::per_element());
         BOOST_TEST(pubsub::codec::verify_message(message));
         ++served;
      }
   }
   BOOST_TEST(wants == 1U);
   BOOST_TEST(served == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   // Keep the legacy registration name, but valid control throttling must never become malformed/P4/P7.
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().protocol_rejections == before.protocol_rejections);
   BOOST_TEST(server.metrics().backpressure_rejections == before.backpressure_rejections);
   BOOST_TEST(server.metrics().active_sessions == before.active_sessions);
   BOOST_TEST(!server.metrics().stopped);
   check_behaviour(server, client.local_peer(), 0);
   check_behaviour(client, server.local_peer(), 0);
   echo(state, client, server);
   forge::asio::blocking::run(state.runtime, channel.async_close());
   shutdown.join();
}

void abusive_peer_crossing_malformed_threshold_closes_only_offender_session() {
   auto state = pubsub_router_fixture{};
   auto resources = resource_manager::limits{};
   resources.max_malformed_messages_per_peer = 1;
   auto& server = state.add("malformed-isolation-server", control_options(state.topic), resources);
   auto& bad = state.add("malformed-isolation-bad", control_options(state.topic));
   auto& good = state.add("malformed-isolation-good", control_options(state.topic));
   auto bad_shutdown = gossipsub_test_shutdown{state.runtime, bad, server};
   auto good_shutdown = gossipsub_test_shutdown{state.runtime, good, server};
   register_echo(server);
   state.subscribe(server);
   state.subscribe(bad);
   state.subscribe(good);
   connect_announced(state, bad, server);
   connect_announced(state, good, server);
   auto healthy_session = std::uint64_t{};
   for (const auto& session : server.diagnostics().sessions) {
      if (!session.closed && session.remote_peer == good.local_peer()) { healthy_session = session.id; }
   }
   BOOST_REQUIRE(healthy_session != 0U);
   const auto before = server.metrics();
   BOOST_REQUIRE(before.active_sessions == 2U);
   const auto diagnostic_before = malformed_diagnostics(server);
   auto first = state.open(bad, server);
   const auto first_id = first.id();
   const auto malformed = std::vector<std::uint8_t>{0x02U, 0x18U, 0x00U};
   forge::asio::blocking::run(state.runtime, first.async_write(malformed));
   BOOST_REQUIRE(state.wait([&] {
      return server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages + 1U &&
             server.metrics().protocol_rejections == before.protocol_rejections + 1U;
   }));
   BOOST_REQUIRE(authenticated_gossipsub_peer(server, bad.local_peer()));
   BOOST_TEST(server.metrics().active_sessions == 2U);
   auto second = state.open(bad, server);
   BOOST_REQUIRE(second.id() != first_id);
   forge::asio::blocking::run(state.runtime, second.async_write(malformed));
   BOOST_REQUIRE_MESSAGE(state.wait([&] {
      return server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages + 2U &&
             server.metrics().protocol_rejections == before.protocol_rejections + 2U &&
             server.metrics().connection_rejections == before.connection_rejections + 1U &&
             server.metrics().active_sessions == 1U && !authenticated_gossipsub_peer(server, bad.local_peer());
   }), "malformed threshold before=" << diagnostic_before << "; after=" << malformed_diagnostics(server));
   BOOST_TEST(server.metrics().protocol_rejections == before.protocol_rejections + 2U);
   BOOST_TEST(server.metrics().sessions_closed == before.sessions_closed + 1U);
   BOOST_TEST(server.metrics().connection_rejections == before.connection_rejections + 1U);
   const auto after = server.diagnostics();
   BOOST_TEST(after.connections.retained_identify_attempts <= after.connections.active_sessions);
   BOOST_TEST(std::ranges::any_of(after.sessions, [&](const auto& session) {
      return session.id == healthy_session && !session.closed && session.remote_peer == good.local_peer() &&
             session.authentication != peer_authentication::unverified;
   }));
   // Decode failure has no valid topic, so it is malformed policy, not a topic's P4 or P7.
   check_behaviour(server, good.local_peer(), 0);
   for (const auto& scored : server.pubsub_scores().peers) {
      if (scored.peer != bad.local_peer()) { continue; }
      BOOST_TEST(!scored.connected);
      BOOST_TEST(scored.behaviour_penalty == 0.0);
      for (const auto& topic : scored.topics) { BOOST_TEST(topic.invalid_message_deliveries == 0.0); }
   }
   echo(state, good, server);
   BOOST_TEST(std::ranges::any_of(server.diagnostics().sessions, [&](const auto& session) {
      return session.id == healthy_session && !session.closed && session.remote_peer == good.local_peer();
   }));
   BOOST_TEST(!server.metrics().stopped);
   first.request_cancel();
   second.request_cancel();
   good_shutdown.join();
   bad_shutdown.join();
}

void active_registered_handler_error_is_charged_once_without_closing_session() {
   auto state = pubsub_router_fixture{};
   auto& server = state.add("active-handler-error-server", control_options(state.topic));
   auto& client = state.add("active-handler-error-client", control_options(state.topic));
   auto shutdown = gossipsub_test_shutdown{state.runtime, client, server};
   register_echo(server);
   const auto protocol = protocol_id{.value = "/forge/test/active-handler-error/1.0.0"};
   const auto payload = std::vector<std::uint8_t>{'e', 'r', 'r', 'o', 'r'};
   auto calls = std::make_shared<std::atomic_uint64_t>(0);
   auto observed = std::make_shared<std::promise<node::session_info>>();
   auto entered = observed->get_future();
   auto observed_stream = std::make_shared<std::atomic_int64_t>(-1);
   server.register_protocol_handler(protocol, [calls, observed, observed_stream, payload](node::incoming_protocol_stream incoming)
       -> boost::asio::awaitable<void> {
      const auto received = co_await incoming.stream.async_read_frame();
      if (received != payload) {
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::protocol_error,
             "active handler regression received an unexpected frame");
      }
      observed_stream->store(incoming.stream.id(), std::memory_order_relaxed);
      calls->fetch_add(1U, std::memory_order_relaxed);
      observed->set_value(incoming.session);
      // The type is orderly-close-like, but this failure is from a live handler, not terminal cleanup.
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::closed, "intentional active registered-handler failure");
   });
   state.subscribe(server);
   state.subscribe(client);
   connect_announced(state, client, server);
   echo(state, client, server);
   const auto sessions = server.diagnostics().sessions;
   BOOST_REQUIRE_EQUAL(sessions.size(), 1U);
   const auto session_id = sessions.front().id;
   BOOST_REQUIRE(session_id != 0U && !sessions.front().closed &&
                 sessions.front().authentication != peer_authentication::unverified);
   const auto before = server.metrics();
   const auto malformed = server.diagnostics().resources.denied_malformed;
   auto channel = forge::asio::blocking::run(state.runtime,
       client.async_open_protocol_stream(server.local_peer(), protocol,
           node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false}));
   BOOST_REQUIRE(channel.authentication() != peer_authentication::unverified);
   forge::asio::blocking::run(state.runtime, channel.async_write_frame(payload));
   BOOST_REQUIRE(entered.wait_for(5s) == std::future_status::ready);
   const auto incoming = entered.get();
   BOOST_TEST(incoming.id == session_id);
   BOOST_TEST((incoming.remote_peer == client.local_peer()));
   BOOST_TEST(observed_stream->load(std::memory_order_relaxed) == channel.id());
   BOOST_REQUIRE_MESSAGE(state.wait([&] {
      return server.metrics().protocol_rejections == before.protocol_rejections + 1U;
   }), malformed_diagnostics(server));
   channel.request_cancel();
   echo(state, client, server);
   BOOST_TEST(calls->load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(server.metrics().protocol_rejections == before.protocol_rejections + 1U);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().connection_rejections == before.connection_rejections);
   BOOST_TEST(server.metrics().sessions_closed == before.sessions_closed);
   BOOST_TEST(server.metrics().active_sessions == 1U);
   BOOST_TEST(server.diagnostics().resources.denied_malformed == malformed);
   BOOST_TEST(std::ranges::any_of(server.diagnostics().sessions, [&](const auto& session) {
      return session.id == session_id && !session.closed && session.remote_peer == client.local_peer() &&
             session.authentication != peer_authentication::unverified;
   }));
   BOOST_TEST(!server.metrics().stopped);
   check_behaviour(server, client.local_peer(), 0);
   shutdown.join();
}

} // namespace forge::tests::p2p::gossipsub_control_tests

BOOST_AUTO_TEST_CASE(p2p_gossipsub_graft_at_topic_subscription_cap_sends_prune_without_state_or_subscribe_loop) {
   forge::tests::p2p::gossipsub_control_tests::graft_at_topic_subscription_cap_sends_prune_without_state_or_subscribe_loop();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_received_prune_blocks_immediate_graft_until_backoff_expiry) {
   forge::tests::p2p::gossipsub_control_tests::received_prune_blocks_immediate_graft_until_backoff_expiry();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_unsubscribe_sends_leave_and_enforces_backoff_until_expiry) {
   forge::tests::p2p::gossipsub_control_tests::unsubscribe_sends_leave_and_enforces_backoff_until_expiry();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_heartbeat_obeys_sent_and_received_prune_backoff_until_expiry) {
   forge::tests::p2p::gossipsub_control_tests::heartbeat_obeys_sent_and_received_prune_backoff_until_expiry();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_control_spam_is_penalized_without_stopping_node) {
   forge::tests::p2p::gossipsub_control_tests::control_spam_is_penalized_without_stopping_node();
}

BOOST_AUTO_TEST_CASE(p2p_abusive_peer_crossing_malformed_threshold_closes_only_offender_session) {
   forge::tests::p2p::gossipsub_control_tests::abusive_peer_crossing_malformed_threshold_closes_only_offender_session();
}

BOOST_AUTO_TEST_CASE(p2p_active_registered_handler_error_is_charged_once_without_closing_session) {
   forge::tests::p2p::gossipsub_control_tests::active_registered_handler_error_is_charged_once_without_closing_session();
}
