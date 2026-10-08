module;

#include <forge/exceptions/macros.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <boost/asio/use_future.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.blocking;
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
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../../libraries/net/p2p/details/node_impl.hxx"
#include "../../libraries/net/p2p/details/pubsub_peer_score.hxx"
#include "../../libraries/net/p2p/details/pubsub_router.hxx"
#include "pubsub_router_fixture.hxx"
#include "gossipsub_test_shutdown.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;

void node_session_fixture::native_retired_validation(bool retry) {
   auto fixture = forge::tests::p2p::pubsub_router_fixture{};
   using shutdown_guard = forge::tests::p2p::gossipsub_test_shutdown;
   auto config = pubsub::options{};
   config.limits.heartbeat_initial_delay = 60s;
   config.limits.heartbeat_interval = 60s;
   config.scoring = pubsub::scoring_params{.behaviour_penalty_weight = -1.0};
   config.scoring->topics.emplace(fixture.topic, pubsub::topic_score_params{.first_message_deliveries_weight = 1.0});
   const auto raw_id = std::make_shared<std::atomic_int64_t>(-1);
   const auto after_request = std::make_shared<std::atomic_bool>(false);
   config.tracer = [raw_id, after_request, topic = fixture.topic, wire = config](const pubsub::trace_event& event) {
      if (event.kind != pubsub::trace_kind::rpc_read || event.stream_id != raw_id->load()) { return; }
      const auto rpc = pubsub::codec::decode(event.framed_rpc, wire);
      if (rpc.subscriptions.size() == 1 && rpc.subscriptions.front().subscribe &&
          rpc.subscriptions.front().subject == topic && !rpc.control_value && rpc.messages.empty()) {
         after_request->store(true); // Passive next-read marker; never holds a native executor.
      }
   };
   auto source_config = pubsub::options{};
   source_config.limits.heartbeat_initial_delay = 60s;
   source_config.limits.heartbeat_interval = 60s;
   auto& source = fixture.add("validation-source", source_config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& owner = fixture.add("validation-owner", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   const auto self = owner.impl_;
   const auto sender = source.impl_;
   const auto peer = source.local_peer();
   auto ticket = forge::asio::gate::ticket{};
   auto raw = stream{};
   const auto barrier = std::make_shared<forge::asio::notification>();
   const auto barrier_epoch = barrier->epoch();
   auto shutdown = shutdown_guard{fixture.runtime, source, owner, [&] {
      ticket.release();
      barrier->notify();
      raw.request_cancel();
   }};
   fixture.subscribe(source);
   const auto published = fixture.publish(source, "claimed-before-native-retirement");
   BOOST_REQUIRE(pubsub::codec::verify_message(published, config));
   const auto id = pubsub::codec::message_id(published, config);
   const auto key = bytes_key(id);
   const auto entered = std::make_shared<std::promise<pubsub::event>>();
   auto claimed = entered->get_future();
   const auto calls = std::make_shared<std::atomic_size_t>(0);
   fixture.subscribe(owner, [published, entered, calls, barrier, barrier_epoch, retry](pubsub::event event)
       -> boost::asio::awaitable<pubsub::validation_result> {
      ++*calls;
      if (event.value.from == published.from && event.value.seqno == published.seqno) {
         entered->set_value(event);
         co_await barrier->async_wait(barrier_epoch);
         co_return retry ? pubsub::validation_result::retry : pubsub::validation_result::accept;
      }
      co_return pubsub::validation_result::accept;
   });
   const auto subscribed = [&](const auto& impl, const peer_id& remote) {
      const auto lock = std::scoped_lock{impl->mutex};
      const auto topics = impl->pubsub_value.peer_topics.find(remote);
      const auto out = impl->pubsub_value.outbound.find(remote);
      return topics != impl->pubsub_value.peer_topics.end() && topics->second.contains(fixture.topic.value) &&
          out != impl->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          impl->pubsub_value.outbound_budget.total() == 0U;
   };
   const auto mesh_ready = [&] {
      return source.pubsub_snapshot().mesh_edges == 1U && owner.pubsub_snapshot().mesh_edges == 1U;
   };
   fixture.connect(source, owner);
   BOOST_REQUIRE(fixture.wait([&] { return subscribed(sender, owner.local_peer()) && subscribed(self, peer); }));
   forge::asio::blocking::run(fixture.runtime, sender->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait(mesh_ready));
   auto old = std::shared_ptr<node::impl::session_state>{};
   auto source_old = std::shared_ptr<node::impl::session_state>{};
   auto lifetime = std::uint64_t{};
   auto gate = std::shared_ptr<forge::asio::gate>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      old = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      lifetime = self->pubsub_value.peers.at(peer).generation;
   }
   {
      const auto lock = std::scoped_lock{sender->mutex};
      source_old = sender->session_for_path_locked(owner.local_peer(), path::kind::direct, std::nullopt);
      gate = sender->pubsub_value.outbound.at(owner.local_peer()).write_gate;
   }
   BOOST_REQUIRE(old && source_old && gate);
   BOOST_CHECK(old->authentication == peer_authentication::noise);
   BOOST_CHECK(source_old->authentication == peer_authentication::noise);
   auto acquired = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquired.wait_for(5s) != std::future_status::ready) { shutdown_guard::fail_closed(); }
   ticket = acquired.get();
   raw = fixture.open(source, owner);
   raw_id->store(raw.id());
   fixture.send(raw, pubsub::rpc{.control_value = pubsub::control{
       .have = {{.subject = fixture.topic, .message_ids = {id}}}}});
   fixture.send(raw, pubsub::rpc{.subscriptions = {{.subscribe = true, .subject = fixture.topic}}});
   // The next RPC on this SAME stream follows finish_pubsub_request(true), not merely its write trace.
   BOOST_REQUIRE(fixture.wait([&] { return after_request->load(); }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 1U);
      BOOST_CHECK(!self->pubsub_value.validations.contains(key));
   }
   const auto requests = [&] {
      auto count = std::size_t{};
      for (const auto& receipt : fixture.receipts(owner)) {
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != peer) { continue; }
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         if (rpc.control_value) {
            for (const auto& want : rpc.control_value->want) { count += std::ranges::count(want.message_ids, id); }
         }
      }
      return count;
   };
   BOOST_TEST(requests() == 1U);
   ticket.release();
   BOOST_REQUIRE(claimed.wait_for(5s) == std::future_status::ready);
   const auto arrival = claimed.get();
   BOOST_CHECK(arrival.source == peer);
   BOOST_CHECK(arrival.value.from == published.from && arrival.value.seqno == published.seqno);
   BOOST_CHECK(arrival.value.data == published.data && pubsub::codec::verify_message(arrival.value, config));
   auto validation_generation = std::uint64_t{};
   auto score_generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto& row = self->pubsub_value.validations.at(key);
      BOOST_CHECK(row.state == node::impl::pubsub_state::validation::status::in_progress);
      BOOST_CHECK(row.source == peer);
      BOOST_TEST(row.attempts == 1U);
      validation_generation = row.generation;
      score_generation = row.score_generation;
      BOOST_REQUIRE(validation_generation != 0U && score_generation != 0U);
      BOOST_TEST(self->pubsub_value.active_validations == 1U);
      BOOST_TEST(self->pubsub_value.active_validations_by_peer.at(peer) == 1U);
      BOOST_TEST(self->pubsub_value.router->pending() == 0U); // P7 fulfilled before retirement can forget promises.
      BOOST_CHECK(self->pubsub_value.cache.contains(key) && !self->can_serve_pubsub_message_locked(key));
   }
   BOOST_TEST(owner.pubsub_scores().pending_validations == 1U);
   self->request_cancel_session(old);
   sender->request_cancel_session(source_old);
   BOOST_REQUIRE(fixture.wait([&] {
      return source.diagnostics().metrics.active_sessions == 0U && owner.diagnostics().metrics.active_sessions == 0U;
   }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(old->closed && !self->sessions.contains(old->id));
      BOOST_CHECK(!self->pubsub_value.peers.at(peer).connected);
   }
   {
      const auto lock = std::scoped_lock{sender->mutex};
      BOOST_CHECK(source_old->closed && !sender->sessions.contains(source_old->id));
   }
   fixture.connect(source, owner);
   BOOST_REQUIRE(fixture.wait([&] { return subscribed(sender, owner.local_peer()) && subscribed(self, peer); }));
   forge::asio::blocking::run(fixture.runtime, sender->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait(mesh_ready));
   {
      const auto lock = std::scoped_lock{sender->mutex};
      const auto live = sender->session_for_path_locked(owner.local_peer(), path::kind::direct, std::nullopt);
      BOOST_REQUIRE(live && live != source_old && live->id != source_old->id);
      BOOST_CHECK(live->authentication == peer_authentication::noise);
   }
   auto current = std::shared_ptr<node::impl::session_state>{};
   auto current_lifetime = std::uint64_t{};
   auto topic_entries = std::size_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      current = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(current && current != old && current->id != old->id);
      BOOST_CHECK(current->authentication == peer_authentication::noise);
      current_lifetime = self->pubsub_value.peers.at(peer).generation;
      BOOST_TEST(current_lifetime > lifetime);
      topic_entries = self->pubsub_value.remote_topic_entries;
      const auto& row = self->pubsub_value.validations.at(key);
      BOOST_CHECK(row.state == node::impl::pubsub_state::validation::status::in_progress);
      BOOST_TEST(row.generation == validation_generation);
      BOOST_TEST(row.score_generation == score_generation);
   }
   BOOST_TEST(owner.pubsub_scores().pending_validations == 1U); // No TTL expiry can masquerade as neutral settlement.
   const auto before = owner.pubsub_snapshot();
   const auto failures = owner.peers().find(peer)->failures;
   barrier->notify();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto& row = self->pubsub_value.validations.at(key);
      return row.state == node::impl::pubsub_state::validation::status::ignored && row.score_generation == 0U &&
          self->pubsub_value.active_validations == 0U && !self->pubsub_value.active_validations_by_peer.contains(peer) &&
          self->pubsub_value.scoring->snapshot(std::chrono::steady_clock::now()).pending_validations == 0U;
   }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto& row = self->pubsub_value.validations.at(key);
      BOOST_TEST(row.generation == validation_generation);
      BOOST_TEST(row.attempts == 1U);
      BOOST_TEST(row.redeliveries == 0U);
      BOOST_TEST(row.requests == 0U);
      BOOST_CHECK(row.retry_after == std::chrono::steady_clock::time_point{});
      BOOST_CHECK(row.request_after == std::chrono::steady_clock::time_point{});
      BOOST_CHECK(self->pubsub_value.cache.contains(key) && !self->can_serve_pubsub_message_locked(key));
      BOOST_CHECK(self->sessions.at(current->id) == current && !current->closed);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == current_lifetime);
      BOOST_TEST(self->pubsub_value.remote_topic_entries == topic_entries);
      BOOST_CHECK(self->pubsub_value.mesh.at(fixture.topic.value).contains(peer));
      BOOST_CHECK(self->pubsub_value.peer_topics.at(peer).contains(fixture.topic.value));
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
   }
   const auto scores = owner.pubsub_scores();
   BOOST_REQUIRE(scores.peers.size() == 1U && scores.peers.front().topics.size() == 1U);
   const auto& score = scores.peers.front();
   BOOST_CHECK(score.connected && score.topics.front().in_mesh);
   BOOST_TEST(score.value == 0.0);
   BOOST_TEST(score.behaviour_penalty == 0.0);
   BOOST_TEST(score.topics.front().first_message_deliveries == 0.0);
   BOOST_TEST(score.topics.front().mesh_message_deliveries == 0.0);
   BOOST_TEST(score.topics.front().invalid_message_deliveries == 0.0);
   BOOST_TEST(owner.pubsub_snapshot().messages_delivered == before.messages_delivered);
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == before.invalid_messages);
   BOOST_TEST(owner.peers().find(peer)->failures == failures);
   // A real G2 peer repeats IHAVE and asks to serve the ignored row: neither may resurrect old work.
   forge::asio::blocking::run(fixture.runtime, self->pubsub_heartbeat_once());
   raw = fixture.open(source, owner);
   raw_id->store(raw.id());
   after_request->store(false);
   fixture.send(raw, pubsub::rpc{.control_value = pubsub::control{
       .have = {{.subject = fixture.topic, .message_ids = {id}}}, .want = {{.message_ids = {id}}}}});
   fixture.send(raw, pubsub::rpc{.subscriptions = {{.subscribe = true, .subject = fixture.topic}}});
   BOOST_REQUIRE(fixture.wait([&] { return after_request->load(); }));
   BOOST_TEST(requests() == 1U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
      BOOST_CHECK(self->pubsub_value.validations.at(key).state == node::impl::pubsub_state::validation::status::ignored);
   }
   for (const auto& receipt : fixture.receipts(owner)) {
      if (receipt.kind == pubsub::trace_kind::rpc_write) {
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         BOOST_CHECK(std::ranges::none_of(rpc.messages, [&](const auto& value) {
            return pubsub::codec::message_id(value, config) == id;
         }));
      }
      if (receipt.id == id) {
         BOOST_CHECK(receipt.kind != pubsub::trace_kind::validation_committed && receipt.kind != pubsub::trace_kind::delivery);
      }
   }
   const auto fresh = fixture.publish(source, "fresh-native-g2-validation");
   BOOST_REQUIRE(pubsub::codec::verify_message(fresh, config));
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().messages_delivered == before.messages_delivered + 1U; }));
   BOOST_TEST(calls->load() == 2U);
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(owner.pubsub_scores().pending_validations == 0U);
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_reflected_self_origin() {
   auto fixture = forge::tests::p2p::pubsub_router_fixture{};
   auto config = pubsub::options{};
   config.limits.heartbeat_initial_delay = 60s;
   config.limits.heartbeat_interval = 60s;
   config.scoring = pubsub::scoring_params{.decay_interval = 60s};
   config.scoring->topics.emplace(fixture.topic, pubsub::topic_score_params{.first_message_deliveries_weight = 1.0});
   auto& owner = fixture.add("self-origin-owner", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& source = fixture.add("self-origin-source", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto& witness = fixture.add("self-origin-witness", config, {}, "/ip4/127.0.0.1/tcp/0", node::stream_security::noise);
   auto raw = stream{};
   auto witness_shutdown = forge::tests::p2p::gossipsub_test_shutdown{fixture.runtime, owner, witness,
       [&] { raw.request_cancel(); }};
   auto source_shutdown = forge::tests::p2p::gossipsub_test_shutdown{fixture.runtime, owner, source,
       [&] { raw.request_cancel(); }};
   const auto calls = std::make_shared<std::atomic_size_t>(0);
   const auto forwarded = std::make_shared<std::atomic_size_t>(0);
   fixture.subscribe(owner, [calls](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      ++*calls;
      co_return pubsub::validation_result::accept;
   });
   fixture.subscribe(source);
   fixture.subscribe(witness, [forwarded](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      ++*forwarded;
      co_return pubsub::validation_result::accept;
   });
   const auto published = fixture.publish(owner, "locally-signed-before-cache-expiry");
   BOOST_REQUIRE(pubsub::codec::verify_message(published, config));
   BOOST_REQUIRE(published.from && *published.from == owner.local_peer());
   const auto id = pubsub::codec::message_id(published, config);
   const auto key = bytes_key(id);
   const auto self = owner.impl_;
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->pubsub_value.cache.contains(key) && self->can_serve_pubsub_message_locked(key));
   }
   // Exercise the actual insertion-bin shift and prune, with no direct cache or clock mutation.
   for (auto tick = std::size_t{}; tick < config.limits.history_length; ++tick) {
      forge::asio::blocking::run(fixture.runtime, self->pubsub_heartbeat_once());
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(!self->pubsub_value.cache.contains(key));
      BOOST_CHECK(!self->pubsub_value.validations.contains(key));
   }
   BOOST_TEST(calls->load() == 0U);
   fixture.connect(source, owner);
   fixture.connect(witness, owner);
   const auto peer = source.local_peer();
   BOOST_CHECK(peer != owner.local_peer());
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      for (const auto& remote : {peer, witness.local_peer()}) {
         const auto topics = self->pubsub_value.peer_topics.find(remote);
         const auto out = self->pubsub_value.outbound.find(remote);
         if (topics == self->pubsub_value.peer_topics.end() || !topics->second.contains(fixture.topic.value) ||
             out == self->pubsub_value.outbound.end() || out->second.snapshot_pending) { return false; }
      }
      return self->pubsub_value.outbound_budget.total() == 0U;
   }));
   forge::asio::blocking::run(fixture.runtime, self->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      return owner.pubsub_snapshot().mesh_edges == 2U && source.pubsub_snapshot().mesh_edges == 1U &&
          witness.pubsub_snapshot().mesh_edges == 1U;
   }));
   auto session = std::shared_ptr<node::impl::session_state>{};
   auto lifetime = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      session = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(session);
      BOOST_CHECK(session->authentication == peer_authentication::noise);
      lifetime = self->pubsub_value.peers.at(peer).generation;
   }
   const auto before = owner.diagnostics();
   const auto before_pubsub = owner.pubsub_snapshot();
   const auto before_witness = witness.pubsub_snapshot();
   const auto failures = owner.peers().find(peer)->failures;
   raw = fixture.open(source, owner);
   fixture.send(raw, pubsub::rpc{.messages = {published}});
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().invalid_messages == before_pubsub.invalid_messages + 1U; }));
   BOOST_TEST(calls->load() == 0U);
   BOOST_TEST(forwarded->load() == 0U);
   BOOST_TEST(owner.pubsub_snapshot().messages_received == before_pubsub.messages_received + 1U);
   BOOST_TEST(owner.pubsub_snapshot().messages_delivered == before_pubsub.messages_delivered);
   BOOST_TEST(owner.pubsub_snapshot().duplicates == before_pubsub.duplicates);
   BOOST_TEST(witness.pubsub_snapshot().messages_received == before_witness.messages_received);
   const auto scores = owner.pubsub_scores();
   const auto score = std::ranges::find(scores.peers, peer, &pubsub::peer_score_snapshot::peer);
   BOOST_REQUIRE(score != scores.peers.end() && score->topics.size() == 1U);
   BOOST_CHECK(score->connected && score->topics.front().in_mesh);
   BOOST_TEST(score->topics.front().invalid_message_deliveries == 1.0);
   BOOST_TEST(score->topics.front().first_message_deliveries == 0.0);
   BOOST_TEST(score->topics.front().mesh_message_deliveries == 0.0);
   BOOST_TEST(scores.delivery_records == 0U);
   BOOST_TEST(scores.pending_validations == 0U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->sessions.at(session->id) == session && !session->closed);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == lifetime);
      BOOST_CHECK(self->pubsub_value.mesh.at(fixture.topic.value).contains(peer));
      BOOST_CHECK(!self->pubsub_value.cache.contains(key) && !self->pubsub_value.validations.contains(key));
      BOOST_TEST(self->pubsub_value.active_validations == 0U);
      BOOST_CHECK(self->pubsub_value.active_validations_by_peer.empty());
      BOOST_TEST(self->pubsub_value.router->pending() == 0U);
      BOOST_TEST(self->pubsub_value.scores.at(peer).invalid_messages == 1U);
   }
   const auto after = owner.diagnostics();
   BOOST_TEST(after.resources.malformed_scopes == before.resources.malformed_scopes);
   BOOST_TEST(after.resources.denied_malformed == before.resources.denied_malformed);
   BOOST_TEST(after.metrics.protocol_rejections == before.metrics.protocol_rejections);
   BOOST_TEST(after.metrics.sessions_closed == before.metrics.sessions_closed);
   BOOST_TEST(after.metrics.active_sessions == before.metrics.active_sessions);
   BOOST_TEST(owner.peers().find(peer)->failures == failures);
   auto read = false;
   for (const auto& receipt : fixture.receipts(owner)) {
      if (receipt.kind == pubsub::trace_kind::rpc_read && receipt.stream == raw.id()) {
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         if (rpc.messages.empty()) { continue; }
         BOOST_CHECK(receipt.peer == peer && receipt.session == session->id);
         BOOST_TEST(receipt.generation != 0U);
         BOOST_REQUIRE(rpc.messages.size() == 1U);
         BOOST_CHECK(pubsub::codec::verify_message(rpc.messages.front(), config));
         BOOST_CHECK(rpc.messages.front().signature == published.signature);
         BOOST_CHECK(pubsub::codec::message_id(rpc.messages.front(), config) == id);
         read = true;
      }
      if (receipt.kind == pubsub::trace_kind::rpc_write) {
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         BOOST_CHECK(std::ranges::none_of(rpc.messages, [&](const auto& value) {
            return pubsub::codec::message_id(value, config) == id;
         }));
      }
      if (receipt.id == id) {
         BOOST_CHECK(receipt.kind != pubsub::trace_kind::validation_committed && receipt.kind != pubsub::trace_kind::delivery);
      }
   }
   BOOST_CHECK(read);
   const auto fresh = fixture.publish(source, "fresh-foreign-signed-message");
   BOOST_REQUIRE(pubsub::codec::verify_message(fresh, config));
   BOOST_CHECK(fresh.from && *fresh.from == peer && *fresh.from != owner.local_peer());
   BOOST_REQUIRE(fixture.wait([&] {
      return owner.pubsub_snapshot().messages_delivered == before_pubsub.messages_delivered + 1U &&
          witness.pubsub_snapshot().messages_delivered == before_witness.messages_delivered + 1U;
   }));
   BOOST_TEST(calls->load() == 1U);
   BOOST_TEST(forwarded->load() == 1U);
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == before_pubsub.invalid_messages + 1U);
   BOOST_TEST(owner.pubsub_scores().pending_validations == 0U);
   source_shutdown.join();
   witness_shutdown.join();
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(witness.diagnostics().resources.streams.memory == 0U);
}

void node_session_fixture::native_received_policy(bool sign) {
   auto fixture = forge::tests::p2p::pubsub_router_fixture{};
   using shutdown_guard = forge::tests::p2p::gossipsub_test_shutdown;
   auto config = pubsub::options{};
   config.signatures = sign ? pubsub::signature_policy::strict_sign : pubsub::signature_policy::strict_no_sign;
   config.limits.heartbeat_initial_delay = 60s;
   config.limits.heartbeat_interval = 60s;
   config.scoring = pubsub::scoring_params{.decay_interval = 60s};
   // Keep all twelve P4 observations below graylist without disabling their exact raw counters.
   config.scoring->topics.emplace(fixture.topic, pubsub::topic_score_params{
       .first_message_deliveries_weight = 1.0, .invalid_message_deliveries_weight = -0.01});
   const auto marker = pubsub::codec::encode(pubsub::rpc{
       .subscriptions = {{.subscribe = true, .subject = fixture.topic}}}, config);
   const auto raw_id = std::make_shared<std::atomic_int64_t>(-1);
   const auto after_request = std::make_shared<std::atomic_bool>(false);
   config.tracer = [raw_id, after_request, marker](const pubsub::trace_event& event) {
      if (event.kind == pubsub::trace_kind::rpc_read && event.stream_id == raw_id->load() &&
          std::ranges::equal(event.framed_rpc, marker)) {
         after_request->store(true); // A passive next-read marker, never a native-executor barrier.
      }
   };
   const auto security = sign ? node::stream_security::noise : node::stream_security::tls;
   auto& source = fixture.add(sign ? "policy-sign-source" : "policy-nosign-source", config, {},
       "/ip4/127.0.0.1/tcp/0", security);
   auto& owner = fixture.add(sign ? "policy-sign-owner" : "policy-nosign-owner", config, {},
       "/ip4/127.0.0.1/tcp/0", security);
   const auto self = owner.impl_;
   const auto sender = source.impl_;
   const auto peer = source.local_peer();
   auto ticket = forge::asio::gate::ticket{};
   auto raw = stream{};
   auto shutdown = shutdown_guard{fixture.runtime, source, owner, [&] {
      ticket.release();
      raw.request_cancel();
   }};
   const auto calls = std::make_shared<std::atomic_size_t>(0);
   fixture.subscribe(source);
   fixture.subscribe(owner, [calls](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      ++*calls;
      co_return pubsub::validation_result::accept;
   });
   // Real public publication fills SOURCE's cache before it has any native recipient.
   const auto canonical = fixture.publish(source, "pending-native-policy");
   const auto sibling = fixture.publish(source, "valid-policy-sibling");
   const auto continuation = sign ? fixture.publish(source, "valid-policy-continuation") :
       forge::asio::blocking::run(fixture.runtime, source.async_publish(fixture.topic,
           {'c'}, pubsub::publish_options{.sign = false}));
   const auto canonical_id = pubsub::codec::message_id(canonical, config);
   const auto canonical_key = bytes_key(canonical_id);
   const auto append_field = [](std::vector<std::uint8_t>& out, std::uint8_t field,
                                 std::span<const std::uint8_t> bytes) {
      out.push_back(static_cast<std::uint8_t>((field << 3U) | 2U));
      const auto size = forge::multiformats::varint_encode(bytes.size());
      out.insert(out.end(), size.begin(), size.end());
      out.insert(out.end(), bytes.begin(), bytes.end());
   };
   const auto frame = [&](const std::vector<std::vector<std::uint8_t>>& bodies,
                          const pubsub::rpc& prefix = pubsub::rpc{}) {
      auto bytes = unwrap_length_delimited(pubsub::codec::encode(prefix, config), config.limits.max_rpc_size);
      for (const auto& body : bodies) { append_field(bytes, 2, body); }
      return wrap_length_delimited(bytes);
   };
   auto invalid_bodies = std::vector<std::vector<std::uint8_t>>{};
   auto invalid_ids = std::vector<std::vector<std::uint8_t>>{};
   if (sign) {
      BOOST_REQUIRE(canonical.from && *canonical.from == peer && peer != owner.local_peer());
      BOOST_REQUIRE(sender->identity.private_key);
      BOOST_TEST(canonical.seqno.size() == 8U);
      BOOST_CHECK(pubsub::codec::verify_message(canonical, config));
      for (const auto size : {0U, 1U, 7U, 9U}) {
         auto invalid = canonical;
         invalid.seqno.assign(size, 1);
         if (size != 0U) {
            // Crypto-only helpers still accept short/non-normative seqnos; native policy must reject first.
            pubsub::codec::sign_message(invalid, *sender->identity.private_key, config);
            BOOST_CHECK(pubsub::codec::verify_message(invalid, config));
         }
         invalid_ids.push_back(pubsub::codec::message_id(invalid, config));
         invalid_bodies.push_back(pubsub::codec::encode_message(invalid, config));
         const auto old = pubsub::codec::decode(frame({invalid_bodies.back()}), config);
         BOOST_REQUIRE(old.messages.size() == 1U);
         BOOST_TEST(old.messages.front().seqno.size() == size);
      }
   } else {
      for (const auto& value : {canonical, sibling, continuation}) {
         BOOST_CHECK(!value.from && value.seqno.empty() && value.signature.empty() && value.key.empty());
      }
      const auto before = source.pubsub_snapshot();
      auto sequence = std::uint64_t{};
      {
         const auto lock = std::scoped_lock{sender->mutex};
         sequence = sender->pubsub_value.next_seqno;
      }
      BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.runtime, source.async_publish(fixture.topic,
          {'s'}, pubsub::publish_options{.sign = true})), exceptions::invalid_options);
      BOOST_TEST(source.pubsub_snapshot().messages_published == before.messages_published);
      BOOST_TEST(source.pubsub_snapshot().cached_messages == before.cached_messages);
      {
         const auto lock = std::scoped_lock{sender->mutex};
         BOOST_TEST(sender->pubsub_value.next_seqno == sequence);
      }
      for (const auto field : {1U, 3U, 5U, 6U}) {
         for (const auto variant : {0U, 1U, 2U}) {
            auto body = pubsub::codec::encode_message(canonical, config);
            const auto forbidden = variant == 0U ? std::vector<std::uint8_t>{} : std::vector<std::uint8_t>{0xff};
            append_field(body, field, forbidden);
            if (variant == 2U) { append_field(body, field, {}); }
            invalid_bodies.push_back(std::move(body));
            if (field != 1U) {
               // The legacy wire decoder normalizes empty/repeated bytes without applying receive policy.
               const auto old = pubsub::codec::decode(frame({invalid_bodies.back()}), config);
               BOOST_REQUIRE(old.messages.size() == 1U);
            }
         }
      }
      auto legacy = canonical;
      legacy.from = peer;
      legacy.seqno = {1};
      const auto legacy_frame = pubsub::codec::encode(pubsub::rpc{.messages = {legacy}}, config);
      BOOST_CHECK(pubsub::codec::decode(legacy_frame, config).messages.front().from == legacy.from);
      auto lax = config;
      lax.signatures = pubsub::signature_policy::lax_no_sign;
      BOOST_TEST(pubsub::codec::decode_received(legacy_frame, lax).value.messages.size() == 1U);
      BOOST_TEST(pubsub::codec::decode_received(legacy_frame, config).invalid_messages.size() == 1U);
   }
   for (const auto& body : invalid_bodies) {
      const auto decoded = pubsub::codec::decode_received(frame({body}), config);
      BOOST_CHECK(decoded.value.messages.empty());
      BOOST_REQUIRE(decoded.invalid_messages.size() == 1U);
      BOOST_CHECK(decoded.invalid_messages.front() == fixture.topic);
   }
   const auto valid_frame = pubsub::codec::encode(pubsub::rpc{.messages = {canonical}}, config);
   const auto healthy = pubsub::codec::decode_received(valid_frame, config);
   BOOST_REQUIRE(healthy.value.messages.size() == 1U);
   BOOST_CHECK(healthy.invalid_messages.empty());
   BOOST_CHECK(pubsub::codec::message_id(healthy.value.messages.front(), config) == canonical_id);

   auto malformed = invalid_bodies.front();
   malformed.insert(malformed.end(), {0x12, 0x02, 0x01}); // Truncated data AFTER the forbidden/policy-invalid prefix.
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({malformed}), config)), exceptions::codec_error);
   auto wrong_type = invalid_bodies.front();
   wrong_type.insert(wrong_type.end(), {0x18, 0x00}); // A known seqno field must be length-delimited.
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({wrong_type}), config)), exceptions::codec_error);
   auto missing_topic = std::vector<std::uint8_t>{};
   append_field(missing_topic, 5, {});
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({missing_topic}), config)), exceptions::codec_error);
   auto empty_topic = invalid_bodies.front();
   append_field(empty_topic, 4, {});
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({empty_topic}), config)), exceptions::invalid_options);
   auto long_topic = invalid_bodies.front();
   append_field(long_topic, 4, std::vector<std::uint8_t>(config.limits.max_topic_size + 1U, 't'));
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({long_topic}), config)), exceptions::invalid_options);
   auto bounded = config;
   bounded.limits.max_messages = 1;
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({invalid_bodies[0], invalid_bodies[1]}), bounded)), exceptions::codec_error);
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({invalid_bodies[0], pubsub::codec::encode_message(sibling, config)}),
       bounded)), exceptions::codec_error); // Accepted and rejected both consume the same original wire bound.
   bounded = config;
   bounded.limits.max_data_size = 1;
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({invalid_bodies[0]}), bounded)), exceptions::codec_error);
   bounded = config;
   bounded.limits.max_message_size = invalid_bodies[0].size() - 1U;
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(frame({invalid_bodies[0]}), bounded)), exceptions::codec_error);
   bounded = config;
   bounded.limits.max_rpc_size = unwrap_length_delimited(valid_frame, config.limits.max_rpc_size).size() - 1U;
   BOOST_CHECK_THROW(static_cast<void>(pubsub::codec::decode_received(valid_frame, bounded)), exceptions::codec_error);

   const auto subscribed = [&](const auto& impl, const peer_id& remote) {
      const auto lock = std::scoped_lock{impl->mutex};
      const auto topics = impl->pubsub_value.peer_topics.find(remote);
      const auto out = impl->pubsub_value.outbound.find(remote);
      return topics != impl->pubsub_value.peer_topics.end() && topics->second.contains(fixture.topic.value) &&
          out != impl->pubsub_value.outbound.end() && !out->second.snapshot_pending &&
          impl->pubsub_value.outbound_budget.total() == 0U;
   };
   fixture.connect(source, owner);
   BOOST_REQUIRE(fixture.wait([&] { return subscribed(sender, owner.local_peer()) && subscribed(self, peer); }));
   forge::asio::blocking::run(fixture.runtime, sender->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] {
      return source.pubsub_snapshot().mesh_edges == 1U && owner.pubsub_snapshot().mesh_edges == 1U;
   }));
   auto session = std::shared_ptr<node::impl::session_state>{};
   auto gate = std::shared_ptr<forge::asio::gate>{};
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      session = self->session_for_path_locked(peer, path::kind::direct, std::nullopt);
      BOOST_REQUIRE(session);
      BOOST_CHECK(session->authentication == (sign ? peer_authentication::noise : peer_authentication::libp2p_tls));
      generation = self->pubsub_value.peers.at(peer).generation;
   }
   {
      const auto lock = std::scoped_lock{sender->mutex};
      gate = sender->pubsub_value.outbound.at(owner.local_peer()).write_gate;
   }
   auto acquired = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquired.wait_for(5s) != std::future_status::ready) { shutdown_guard::fail_closed(); }
   ticket = acquired.get();
   raw = fixture.open(source, owner);
   raw_id->store(raw.id());
   auto requested_ids = invalid_ids;
   requested_ids.insert(requested_ids.begin(), canonical_id);
   for (const auto& id : requested_ids) {
      // Each single-ID IHAVE deterministically stages that promise, without router state injection.
      fixture.send(raw, pubsub::rpc{.control_value = pubsub::control{
          .have = {{.subject = fixture.topic, .message_ids = {id}}}}});
   }
   forge::asio::blocking::run(fixture.runtime, raw.async_write(marker));
   BOOST_REQUIRE(fixture.wait([&] { return after_request->load(); }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == requested_ids.size());
      BOOST_CHECK(!self->pubsub_value.cache.contains(canonical_key));
   }
   for (const auto& id : requested_ids) {
      auto writes = std::size_t{};
      for (const auto& receipt : fixture.receipts(owner)) {
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != peer) { continue; }
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         if (!rpc.control_value) { continue; }
         for (const auto& want : rpc.control_value->want) { writes += std::ranges::count(want.message_ids, id); }
      }
      BOOST_TEST(writes == 1U);
   }
   const auto extra_topic = pubsub::topic{"forge.policy.sibling"};
   const auto prefix = pubsub::rpc{.subscriptions = {{.subscribe = true, .subject = extra_topic}},
       .control_value = pubsub::control{.grafts = {{.subject = fixture.topic}}}};
   auto mixed_bodies = invalid_bodies;
   mixed_bodies.push_back(pubsub::codec::encode_message(sibling, config));
   const auto mixed_frame = frame(mixed_bodies, prefix);
   const auto mixed = pubsub::codec::decode_received(mixed_frame, config);
   BOOST_TEST(mixed.value.messages.size() == 1U);
   BOOST_TEST(mixed.invalid_messages.size() == invalid_bodies.size());
   BOOST_REQUIRE(mixed.value.subscriptions.size() == 1U && mixed.value.control_value);
   BOOST_CHECK(mixed.value.subscriptions.front().subject == extra_topic);
   BOOST_TEST(mixed.value.control_value->grafts.size() == 1U);
   const auto before = owner.diagnostics();
   const auto before_pubsub = owner.pubsub_snapshot();
   forge::asio::blocking::run(fixture.runtime, raw.async_write(mixed_frame));
   BOOST_REQUIRE(fixture.wait([&] {
      return owner.pubsub_snapshot().invalid_messages == invalid_bodies.size() &&
          owner.pubsub_snapshot().messages_delivered == 1U;
   }));
   BOOST_TEST(calls->load() == 1U);
   BOOST_TEST(owner.pubsub_snapshot().messages_received == invalid_bodies.size() + 1U);
   BOOST_TEST(owner.pubsub_snapshot().control_messages == before_pubsub.control_messages + 1U);
   const auto scores = owner.pubsub_scores();
   BOOST_REQUIRE(scores.peers.size() == 1U && scores.peers.front().topics.size() == 1U);
   const auto& score = scores.peers.front();
   BOOST_CHECK(score.connected && score.topics.front().in_mesh);
   BOOST_TEST(score.topics.front().invalid_message_deliveries == static_cast<double>(invalid_bodies.size()));
   BOOST_TEST(score.topics.front().first_message_deliveries == 1.0);
   BOOST_TEST(score.topics.front().mesh_message_deliveries == 1.0);
   BOOST_TEST(score.behaviour_penalty == 0.0);
   BOOST_TEST(scores.delivery_records == 1U);
   BOOST_TEST(scores.pending_validations == 0U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->sessions.at(session->id) == session && !session->closed);
      BOOST_TEST(self->pubsub_value.peers.at(peer).generation == generation);
      BOOST_CHECK(self->pubsub_value.mesh.at(fixture.topic.value).contains(peer));
      BOOST_CHECK(self->pubsub_value.peer_topics.at(peer).contains(extra_topic.value));
      BOOST_TEST(self->pubsub_value.router->pending() == requested_ids.size());
      BOOST_CHECK(!self->pubsub_value.cache.contains(canonical_key) && !self->pubsub_value.validations.contains(canonical_key));
      for (const auto& id : invalid_ids) {
         BOOST_CHECK(!self->pubsub_value.cache.contains(bytes_key(id)) && !self->pubsub_value.validations.contains(bytes_key(id)));
      }
      BOOST_TEST(self->pubsub_value.active_validations == 0U);
      BOOST_TEST(self->pubsub_value.scores.at(peer).invalid_messages == invalid_bodies.size());
   }
   const auto after = owner.diagnostics();
   BOOST_TEST(after.resources.malformed_scopes == before.resources.malformed_scopes);
   BOOST_TEST(after.resources.denied_malformed == before.resources.denied_malformed);
   BOOST_TEST(after.metrics.protocol_rejections == before.metrics.protocol_rejections);
   BOOST_TEST(after.metrics.sessions_closed == before.metrics.sessions_closed);
   BOOST_TEST(after.metrics.active_sessions == before.metrics.active_sessions);
   ticket.release(); // The canonical, policy-valid native cached reply may now fulfill its real promise.
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().messages_delivered == 2U; }));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == invalid_ids.size());
      BOOST_CHECK(self->can_serve_pubsub_message_locked(canonical_key));
   }
   fixture.send(raw, pubsub::rpc{.messages = {continuation}});
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().messages_delivered == 3U; }));
   BOOST_TEST(calls->load() == 3U);
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == invalid_bodies.size());
   BOOST_TEST(owner.pubsub_snapshot().messages_received == invalid_bodies.size() + 3U);
   // A malformed suffix is still a whole-RPC protocol error, not an early policy rejection.
   forge::asio::blocking::run(fixture.runtime, raw.async_write(frame({malformed})));
   BOOST_REQUIRE(fixture.wait([&] {
      return owner.diagnostics().metrics.protocol_rejections == before.metrics.protocol_rejections + 1U;
   }));
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == invalid_bodies.size() + 1U);
   BOOST_TEST(owner.pubsub_snapshot().messages_received == invalid_bodies.size() + 3U);
   BOOST_TEST(calls->load() == 3U);
   BOOST_TEST(owner.diagnostics().metrics.sessions_closed == before.metrics.sessions_closed);
   BOOST_TEST(owner.diagnostics().resources.denied_malformed == before.resources.denied_malformed);
   BOOST_TEST(owner.pubsub_scores().peers.front().topics.front().invalid_message_deliveries ==
       static_cast<double>(invalid_bodies.size()));
   shutdown.join();
   BOOST_TEST(source.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_CASE(terminal_native_retired_validator_accept_settles_old_generation_without_g2_delivery) {
   forge::net::p2p::node_session_fixture::native_retired_validation(false);
}

BOOST_AUTO_TEST_CASE(terminal_native_retired_validator_retry_settles_old_generation_without_g2_retry) {
   forge::net::p2p::node_session_fixture::native_retired_validation(true);
}

BOOST_AUTO_TEST_CASE(terminal_native_reflected_self_origin_after_cache_expiry_is_p4_not_malformed) {
   forge::net::p2p::node_session_fixture::native_reflected_self_origin();
}

BOOST_AUTO_TEST_CASE(terminal_native_strict_no_sign_preserves_policy_rejections_siblings_and_stream) {
   forge::net::p2p::node_session_fixture::native_received_policy(false);
}

BOOST_AUTO_TEST_CASE(terminal_native_strict_sign_requires_eight_byte_seqno_before_claim) {
   forge::net::p2p::node_session_fixture::native_received_policy(true);
}
