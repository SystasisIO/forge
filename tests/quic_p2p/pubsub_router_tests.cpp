module;

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/scope/scope_exit.hpp>

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.exceptions;
import forge.net.p2p.endpoint;
import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;

#include "pubsub_router_fixture.hxx"
#include "../../libraries/net/p2p/details/connection_singleflight_registry.hxx"
#include "../../libraries/net/p2p/details/pubsub_outbound_budget.hxx"
#include "../../libraries/net/p2p/details/pubsub_router.hxx"
#include "../../libraries/net/p2p/details/resource_stream.hxx"

namespace {

namespace p2p = forge::net::p2p;
namespace ps = p2p::pubsub;
using fixture = forge::tests::p2p::pubsub_router_fixture;
using receipt = forge::tests::p2p::pubsub_router_receipt;
using namespace std::chrono_literals;

ps::options options(const ps::topic& topic) {
   auto result = ps::options{};
   result.limits.mesh_n = 2;
   result.limits.mesh_n_low = 1;
   result.limits.mesh_n_high = 4;
   result.limits.mesh_outbound_min = 0;
   result.limits.mesh_score_min = 1;
   result.limits.heartbeat_initial_delay = 40ms;
   result.limits.heartbeat_interval = 80ms;
   result.limits.prune_backoff = 1s;
   result.limits.iwant_followup_time = 160ms;
   result.scoring.emplace();
   auto params = ps::topic_score_params{};
   params.invalid_message_deliveries_weight = -100;
   result.scoring->topics.emplace(topic, params);
   result.scoring->retain_score = 5s;
   return result;
}

bool in_mesh(const p2p::node& owner, const p2p::peer_id& peer) {
   for (const auto& row : owner.pubsub_scores().peers) {
      if (row.peer == peer) {
         return std::ranges::any_of(row.topics, [](const auto& topic) { return topic.in_mesh; });
      }
   }
   return false;
}

double score(const p2p::node& owner, const p2p::peer_id& peer) {
   for (const auto& row : owner.pubsub_scores().peers) {
      if (row.peer == peer) { return row.value; }
   }
   return 0;
}

std::optional<ps::peer_score_snapshot> peer_score(const p2p::node& owner, const p2p::peer_id& peer) {
   for (const auto& row : owner.pubsub_scores().peers) {
      if (row.peer == peer) { return row; }
   }
   return std::nullopt;
}

bool data_is(const receipt& value, std::string_view data) {
   return std::ranges::equal(value.data, data);
}

bool committed(const fixture& state, const p2p::node& owner, std::string_view data, ps::validation_result result) {
   return std::ranges::any_of(state.receipts(owner), [&](const auto& event) {
      return event.kind == ps::trace_kind::validation_committed && event.result == result && data_is(event, data);
   });
}

bool delivered(const fixture& state, const p2p::node& owner, std::string_view data) {
   return std::ranges::any_of(state.receipts(owner), [&](const auto& event) {
      return event.kind == ps::trace_kind::delivery && data_is(event, data);
   });
}

bool control(const fixture& state, const p2p::node& owner, const p2p::peer_id& peer,
             ps::trace_kind kind, const std::function<bool(const ps::control&)>& match) {
   for (const auto& event : state.receipts(owner)) {
      if (event.kind != kind || event.peer != peer || event.frame.empty()) { continue; }
      const auto rpc = ps::codec::decode(event.frame);
      if (rpc.control_value && match(*rpc.control_value)) { return true; }
   }
   return false;
}

bool subscription(const fixture& state, const p2p::node& owner, const p2p::peer_id& peer) {
   for (const auto& event : state.receipts(owner)) {
      if (event.kind != ps::trace_kind::rpc_read || event.peer != peer || event.frame.empty()) { continue; }
      const auto rpc = ps::codec::decode(event.frame);
      if (std::ranges::any_of(rpc.subscriptions, [&](const auto& entry) {
         return entry.subscribe && entry.subject == state.topic;
      })) { return true; }
   }
   return false;
}

p2p::peer_id peer(std::uint8_t value) {
   return p2p::make_peer_id({.type = p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, value)});
}

} // namespace

BOOST_AUTO_TEST_SUITE(pubsub_router)

BOOST_AUTO_TEST_CASE(native_off_mesh_gossip_ignore_negative_prune_and_replacement_are_causal) {
   auto state = fixture{};
   auto& victim = state.add("pubsub-victim", options(state.topic));
   auto& offender = state.add("pubsub-offender", options(state.topic));
   auto& replacement = state.add("pubsub-replacement", options(state.topic));
   auto& sink = state.add("pubsub-sink", options(state.topic));
   state.subscribe(victim, [](ps::event event) -> boost::asio::awaitable<ps::validation_result> {
      if (event.value.data == std::vector<std::uint8_t>{'I'}) { co_return ps::validation_result::ignore; }
      if (event.value.data == std::vector<std::uint8_t>{'R'}) { co_return ps::validation_result::reject; }
      co_return ps::validation_result::accept;
   });
   state.subscribe(offender);
   state.subscribe(replacement);
   state.subscribe(sink);
   state.connect(offender, victim);
   state.connect(replacement, sink);
   BOOST_REQUIRE(state.wait([&] { return in_mesh(victim, offender.local_peer()) &&
      in_mesh(offender, victim.local_peer()) && in_mesh(replacement, sink.local_peer()) &&
      in_mesh(sink, replacement.local_peer()); }));
   state.connect(victim, replacement);
   static_cast<void>(state.publish(offender, "I"));
   BOOST_REQUIRE(state.wait([&] { return committed(state, victim, "I", ps::validation_result::ignore); }));
   BOOST_TEST(victim.pubsub_snapshot().invalid_messages == 0U);
   static_cast<void>(state.publish(victim, "gossip"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, sink, "gossip"); }));
   BOOST_REQUIRE(control(state, victim, replacement.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.have.empty(); }));
   BOOST_REQUIRE(control(state, replacement, victim.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.want.empty(); }));
   BOOST_TEST(!in_mesh(victim, replacement.local_peer()));
   BOOST_TEST(!in_mesh(replacement, victim.local_peer()));
   BOOST_TEST(!delivered(state, replacement, "I"));
   BOOST_TEST(!delivered(state, sink, "I"));
   static_cast<void>(state.publish(offender, "R"));
   BOOST_REQUIRE(state.wait([&] { return committed(state, victim, "R", ps::validation_result::reject) &&
      !in_mesh(victim, offender.local_peer()) && in_mesh(victim, replacement.local_peer()) &&
      control(state, victim, offender.local_peer(), ps::trace_kind::rpc_write,
         [](const auto& rpc) { return !rpc.prunes.empty(); }) &&
      control(state, victim, replacement.local_peer(), ps::trace_kind::rpc_write,
         [](const auto& rpc) { return !rpc.grafts.empty(); }); }));
   BOOST_TEST(score(victim, offender.local_peer()) < -80.0);
   BOOST_REQUIRE(control(state, victim, offender.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.prunes.empty(); }));
   static_cast<void>(state.publish(victim, "replacement"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, sink, "replacement"); }));
   for (const auto& event : state.receipts(victim)) {
      BOOST_TEST(event.session != 0U);
      BOOST_TEST(event.stream >= 0);
      BOOST_TEST(event.generation != 0U);
      BOOST_CHECK(event.protocol == p2p::builtins::meshsub_v11);
   }
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_v10_and_passive_reentrant_throwing_tracer_do_not_change_policy) {
   auto state = fixture{};
   auto source_options = options(state.topic);
   source_options.preferred = ps::version::v1_0;
   source_options.allow_v1_0_fallback = false;
   auto observed = std::make_shared<p2p::node*>(nullptr);
   auto samples = std::make_shared<std::atomic_size_t>();
   source_options.scoring->app_specific_score = [observed, samples](const auto&) {
      if (!*observed) { throw std::runtime_error{"native owner not published"}; }
      static_cast<void>((*observed)->pubsub_scores()); // Would deadlock if sampled under node/engine locks.
      samples->fetch_add(1);
      return 0.0;
   };
   source_options.tracer = [observed](const auto&) {
      if (!*observed) { throw std::runtime_error{"native owner not published"}; }
      static_cast<void>((*observed)->pubsub_snapshot());
      static_cast<void>((*observed)->pubsub_scores());
      throw std::runtime_error{"passive observer failure"};
   };
   auto target_options = options(state.topic);
   target_options.preferred = ps::version::v1_0;
   target_options.allow_v1_0_fallback = false;
   auto& source = state.add("pubsub-v10-source", std::move(source_options));
   *observed = &source;
   auto& target = state.add("pubsub-v10-target", std::move(target_options));
   state.subscribe(source);
   state.subscribe(target);
   state.connect(source, target);
   BOOST_REQUIRE(state.wait([&] { return in_mesh(source, target.local_peer()); }));
   static_cast<void>(state.publish(source, "v10"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, target, "v10"); }));
   forge::asio::blocking::run(state.runtime, source.async_unsubscribe(state.topic));
   BOOST_REQUIRE(state.wait([&] { return control(state, source, target.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.prunes.empty(); }); }));
   for (const auto& event : state.receipts(source)) {
      BOOST_CHECK(event.protocol == p2p::builtins::meshsub_v10);
      if (!event.frame.empty()) {
         const auto rpc = ps::codec::decode(event.frame);
         if (rpc.control_value) {
            for (const auto& prune : rpc.control_value->prunes) {
               BOOST_TEST(prune.peers.empty());
               BOOST_TEST(prune.backoff.count() == 0);
            }
         }
      }
   }
   BOOST_TEST(source.pubsub_snapshot().trace_failures > 0U);
   BOOST_TEST(samples->load() > 0U);
   BOOST_TEST(source.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(score(target, source.local_peer()) == 0.0);
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_throwing_application_score_keeps_last_sample_and_heartbeat_recovers) {
   auto state = fixture{};
   auto config = options(state.topic);
   auto mode = std::make_shared<std::atomic_uint>();
   config.scoring->app_specific_weight = 1;
   config.scoring->app_specific_score = [mode](const auto&) {
      switch (mode->load()) {
      case 1: throw std::runtime_error{"application sampling failure"};
      case 2: return std::numeric_limits<double>::quiet_NaN();
      case 3: return 7.0;
      default: return 4.0;
      }
   };
   auto& owner = state.add("pubsub-sampling-owner", std::move(config));
   auto& remote = state.add("pubsub-sampling-peer", options(state.topic));
   state.subscribe(owner);
   state.subscribe(remote);
   state.connect(owner, remote);
   BOOST_REQUIRE(state.wait([&] {
      const auto row = peer_score(owner, remote.local_peer());
      return row && row->app_specific_sample == 4.0 && in_mesh(owner, remote.local_peer());
   }));
   mode->store(1);
   BOOST_REQUIRE(state.wait([&] { return owner.pubsub_snapshot().application_score_failures >= 2U; }));
   auto row = peer_score(owner, remote.local_peer());
   BOOST_REQUIRE(row);
   BOOST_TEST(row->app_specific_sample == 4.0);
   BOOST_TEST(in_mesh(owner, remote.local_peer()));
   const auto failed = owner.pubsub_snapshot().application_score_failures;
   mode->store(2);
   BOOST_REQUIRE(state.wait([&] { return owner.pubsub_snapshot().application_score_failures > failed; }));
   row = peer_score(owner, remote.local_peer());
   BOOST_REQUIRE(row);
   BOOST_TEST(row->app_specific_sample == 4.0);
   mode->store(3);
   // No RPC/publication triggers sampling here: only the still-live maintenance worker can recover it.
   BOOST_REQUIRE(state.wait([&] {
      const auto observed = peer_score(owner, remote.local_peer());
      return observed && observed->app_specific_sample == 7.0;
   }));
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(owner.pubsub_snapshot().trace_failures == 0U);
   static_cast<void>(state.publish(owner, "sampling-recovered"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, remote, "sampling-recovered"); }));
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_initial_subscription_snapshot_pressure_preserves_mesh_and_sticky_score) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.heartbeat_initial_delay = 30s;
   config.limits.max_outbound_queue_bytes = 768;
   auto& params = config.scoring->topics.at(state.topic);
   params.mesh_failure_penalty_weight = -1;
   params.mesh_message_deliveries_activation = 1s;
   params.mesh_message_deliveries_threshold = 1;
   auto& owner = state.add("pubsub-snapshot-owner", config);
   auto& remote = state.add("pubsub-snapshot-peer", options(state.topic));
   state.subscribe(owner);
   auto extra = std::vector<ps::topic>{};
   for (auto index = 0; index < 40; ++index) {
      extra.push_back(ps::topic{"snapshot-" + std::to_string(index) + std::string(80, 's')});
      static_cast<void>(forge::asio::blocking::run(state.runtime, owner.async_subscribe(extra.back(),
         [](auto) -> boost::asio::awaitable<ps::validation_result> { co_return ps::validation_result::accept; })));
   }
   state.connect(remote, owner);
   auto input = state.open(remote, owner);
   // Genuine remote subscription/GRAFT establishes the mesh without requiring an outbound snapshot to fit.
   state.send(input, ps::rpc{.subscriptions = {{.subscribe = true, .subject = state.topic}},
      .control_value = ps::control{.grafts = {{.subject = state.topic}}}});
   BOOST_REQUIRE(state.wait([&] {
      const auto row = peer_score(owner, remote.local_peer());
      return row && !row->topics.empty() && row->topics.front().in_mesh && row->topics.front().mesh_deliveries_active;
   }));
   const auto baseline = owner.metrics().backpressure_rejections;
   for (auto attempt = 0; attempt < 3; ++attempt) {
      auto rejected = false;
      try {
         static_cast<void>(state.publish(owner, "snapshot-pressure"));
      } catch (const forge::exceptions::base& error) {
         rejected = p2p::exceptions::is(error, p2p::exceptions::code::backpressure_rejected);
         BOOST_CHECK(rejected);
      }
      BOOST_REQUIRE(rejected);
      BOOST_TEST(in_mesh(owner, remote.local_peer()));
      const auto row = peer_score(owner, remote.local_peer());
      BOOST_REQUIRE(row);
      BOOST_TEST(row->topics.front().mesh_failure_penalty == 0.0);
   }
   BOOST_TEST(owner.metrics().backpressure_rejections >= baseline + 3U);
   for (const auto& subject : extra) {
      forge::asio::blocking::run(state.runtime, owner.async_unsubscribe(subject));
   }
   state.subscribe(remote);
   static_cast<void>(state.publish(owner, "snapshot-recovered"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, remote, "snapshot-recovered"); }));
   BOOST_TEST(in_mesh(owner, remote.local_peer()));
   forge::asio::blocking::run(state.runtime, input.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_pending_validation_attributes_duplicate_reject_and_not_pressure) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.heartbeat_initial_delay = 30s; // Native RPC/validation test, not a fabricated mesh.
   config.limits.max_validation_queue = 1;
   config.scoring->decay_interval = 30s;
   auto& receiver = state.add("pubsub-validation-owner", config);
   auto& first = state.add("pubsub-validation-first", config);
   auto& duplicate = state.add("pubsub-validation-duplicate", config);
   auto release = std::make_shared<forge::asio::notification>();
   auto release_on_failure = boost::scope::scope_exit{[release] { release->notify(); }};
   auto entered = std::make_shared<std::atomic_size_t>();
   state.subscribe(receiver, [release, entered](auto) -> boost::asio::awaitable<ps::validation_result> {
      entered->fetch_add(1);
      static_cast<void>(co_await release->async_wait(0));
      co_return ps::validation_result::reject;
   });
   // Sign through the real public API before there is any remote subscription/dial route.
   const auto message = state.publish(first, "pending");
   const auto pressure = state.publish(first, "pressure");
   state.connect(first, receiver);
   state.connect(duplicate, receiver);
   auto a = state.open(first, receiver);
   auto first_duplicate = state.open(first, receiver);
   auto b = state.open(duplicate, receiver);
   // An actual signed local message with no remote topic route; raw streams deliver identical signed bytes.
   state.send(a, ps::rpc{.messages = {message}});
   BOOST_REQUIRE(state.wait([&] { return entered->load() == 1U; }));
   state.send(first_duplicate, ps::rpc{.messages = {message}});
   state.send(b, ps::rpc{.messages = {message, pressure}});
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().duplicates == 2U &&
      receiver.metrics().backpressure_rejections > 0U; }));
   BOOST_TEST(entered->load() == 1U);
   BOOST_TEST(score(receiver, first.local_peer()) == 0.0);
   BOOST_TEST(score(receiver, duplicate.local_peer()) == 0.0);
   release->notify();
   BOOST_REQUIRE(state.wait([&] { return committed(state, receiver, "pending", ps::validation_result::reject); }));
   const auto first_rejected = peer_score(receiver, first.local_peer());
   const auto duplicate_rejected = peer_score(receiver, duplicate.local_peer());
   BOOST_REQUIRE(first_rejected);
   BOOST_REQUIRE(duplicate_rejected);
   BOOST_REQUIRE_EQUAL(first_rejected->topics.size(), 1U);
   BOOST_REQUIRE_EQUAL(duplicate_rejected->topics.size(), 1U);
   BOOST_TEST(first_rejected->topics.front().invalid_message_deliveries == 2.0);
   BOOST_TEST(duplicate_rejected->topics.front().invalid_message_deliveries == 1.0);
   BOOST_TEST(first_rejected->value == -400.0);
   BOOST_TEST(duplicate_rejected->value == -100.0);
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 1U);
   BOOST_TEST(!delivered(state, receiver, "pressure"));
   forge::asio::blocking::run(state.runtime, a.async_close());
   forge::asio::blocking::run(state.runtime, first_duplicate.async_close());
   forge::asio::blocking::run(state.runtime, b.async_close());
   forge::asio::blocking::run(state.runtime, first.async_stop());
   forge::asio::blocking::run(state.runtime, duplicate.async_stop());
   BOOST_REQUIRE(state.wait([&] {
      const auto snapshot = receiver.pubsub_scores();
      return std::ranges::count_if(snapshot.peers, [&](const auto& row) {
         return (row.peer == first.local_peer() || row.peer == duplicate.local_peer()) && !row.connected &&
                row.retain_until.has_value() && row.value <= -99.0;
      }) == 2;
   }));
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_cached_stream_memory_pressure_keeps_generation_mesh_and_scores_then_recovers) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.heartbeat_initial_delay = 30s;
   auto& params = config.scoring->topics.at(state.topic);
   params.mesh_failure_penalty_weight = -1;
   params.mesh_message_deliveries_activation = 1s;
   params.mesh_message_deliveries_threshold = 1;
   auto resources = p2p::resource_manager::limits{};
   resources.stream.max_memory = 4'096;
   auto& owner = state.add("pubsub-memory-owner", std::move(config), resources);
   auto& remote = state.add("pubsub-memory-peer", options(state.topic));
   state.subscribe(owner);
   state.subscribe(remote);
   state.connect(owner, remote);
   BOOST_REQUIRE(state.wait([&] {
      const auto row = peer_score(owner, remote.local_peer());
      return row && !row->topics.empty() && row->topics.front().in_mesh && row->topics.front().mesh_deliveries_active &&
         in_mesh(remote, owner.local_peer());
   }));
   const auto message_receipt = [&](std::string_view data) -> std::optional<receipt> {
      for (const auto& event : state.receipts(owner)) {
         if (event.kind != ps::trace_kind::rpc_write || event.peer != remote.local_peer() || event.frame.empty()) { continue; }
         const auto rpc = ps::codec::decode(event.frame);
         if (std::ranges::any_of(rpc.messages, [&](const auto& message) { return std::ranges::equal(message.data, data); })) {
            return event;
         }
      }
      return std::nullopt;
   };
   static_cast<void>(state.publish(owner, "memory-small-before"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, remote, "memory-small-before") &&
      owner.diagnostics().resources.streams.memory == 0U; }));
   const auto before = message_receipt("memory-small-before");
   BOOST_REQUIRE(before);
   BOOST_TEST(before->session != 0U);
   BOOST_TEST(before->stream >= 0);
   BOOST_TEST(before->generation != 0U);
   BOOST_TEST(before->frame.size() < resources.stream.max_memory);
   const auto memory = owner.diagnostics().resources;
   const auto scored = peer_score(owner, remote.local_peer());
   BOOST_REQUIRE(scored);
   BOOST_REQUIRE_EQUAL(scored->topics.size(), 1U);
   const auto oversized = std::string(8'192, 'p'); // Legal RPC/data, but above this real stream's memory limit.
   auto rejected = false;
   try { static_cast<void>(state.publish(owner, oversized)); }
   catch (const forge::exceptions::base& error) {
      rejected = p2p::exceptions::is(error, p2p::exceptions::code::backpressure_rejected);
      BOOST_CHECK(rejected);
   }
   BOOST_REQUIRE(rejected);
   BOOST_TEST(owner.diagnostics().resources.denied_memory > memory.denied_memory);
   BOOST_TEST(owner.diagnostics().resources.streams.memory == 0U);
   BOOST_TEST(owner.diagnostics().resources.streams.outbound_streams == memory.streams.outbound_streams);
   const auto pressured = peer_score(owner, remote.local_peer());
   BOOST_REQUIRE(pressured);
   BOOST_REQUIRE_EQUAL(pressured->topics.size(), 1U);
   BOOST_TEST(pressured->topics.front().in_mesh);
   BOOST_TEST(pressured->topics.front().mesh_failure_penalty == scored->topics.front().mesh_failure_penalty);
   BOOST_TEST(pressured->topics.front().invalid_message_deliveries == scored->topics.front().invalid_message_deliveries);
   BOOST_TEST(pressured->value == scored->value);
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(!message_receipt(oversized).has_value());
   BOOST_TEST(!delivered(state, remote, oversized));
   BOOST_TEST(!control(state, owner, remote.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.prunes.empty(); }));
   static_cast<void>(state.publish(owner, "memory-small-after"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, remote, "memory-small-after") &&
      owner.diagnostics().resources.streams.memory == 0U; }));
   const auto after = message_receipt("memory-small-after");
   BOOST_REQUIRE(after);
   BOOST_TEST(after->session == before->session);
   BOOST_TEST(after->stream == before->stream);
   BOOST_TEST(after->generation == before->generation);
   BOOST_TEST(in_mesh(owner, remote.local_peer()));
   state.stop();
}

BOOST_AUTO_TEST_CASE(memory_rejection_provenance_never_accepts_unmarked_downstream_errors) {
   const auto downstream_pressure = p2p::exceptions::backpressure_rejected{"downstream pressure after write start"};
   const auto downstream_io = std::runtime_error{"downstream native write failure"};
   const auto allocation = std::bad_alloc{};
   BOOST_TEST(!p2p::detail::resource_stream::is_memory_rejection(downstream_pressure));
   BOOST_TEST(!p2p::detail::resource_stream::is_memory_rejection(downstream_io));
   BOOST_TEST(!p2p::detail::resource_stream::is_memory_rejection(allocation));
   BOOST_TEST(!p2p::detail::resource_stream::preparation_failure(downstream_pressure));
   BOOST_TEST(!p2p::detail::resource_stream::preparation_failure(downstream_io));
   BOOST_TEST(!p2p::detail::resource_stream::preparation_failure(allocation));
}

BOOST_AUTO_TEST_CASE(native_public_open_prepare_failure_keeps_session_cached_pubsub_mesh_and_next_rpc) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.heartbeat_initial_delay = 1h; // No periodic stream admission inside this bounded test.
   config.scoring->decay_interval = 1s; // Score snapshots refresh P3 activation on decay steps.
   auto& params = config.scoring->topics.at(state.topic);
   params.mesh_failure_penalty_weight = -1;
   params.mesh_message_deliveries_activation = 1s;
   params.mesh_message_deliveries_threshold = 10;
   auto remote_config = config;
   auto mutex = std::make_shared<std::mutex>();
   auto changed = std::make_shared<std::condition_variable>();
   auto released = std::make_shared<bool>(false);
   auto entered = std::make_shared<std::atomic_bool>();
   config.tracer = [mutex, changed, released, entered](const ps::trace_event& event) {
      if (event.kind != ps::trace_kind::rpc_write || event.framed_rpc.empty()) { return; }
      const auto rpc = ps::codec::decode(event.framed_rpc);
      if (std::ranges::none_of(rpc.messages, [](const auto& message) {
         return std::ranges::equal(message.data, std::string_view{"admission-barrier"});
      })) { return; }
      auto lock = std::unique_lock{*mutex};
      entered->store(true);
      changed->notify_all();
      changed->wait(lock, [&] { return *released; }); // Actual write finished, node mutex/write ticket already released.
   };
   auto& owner = state.add("pubsub-open-prepare-owner", std::move(config));
   auto& remote = state.add("pubsub-open-prepare-peer", std::move(remote_config));
   state.subscribe(owner);
   state.subscribe(remote);
   state.connect(owner, remote);
   for (const auto& [source, target] : std::array{
           std::pair{&owner, &remote}, std::pair{&remote, &owner}}) {
      // PX joins automatic Identify before checking the exact remote protocol. These PubSub-only
      // peers do not advertise PX, so its public call stops without opening another native stream.
      auto unavailable = false;
      try { forge::asio::blocking::run(state.runtime, source->async_request_peer_exchange(target->local_peer())); }
      catch (const forge::exceptions::base& error) {
         unavailable = p2p::exceptions::is(error, p2p::exceptions::code::unsupported_protocol);
      }
      BOOST_REQUIRE(unavailable);
      const auto snapshot = source->diagnostics();
      BOOST_REQUIRE_EQUAL(snapshot.sessions.size(), 1U);
      BOOST_TEST(!snapshot.sessions.front().closed);
      BOOST_CHECK(snapshot.sessions.front().identify_state == p2p::identify::state::identified);
      BOOST_CHECK(snapshot.sessions.front().authentication != p2p::peer_authentication::unverified);
   }
   auto to_owner = state.open(remote, owner);
   auto to_remote = state.open(owner, remote);
   const auto graft = ps::rpc{.subscriptions = {{.subscribe = true, .subject = state.topic}},
      .control_value = ps::control{.grafts = {{.subject = state.topic}}}};
   state.send(to_owner, graft);
   state.send(to_remote, graft);
   BOOST_REQUIRE(state.wait([&] {
      return in_mesh(owner, remote.local_peer()) && in_mesh(remote, owner.local_peer()) &&
         subscription(state, owner, remote.local_peer()) && subscription(state, remote, owner.local_peer());
   }));
   BOOST_REQUIRE(state.wait([&] {
      const auto row = peer_score(owner, remote.local_peer());
      return row && !row->topics.empty() && row->topics.front().mesh_deliveries_active &&
         in_mesh(owner, remote.local_peer()) && in_mesh(remote, owner.local_peer()) &&
         subscription(state, owner, remote.local_peer()) && subscription(state, remote, owner.local_peer());
   }));

   auto publication = std::future<ps::message>{};
   auto release_and_join = boost::scope::scope_exit{[&] {
      { const auto lock = std::scoped_lock{*mutex}; *released = true; }
      changed->notify_all();
      if (publication.valid()) { try { static_cast<void>(publication.get()); } catch (...) {} }
   }};
   publication = boost::asio::co_spawn(state.runtime.context(),
      owner.async_publish(state.topic, {'a', 'd', 'm', 'i', 's', 's', 'i', 'o', 'n', '-', 'b', 'a', 'r', 'r', 'i', 'e', 'r'}),
      boost::asio::use_future);
   BOOST_REQUIRE(state.wait([&] {
      if (!entered->load() || !delivered(state, remote, "admission-barrier")) { return false; }
      for (const auto* peer : {&owner, &remote}) {
         const auto resources = peer->diagnostics().resources;
         // Only cached PubSub and explicit GRAFT streams remain; Identify/Push resource scopes
         // and all pending write memory must have drained on both owners before arming the hook.
         if (resources.active_protocol_scopes != 1U || resources.transient.inbound_streams != 0U ||
             resources.transient.outbound_streams != 0U || resources.streams.inbound_streams != 2U ||
             resources.streams.outbound_streams != 2U || resources.streams.memory != 0U) { return false; }
      }
      return true;
   }));
   const auto message_receipt = [&](std::string_view data) -> std::optional<receipt> {
      for (const auto& event : state.receipts(owner)) {
         if (event.kind != ps::trace_kind::rpc_write || event.peer != remote.local_peer() || event.frame.empty()) { continue; }
         const auto rpc = ps::codec::decode(event.frame);
         if (std::ranges::any_of(rpc.messages, [&](const auto& message) { return std::ranges::equal(message.data, data); })) {
            return event;
         }
      }
      return std::nullopt;
   };
   const auto before = message_receipt("admission-barrier");
   BOOST_REQUIRE(before);
   const auto native = owner.diagnostics();
   const auto neighbor = remote.diagnostics();
   const auto scored = peer_score(owner, remote.local_peer());
   BOOST_REQUIRE(scored);
   BOOST_REQUIRE_EQUAL(scored->topics.size(), 1U);
   BOOST_REQUIRE_EQUAL(native.sessions.size(), 1U);
   BOOST_TEST(before->session == native.sessions.front().id);

   // Consume an unclaimed process-wide injection on exceptional exit before releasing the
   // native barrier, so teardown/other tests cannot inherit the existing private failpoint.
   auto failpoint_cleanup = p2p::resource_manager{};
   auto cleanup_peer = remote.local_peer();
   auto clear_injection = boost::scope::scope_exit{[&failpoint_cleanup, cleanup_peer = std::move(cleanup_peer)]() mutable noexcept {
      static_cast<void>(failpoint_cleanup.reserve_stream(std::move(cleanup_peer),
         p2p::resource_manager::session_direction::outbound));
   }};
   forge_test_pubsub_fail_next_stream_reserve_prepare();
   auto rejected = false;
   try {
      auto unexpected = state.open(owner, remote); // Public API, no fixture-supplied private admission phase.
      forge::asio::blocking::run(state.runtime, unexpected.async_close());
   } catch (const forge::exceptions::base& error) {
      rejected = p2p::exceptions::is(error, p2p::exceptions::code::internal);
   }
   BOOST_REQUIRE(rejected);
   const auto failed = owner.diagnostics();
   BOOST_TEST(failed.resources.runtime_failures == native.resources.runtime_failures + 1U);
   BOOST_TEST(remote.diagnostics().resources.runtime_failures == neighbor.resources.runtime_failures);
   BOOST_TEST(failed.resources.denied_streams == native.resources.denied_streams);
   BOOST_TEST(failed.resources.streams.inbound_streams == native.resources.streams.inbound_streams);
   BOOST_TEST(failed.resources.streams.outbound_streams == native.resources.streams.outbound_streams);
   BOOST_REQUIRE_EQUAL(failed.sessions.size(), 1U);
   BOOST_TEST(failed.sessions.front().id == native.sessions.front().id);
   BOOST_TEST(!failed.sessions.front().closed);
   const auto after_failure = peer_score(owner, remote.local_peer());
   BOOST_REQUIRE(after_failure);
   BOOST_REQUIRE_EQUAL(after_failure->topics.size(), 1U);
   BOOST_TEST(after_failure->topics.front().in_mesh);
   BOOST_TEST(after_failure->topics.front().mesh_failure_penalty == scored->topics.front().mesh_failure_penalty);
   BOOST_TEST(after_failure->topics.front().invalid_message_deliveries == scored->topics.front().invalid_message_deliveries);
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(!control(state, owner, remote.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.prunes.empty(); }));

   { const auto lock = std::scoped_lock{*mutex}; *released = true; }
   changed->notify_all();
   static_cast<void>(publication.get());
   static_cast<void>(state.publish(owner, "admission-recovered"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, remote, "admission-recovered"); }));
   const auto after = message_receipt("admission-recovered");
   BOOST_REQUIRE(after);
   BOOST_TEST(after->session == before->session);
   BOOST_TEST(after->stream == before->stream);
   BOOST_TEST(after->generation == before->generation);
   BOOST_TEST(in_mesh(owner, remote.local_peer()));
   BOOST_TEST(in_mesh(remote, owner.local_peer()));
   forge::asio::blocking::run(state.runtime, to_owner.async_close());
   forge::asio::blocking::run(state.runtime, to_remote.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_retry_is_neutral_and_cached_redelivery_commits_once) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.history_length = 100;
   config.limits.validation_retry_initial_delay = 20ms;
   config.limits.validation_retry_max_delay = 80ms;
   config.scoring->topics.at(state.topic).first_message_deliveries_weight = 3;
   auto& source = state.add("pubsub-retry-source", config);
   auto& receiver = state.add("pubsub-retry-owner", config);
   const auto message = state.publish(source, "retry");
   const auto id = ps::codec::message_id(message);
   auto calls = std::make_shared<std::atomic_size_t>();
   state.subscribe(receiver, [calls](auto) -> boost::asio::awaitable<ps::validation_result> {
      co_return calls->fetch_add(1) == 0 ? ps::validation_result::retry : ps::validation_result::accept;
   });
   state.connect(source, receiver);
   auto stream = state.open(source, receiver);
   state.send(stream, ps::rpc{.messages = {message}});
   BOOST_REQUIRE(state.wait([&] { return delivered(state, receiver, "retry"); }));
   BOOST_TEST(calls->load() == 2U);
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(score(receiver, source.local_peer()) > 2.5);
   BOOST_REQUIRE(control(state, receiver, source.local_peer(), ps::trace_kind::rpc_write,
      [&](const auto& rpc) { return !rpc.want.empty() && rpc.want.front().message_ids.size() == 1 &&
         rpc.want.front().message_ids.front() == id; }));
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 1U);
   forge::asio::blocking::run(state.runtime, stream.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_broken_iwant_promise_is_p7_not_invalid_message) {
   auto state = fixture{};
   auto receiver_options = options(state.topic);
   receiver_options.scoring->behaviour_penalty_weight = -3;
   auto& receiver = state.add("pubsub-promise-owner", std::move(receiver_options));
   auto& source = state.add("pubsub-promise-source", options(state.topic));
   state.subscribe(receiver);
   state.connect(source, receiver);
   auto stream = state.open(source, receiver);
   const auto missing = std::vector<std::uint8_t>{0, 7, 0, 9};
   state.send(stream, ps::rpc{.control_value = ps::control{
      .have = {ps::control::ihave{.subject = state.topic, .message_ids = {missing}}}}});
   BOOST_REQUIRE(state.wait([&] { return control(state, receiver, source.local_peer(), ps::trace_kind::rpc_write,
      [&](const auto& rpc) { return !rpc.want.empty() && rpc.want.front().message_ids.size() == 1 &&
         rpc.want.front().message_ids.front() == missing; }); }));
   BOOST_REQUIRE(state.wait([&] { return score(receiver, source.local_peer()) < -2.0; }));
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   forge::asio::blocking::run(state.runtime, stream.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_retry_claim_after_validation_queue_pressure_keeps_retry_and_neutral_scores) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.max_validation_queue = 1;
   config.limits.history_length = 100;
   config.limits.validation_retry_initial_delay = 500ms;
   config.limits.validation_retry_max_delay = 500ms;
   config.limits.max_validation_redeliveries = 8;
   config.scoring->behaviour_penalty_weight = -3;
   config.scoring->topics.at(state.topic).first_message_deliveries_weight = 3;
   auto& source = state.add("pubsub-retry-pressure-source", config);
   auto& receiver = state.add("pubsub-retry-pressure-owner", config);
   const auto retried = state.publish(source, "retry-after-pressure");
   const auto held = state.publish(source, "held-validation");
   const auto id = ps::codec::message_id(retried);
   auto release = std::make_shared<forge::asio::notification>();
   auto release_on_failure = boost::scope::scope_exit{[release] { release->notify(); }};
   auto held_entered = std::make_shared<std::atomic_bool>();
   auto retry_calls = std::make_shared<std::atomic_size_t>();
   state.subscribe(receiver, [release, held_entered, retry_calls](ps::event event)
       -> boost::asio::awaitable<ps::validation_result> {
      if (std::ranges::equal(event.value.data, std::string_view{"held-validation"})) {
         held_entered->store(true);
         static_cast<void>(co_await release->async_wait(0));
         co_return ps::validation_result::ignore;
      }
      co_return retry_calls->fetch_add(1) == 0 ? ps::validation_result::retry : ps::validation_result::accept;
   });
   state.connect(source, receiver);
   auto retry_stream = state.open(source, receiver);
   // One actual RPC processes retry/defer before entering the second, held validation.
   state.send(retry_stream, ps::rpc{.messages = {retried, held}});
   BOOST_REQUIRE(state.wait([&] { return held_entered->load(); }));
   BOOST_TEST(retry_calls->load() == 1U);
   const auto baseline = receiver.metrics().backpressure_rejections;
   // The real heartbeat requests a due cached retry. Its native response reaches claim while the queue is full.
   BOOST_REQUIRE(state.wait([&] { return receiver.metrics().backpressure_rejections > baseline; }));
   BOOST_REQUIRE(control(state, receiver, source.local_peer(), ps::trace_kind::rpc_write,
      [&](const auto& rpc) { return !rpc.want.empty() && rpc.want.front().message_ids.size() == 1U &&
         rpc.want.front().message_ids.front() == id; }));
   BOOST_TEST(retry_calls->load() == 1U);
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   const auto pressured = peer_score(receiver, source.local_peer());
   BOOST_REQUIRE(pressured);
   BOOST_TEST(pressured->behaviour_penalty == 0.0);
   BOOST_TEST(pressured->topics.front().invalid_message_deliveries == 0.0);
   release->notify();
   BOOST_REQUIRE(state.wait([&] { return delivered(state, receiver, "retry-after-pressure"); }));
   BOOST_TEST(retry_calls->load() == 2U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 1U);
   const auto completed = peer_score(receiver, source.local_peer());
   BOOST_REQUIRE(completed);
   BOOST_TEST(completed->behaviour_penalty == 0.0);
   BOOST_TEST(completed->topics.front().first_message_deliveries > 0.9);
   BOOST_TEST(completed->topics.front().first_message_deliveries <= 1.0);
   BOOST_TEST(completed->topics.front().invalid_message_deliveries == 0.0);
   forge::asio::blocking::run(state.runtime, retry_stream.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(indexed_promises_preserve_original_deadline_and_capacity_and_binary_ids) {
   const auto start = std::chrono::steady_clock::time_point{};
   auto router = p2p::detail::pubsub_router{2, 7};
   const auto id = std::string{"a\0b", 3};
   BOOST_REQUIRE(router.promise(peer(1), id, start + 1s));
   BOOST_REQUIRE(router.promise(peer(2), id, start + 2s));
   BOOST_REQUIRE(router.promise(peer(1), id, start + 20s));
   BOOST_TEST(!router.promise(peer(3), "other", start + 3s));
   const auto expired = router.expire(start + 1s);
   BOOST_REQUIRE_EQUAL(expired.size(), 1U);
   BOOST_TEST(expired.at(peer(1)) == 1U);
   BOOST_TEST(router.pending() == 1U);
   router.fulfill(id);
   BOOST_TEST(router.pending() == 0U);
   BOOST_REQUIRE(router.promise(peer(3), "other", start + 3s));
   router.forget(peer(3));
   BOOST_TEST(router.expire(start + 30s).empty());
}

BOOST_AUTO_TEST_CASE(native_slow_validation_past_score_ttl_settles_neutrally_and_releases_cache_admission) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.history_length = 1;
   config.limits.history_gossip = 1;
   config.limits.max_messages = 1;
   config.limits.max_validation_queue = 1;
   config.scoring->seen_message_ttl = 50ms;
   auto& receiver = state.add("pubsub-slow-ttl-owner", std::move(config));
   auto& source = state.add("pubsub-slow-ttl-source", options(state.topic));
   const auto slow = state.publish(source, "slow-ttl");
   const auto next = state.publish(source, "after-ttl");
   auto release = std::make_shared<forge::asio::notification>();
   auto release_on_failure = boost::scope::scope_exit{[release] { release->notify(); }};
   auto calls = std::make_shared<std::atomic_size_t>();
   state.subscribe(receiver, [release, calls](auto) -> boost::asio::awaitable<ps::validation_result> {
      if (calls->fetch_add(1) == 0) { static_cast<void>(co_await release->async_wait(0)); }
      co_return ps::validation_result::accept;
   });
   state.connect(source, receiver);
   auto stream = state.open(source, receiver);
   state.send(stream, ps::rpc{.messages = {slow}});
   BOOST_REQUIRE(state.wait([&] { return calls->load() == 1U; }));
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_scores().pending_validations == 0U; }));
   BOOST_TEST(receiver.pubsub_snapshot().cached_messages == 1U); // Actual native callback still owns this row.
   release->notify();
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().cached_messages == 0U; }));
   BOOST_TEST(!delivered(state, receiver, "slow-ttl"));
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(score(receiver, source.local_peer()) == 0.0);
   state.send(stream, ps::rpc{.messages = {next}});
   BOOST_REQUIRE(state.wait([&] { return delivered(state, receiver, "after-ttl"); }));
   BOOST_TEST(calls->load() == 2U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 1U);
   forge::asio::blocking::run(state.runtime, stream.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_first_heartbeat_gossips_single_history_bin_before_cache_shift) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.mesh_n = 1;
   config.limits.mesh_n_low = 1;
   config.limits.mesh_n_high = 1;
   config.limits.mesh_score_min = 0;
   config.limits.history_length = 1;
   config.limits.history_gossip = 1;
   config.limits.max_messages = 1;
   config.limits.heartbeat_initial_delay = 1s;
   auto remote_config = options(state.topic);
   remote_config.limits.heartbeat_initial_delay = 30s;
   auto& owner = state.add("pubsub-one-bin-owner", std::move(config));
   auto& mesh = state.add("pubsub-one-bin-mesh", remote_config);
   auto& off_mesh = state.add("pubsub-one-bin-offmesh", remote_config);
   state.subscribe(owner);
   state.subscribe(mesh);
   state.subscribe(off_mesh);
   state.connect(mesh, owner);
   auto input = state.open(mesh, owner);
   state.send(input, ps::rpc{.subscriptions = {{.subscribe = true, .subject = state.topic}},
      .control_value = ps::control{.grafts = {{.subject = state.topic}}}});
   BOOST_REQUIRE(state.wait([&] { return in_mesh(owner, mesh.local_peer()); }));
   state.connect(off_mesh, owner);
   BOOST_REQUIRE(state.wait([&] { return owner.pubsub_snapshot().peers == 2U; }));
   BOOST_TEST(!in_mesh(owner, off_mesh.local_peer()));
   const auto message = state.publish(owner, "single-bin-gossip");
   const auto id = ps::codec::message_id(message);
   BOOST_REQUIRE(state.wait([&] { return control(state, owner, off_mesh.local_peer(), ps::trace_kind::rpc_write,
      [&](const auto& rpc) {
         return std::ranges::any_of(rpc.have, [&](const auto& have) {
            return have.subject == state.topic && std::ranges::find(have.message_ids, id) != have.message_ids.end();
         });
      }); }));
   BOOST_TEST(!in_mesh(owner, off_mesh.local_peer()));
   BOOST_REQUIRE(state.wait([&] { return owner.pubsub_snapshot().cached_messages == 0U; }));
   forge::asio::blocking::run(state.runtime, input.async_close());
   state.stop();
}

namespace {

void check_native_outbound_quota(std::size_t target, std::size_t low, std::size_t protected_count) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.mesh_n = target;
   config.limits.mesh_n_low = low;
   config.limits.mesh_n_high = target;
   config.limits.mesh_score_min = protected_count;
   config.limits.mesh_outbound_min = 1;
   config.limits.max_peers_per_topic = target + 1;
   config.limits.heartbeat_initial_delay = 1s;
   auto mutex = std::make_shared<std::mutex>();
   auto changed = std::make_shared<std::condition_variable>();
   auto released = std::make_shared<bool>(false);
   auto graft_written = std::make_shared<std::atomic_bool>();
   auto outbound_peer = std::make_shared<p2p::peer_id>();
   config.scoring->app_specific_weight = 1;
   config.scoring->app_specific_score = [outbound_peer](const auto& peer) {
      return peer == *outbound_peer ? 1.0 : 10.0;
   };
   config.tracer = [mutex, changed, released, graft_written, outbound_peer](const ps::trace_event& event) {
      if (event.kind != ps::trace_kind::rpc_write || event.peer != *outbound_peer || event.framed_rpc.empty()) { return; }
      const auto rpc = ps::codec::decode(event.framed_rpc);
      if (!rpc.control_value || rpc.control_value->grafts.empty()) { return; }
      auto lock = std::unique_lock{*mutex};
      graft_written->store(true);
      changed->wait(lock, [&] { return *released; });
   };
   auto remote_config = options(state.topic);
   remote_config.limits.heartbeat_initial_delay = 30s;
   auto& owner = state.add("pubsub-dout-owner", std::move(config));
   auto inbounds = std::vector<p2p::node*>{};
   for (auto index = std::size_t{}; index < target; ++index) {
      inbounds.push_back(&state.add("pubsub-dout-inbound-" + std::to_string(index), remote_config));
   }
   auto& outbound = state.add("pubsub-dout-outbound", remote_config);
   auto& capacity_peer = state.add("pubsub-dout-capacity", remote_config);
   *outbound_peer = outbound.local_peer();
   auto release_on_failure = boost::scope::scope_exit{[mutex, changed, released] {
      { const auto lock = std::scoped_lock{*mutex}; *released = true; }
      changed->notify_all();
   }};
   state.subscribe(owner);
   for (const auto inbound : inbounds) { state.subscribe(*inbound); }
   state.subscribe(outbound);
   state.subscribe(capacity_peer);
   auto inputs = std::vector<p2p::stream>{};
   const auto graft = ps::rpc{.subscriptions = {{.subscribe = true, .subject = state.topic}},
      .control_value = ps::control{.grafts = {{.subject = state.topic}}}};
   for (const auto inbound : inbounds) {
      state.connect(*inbound, owner);
      inputs.push_back(state.open(*inbound, owner));
      state.send(inputs.back(), graft);
   }
   BOOST_REQUIRE(state.wait([&] {
      return owner.pubsub_snapshot().mesh_edges == target &&
         std::ranges::all_of(inbounds, [&](const auto inbound) { return in_mesh(owner, inbound->local_peer()); });
   }));
   state.connect(owner, outbound);
   BOOST_REQUIRE(state.wait([&] { return graft_written->load(); }));
   BOOST_TEST(owner.pubsub_snapshot().mesh_edges == target + 1);
   BOOST_TEST(in_mesh(owner, outbound.local_peer()));
   BOOST_TEST(score(owner, outbound.local_peer()) == 1.0);
   for (const auto inbound : inbounds) { BOOST_TEST(score(owner, inbound->local_peer()) == 10.0); }
   const auto sessions = owner.diagnostics().sessions;
   BOOST_TEST(std::ranges::any_of(sessions, [&](const auto& session) {
      return session.remote_peer == outbound.local_peer() && session.direction == p2p::diagnostics::session_direction::outbound;
   }));
   for (const auto inbound : inbounds) {
      BOOST_TEST(std::ranges::any_of(sessions, [&](const auto& session) {
         return session.remote_peer == inbound->local_peer() && session.direction == p2p::diagnostics::session_direction::inbound;
      }));
   }
   BOOST_TEST(owner.pubsub_snapshot().peers == target + 1);
   { const auto lock = std::scoped_lock{*mutex}; *released = true; }
   changed->notify_all();
   BOOST_REQUIRE(state.wait([&] {
      return owner.pubsub_snapshot().mesh_edges == target && in_mesh(owner, outbound.local_peer()) &&
         std::ranges::any_of(inbounds, [&](const auto inbound) {
            return control(state, owner, inbound->local_peer(), ps::trace_kind::rpc_write,
               [](const auto& rpc) { return !rpc.prunes.empty(); });
         });
   }));
   // The synchronous tracer can retain a listener-strand completion stack. Test the
   // hard subscription capacity after releasing it, not a new handshake behind it.
   BOOST_REQUIRE_EQUAL(owner.pubsub_snapshot().peers, target + 1);
   const auto pressure = owner.metrics().backpressure_rejections;
   state.connect(capacity_peer, owner);
   auto c = state.open(capacity_peer, owner);
   state.send(c, graft);
   BOOST_REQUIRE(state.wait([&] {
      return owner.metrics().backpressure_rejections > pressure &&
         control(state, owner, capacity_peer.local_peer(), ps::trace_kind::rpc_read,
            [](const auto& rpc) { return !rpc.grafts.empty(); }) &&
         control(state, owner, capacity_peer.local_peer(), ps::trace_kind::rpc_write,
            [](const auto& rpc) { return !rpc.prunes.empty(); });
   }));
   BOOST_TEST(owner.pubsub_snapshot().peers == target + 1);
   BOOST_TEST(owner.pubsub_snapshot().mesh_edges == target);
   BOOST_TEST(!in_mesh(owner, capacity_peer.local_peer()));
   BOOST_TEST(in_mesh(owner, outbound.local_peer()));
   BOOST_TEST(!control(state, owner, outbound.local_peer(), ps::trace_kind::rpc_write,
      [](const auto& rpc) { return !rpc.prunes.empty(); }));
   for (auto& input : inputs) { forge::asio::blocking::run(state.runtime, input.async_close()); }
   forge::asio::blocking::run(state.runtime, c.async_close());
   state.stop();
}

} // namespace

BOOST_AUTO_TEST_CASE(native_outbound_quota_can_cross_soft_dhi_then_prune_without_exceeding_topic_capacity) {
   check_native_outbound_quota(2, 2, 0);
}

BOOST_AUTO_TEST_CASE(native_dscore_filled_survivors_still_preserve_physical_outbound_quota) {
   check_native_outbound_quota(4, 3, 4);
}

BOOST_AUTO_TEST_CASE(native_opportunistic_graft_crosses_soft_dhi_and_keeps_better_peer_on_next_prune) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.mesh_n = 2;
   config.limits.mesh_n_low = 2;
   config.limits.mesh_n_high = 2;
   config.limits.mesh_score_min = 1;
   config.limits.max_peers_per_topic = 3;
   config.limits.opportunistic_graft_ticks = 1;
   config.limits.opportunistic_graft_peers = 1;
   config.limits.heartbeat_initial_delay = 1s;
   auto better_peer = std::make_shared<p2p::peer_id>();
   config.scoring->app_specific_weight = 1;
   config.scoring->app_specific_score = [better_peer](const auto& peer) { return peer == *better_peer ? 10.0 : 1.0; };
   auto mutex = std::make_shared<std::mutex>();
   auto changed = std::make_shared<std::condition_variable>();
   auto released = std::make_shared<bool>(false);
   auto graft_written = std::make_shared<std::atomic_bool>();
   config.tracer = [mutex, changed, released, graft_written, better_peer](const ps::trace_event& event) {
      if (event.kind != ps::trace_kind::rpc_write || event.peer != *better_peer || event.framed_rpc.empty()) { return; }
      const auto rpc = ps::codec::decode(event.framed_rpc);
      if (!rpc.control_value || rpc.control_value->grafts.empty()) { return; }
      auto lock = std::unique_lock{*mutex};
      graft_written->store(true);
      changed->wait(lock, [&] { return *released; });
   };
   auto remote_config = options(state.topic);
   remote_config.limits.heartbeat_initial_delay = 30s;
   auto& owner = state.add("pubsub-opportunistic-owner", std::move(config));
   auto& a_peer = state.add("pubsub-opportunistic-a", remote_config);
   auto& b_peer = state.add("pubsub-opportunistic-b", remote_config);
   auto& better = state.add("pubsub-opportunistic-better", remote_config);
   *better_peer = better.local_peer();
   auto release_on_failure = boost::scope::scope_exit{[mutex, changed, released] {
      { const auto lock = std::scoped_lock{*mutex}; *released = true; }
      changed->notify_all();
   }};
   state.subscribe(owner);
   state.subscribe(a_peer);
   state.subscribe(b_peer);
   state.subscribe(better);
   state.connect(a_peer, owner);
   state.connect(b_peer, owner);
   auto a = state.open(a_peer, owner);
   auto b = state.open(b_peer, owner);
   const auto graft = ps::rpc{.subscriptions = {{.subscribe = true, .subject = state.topic}},
      .control_value = ps::control{.grafts = {{.subject = state.topic}}}};
   state.send(a, graft);
   state.send(b, graft);
   BOOST_REQUIRE(state.wait([&] { return in_mesh(owner, a_peer.local_peer()) && in_mesh(owner, b_peer.local_peer()); }));
   state.connect(better, owner);
   BOOST_REQUIRE(state.wait([&] { return graft_written->load(); }));
   BOOST_TEST(owner.pubsub_snapshot().mesh_edges == 3U);
   BOOST_TEST(in_mesh(owner, better.local_peer()));
   BOOST_TEST(score(owner, better.local_peer()) == 10.0);
   { const auto lock = std::scoped_lock{*mutex}; *released = true; }
   changed->notify_all();
   BOOST_REQUIRE(state.wait([&] { return owner.pubsub_snapshot().mesh_edges == 2U && in_mesh(owner, better.local_peer()); }));
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
   forge::asio::blocking::run(state.runtime, a.async_close());
   forge::asio::blocking::run(state.runtime, b.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_positive_disconnect_releases_retention_capacity_for_next_authenticated_peer) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.scoring->limits.max_connected_peers = 1;
   config.scoring->limits.max_retained_peers = 1;
   config.scoring->app_specific_weight = 1;
   config.scoring->app_specific_score = [](const auto&) { return 1.0; };
   auto& owner = state.add("pubsub-retention-owner", std::move(config));
   auto& first = state.add("pubsub-retention-first", options(state.topic));
   auto& next = state.add("pubsub-retention-next", options(state.topic));
   state.subscribe(owner);
   state.subscribe(first);
   state.subscribe(next);
   state.connect(first, owner);
   BOOST_REQUIRE(state.wait([&] { return score(owner, first.local_peer()) == 1.0; }));
   forge::asio::blocking::run(state.runtime, first.async_stop()); // Actual native workers and sessions joined.
   BOOST_REQUIRE(state.wait([&] {
      const auto snapshot = owner.pubsub_scores();
      return snapshot.connected_peers == 0U && snapshot.retained_peers == 0U && snapshot.peers.empty() &&
         owner.metrics().active_sessions == 0U;
   }));
   state.connect(next, owner);
   BOOST_REQUIRE(state.wait([&] {
      const auto snapshot = owner.pubsub_scores();
      return snapshot.connected_peers == 1U && snapshot.peers.size() == 1U &&
         snapshot.peers.front().peer == next.local_peer() && snapshot.peers.front().app_specific_sample == 1.0;
   }));
   BOOST_REQUIRE(state.wait([&] {
      return in_mesh(owner, next.local_peer()) && in_mesh(next, owner.local_peer()) &&
         subscription(state, owner, next.local_peer()) && subscription(state, next, owner.local_peer());
   }));
   static_cast<void>(state.publish(next, "retention-capacity-recovered"));
   BOOST_REQUIRE(state.wait([&] { return delivered(state, owner, "retention-capacity-recovered"); }));
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
   state.stop();
}

BOOST_AUTO_TEST_CASE(router_stop_clear_retires_active_and_staged_without_rebinding_old_tokens) {
   using router_type = p2p::detail::pubsub_router;
   auto router = router_type{2, 7};
   auto random_reference = router_type{2, 7};
   static_assert(noexcept(router.clear()));
   BOOST_TEST(router.choose(256) == random_reference.choose(256));
   const auto now = router_type::clock::time_point{};
   const auto source = peer(1);
   const auto other = peer(2);
   const auto active = router.stage(source, "active");
   const auto staged = router.stage(other, "staged");
   BOOST_REQUIRE(active);
   BOOST_REQUIRE(staged);
   BOOST_REQUIRE(router.activate(source, "active", *active, now + 1s));
   BOOST_TEST(router.pending() == 2U);
   router.clear();
   BOOST_TEST(router.pending() == 0U);
   BOOST_TEST(router.expire(router_type::clock::time_point::max()).empty());
   router.clear();
   BOOST_TEST(router.pending() == 0U);
   BOOST_TEST(router.choose(256) == random_reference.choose(256));

   const auto next_active = router.stage(source, "active");
   const auto next_staged = router.stage(other, "staged");
   BOOST_REQUIRE(next_active);
   BOOST_REQUIRE(next_staged);
   BOOST_TEST(*next_active > *active);
   BOOST_TEST(*next_staged > *staged);
   BOOST_TEST(!router.activate(source, "active", *active, now + 1s));
   BOOST_TEST(!router.activate(other, "staged", *staged, now + 1s));
   router.abort(source, "active", *active);
   router.abort(other, "staged", *staged);
   BOOST_TEST(router.pending() == 2U);
   BOOST_REQUIRE(router.activate(source, "active", *next_active, now + 2s));
   BOOST_TEST(router.expire(now + 1s).empty());
   const auto expired = router.expire(now + 2s);
   BOOST_REQUIRE_EQUAL(expired.size(), 1U);
   BOOST_TEST(expired.at(source) == 1U);
   BOOST_TEST(router.pending() == 1U);
   router.clear();
   BOOST_TEST(router.pending() == 0U);
   BOOST_TEST(!router.activate(other, "staged", *next_staged, now + 3s));
}

BOOST_AUTO_TEST_CASE(staged_iwant_tokens_fulfill_before_activation_and_never_recreate_or_rebind) {
   using router_type = p2p::detail::pubsub_router;
   const auto now = router_type::clock::time_point{};
   auto router = router_type{1, 7};
   const auto source = peer(1);
   const auto id = std::string{"request\0id", 10};
   const auto first = router.stage(source, id);
   BOOST_REQUIRE(first);
   BOOST_TEST(!router.stage(source, id).has_value());
   BOOST_TEST(!router.stage(peer(2), "pressure").has_value());
   BOOST_TEST(router.expire(router_type::clock::time_point::max()).empty()); // Staged I/O has no P7 deadline.
   router.fulfill(id);
   BOOST_TEST(router.pending() == 0U);
   BOOST_TEST(!router.activate(source, id, *first, now + 1s));

   const auto replacement = router.stage(source, id);
   BOOST_REQUIRE(replacement);
   BOOST_TEST(*replacement != *first);
   router.abort(source, id, *first);
   BOOST_TEST(router.pending() == 1U);
   BOOST_TEST(!router.activate(source, id, *first, now + 1s));
   router.abort(source, id, *replacement); // Failed or canceled native send.
   BOOST_TEST(router.expire(now + 2s).empty());
   BOOST_TEST(!router.activate(source, id, *replacement, now + 1s));

   const auto disconnected = router.stage(source, id);
   BOOST_REQUIRE(disconnected);
   router.forget(source);
   const auto reconnected = router.stage(source, id);
   BOOST_REQUIRE(reconnected);
   BOOST_TEST(!router.activate(source, id, *disconnected, now + 1s));
   BOOST_TEST(!router.activate(peer(2), id, *reconnected, now + 1s));
   BOOST_REQUIRE(router.activate(source, id, *reconnected, now + 1s));
   BOOST_TEST(!router.activate(source, id, *reconnected, now + 10s)); // A repeated completion cannot postpone P7.
   BOOST_TEST(router.expire(now + 999ms).empty());
   const auto broken = router.expire(now + 1s);
   BOOST_REQUIRE_EQUAL(broken.size(), 1U);
   BOOST_TEST(broken.at(source) == 1U);
   BOOST_TEST(router.expire(now + 2s).empty());
}

BOOST_AUTO_TEST_CASE(native_verified_second_stream_fulfills_iwant_even_when_cache_is_full) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.limits.history_length = 50;
   config.limits.history_gossip = 1;
   config.limits.max_messages = 1;
   config.limits.iwant_followup_time = 3s;
   config.scoring->behaviour_penalty_weight = -3;
   auto entered = std::make_shared<std::atomic_bool>(false);
   auto requested_stream = std::make_shared<std::atomic<std::int64_t>>(-1);
   auto requested_id = std::make_shared<std::vector<std::uint8_t>>();
   config.tracer = [entered, requested_stream, requested_id](const ps::trace_event& event) {
      if (event.kind != ps::trace_kind::rpc_write || event.framed_rpc.empty()) { return; }
      const auto rpc = ps::codec::decode(event.framed_rpc);
      if (!rpc.control_value || rpc.control_value->want.empty() ||
          rpc.control_value->want.front().message_ids.front() != *requested_id) { return; }
      requested_stream->store(event.stream_id);
      entered->store(true);
   };
   auto& receiver = state.add("pubsub-staged-owner", std::move(config));
   auto& sender = state.add("pubsub-staged-sender", options(state.topic));
   auto& author = state.add("pubsub-staged-author", options(state.topic));
   const auto message = state.publish(author, "verified-pressure");
   BOOST_REQUIRE(ps::codec::verify_message(message));
   *requested_id = ps::codec::message_id(message);
   state.subscribe(receiver);
   state.connect(sender, receiver);
   auto control_stream = state.open(sender, receiver);
   auto message_stream = state.open(sender, receiver);
   BOOST_REQUIRE(control_stream.id() != message_stream.id());
   state.send(control_stream, ps::rpc{.control_value = ps::control{
      .have = {{.subject = state.topic, .message_ids = {*requested_id}}}}});
   BOOST_REQUIRE(state.wait([&] { return entered->load(); }));
   BOOST_REQUIRE(requested_stream->load() >= 0);
   // Stage the obligation before pressure: a full cache must not create new delivery promises.
   for (auto index = 0; index < 50; ++index) {
      static_cast<void>(state.publish(receiver, "full-cache-" + std::to_string(index)));
   }
   BOOST_REQUIRE_EQUAL(receiver.pubsub_snapshot().cached_messages, 50U);
   // The entity test above proves pre-activation races; this nonblocking native observer proves real I/O.
   const auto pressure = receiver.metrics().backpressure_rejections;
   state.send(message_stream, ps::rpc{.messages = {message}});
   const auto pressured = state.wait([&] { return receiver.metrics().backpressure_rejections > pressure; });
   const auto snapshot = receiver.pubsub_snapshot();
   const auto metrics = receiver.metrics();
   BOOST_REQUIRE_MESSAGE(pressured, "verified cache-pressure arrival was not rejected: cached="
      << snapshot.cached_messages << " received=" << snapshot.messages_received
      << " invalid=" << snapshot.invalid_messages << " duplicates=" << snapshot.duplicates
      << " protocol_rejections=" << metrics.protocol_rejections << " sessions=" << metrics.active_sessions
      << " sender_score=" << score(receiver, sender.local_peer()));
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(!delivered(state, receiver, "verified-pressure"));
   // Free capacity through the real heartbeat before creating a fresh, genuinely unanswered obligation.
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().cached_messages == 0U; }));
   const auto missing = std::vector<std::uint8_t>{0, 1, 0, 2};
   state.send(control_stream, ps::rpc{.control_value = ps::control{
      .have = {{.subject = state.topic, .message_ids = {missing}}}}});
   BOOST_REQUIRE(state.wait([&] {
      const auto row = peer_score(receiver, sender.local_peer());
      return row && row->behaviour_penalty > 0.5;
   }));
   const auto row = peer_score(receiver, sender.local_peer());
   BOOST_REQUIRE(row);
   BOOST_TEST(row->behaviour_penalty <= 1.0); // Only the second promise broke, not the verified cache-pressure arrival.
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   forge::asio::blocking::run(state.runtime, control_stream.async_close());
   forge::asio::blocking::run(state.runtime, message_stream.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(native_invalid_signature_does_not_fulfill_iwant_promise) {
   auto state = fixture{};
   auto config = options(state.topic);
   config.scoring->behaviour_penalty_weight = -3;
   config.scoring->topics.at(state.topic).invalid_message_deliveries_weight = -1;
   auto& receiver = state.add("pubsub-invalid-promise-owner", std::move(config));
   auto& sender = state.add("pubsub-invalid-promise-sender", options(state.topic));
   auto& author = state.add("pubsub-invalid-promise-author", options(state.topic));
   auto message = state.publish(author, "invalid-promise");
   const auto id = ps::codec::message_id(message);
   BOOST_REQUIRE(!message.signature.empty());
   message.signature.front() ^= 1;
   state.subscribe(receiver);
   state.connect(sender, receiver);
   auto stream = state.open(sender, receiver);
   state.send(stream, ps::rpc{.control_value = ps::control{.have = {{.subject = state.topic, .message_ids = {id}}}}});
   BOOST_REQUIRE(state.wait([&] { return control(state, receiver, sender.local_peer(), ps::trace_kind::rpc_write,
      [&](const auto& rpc) { return !rpc.want.empty() && rpc.want.front().message_ids.front() == id; }); }));
   state.send(stream, ps::rpc{.messages = {message}});
   BOOST_REQUIRE(state.wait([&] {
      const auto row = peer_score(receiver, sender.local_peer());
      return receiver.pubsub_snapshot().invalid_messages == 1U && row && row->behaviour_penalty > 0.5;
   }));
   BOOST_TEST(!delivered(state, receiver, "invalid-promise"));
   forge::asio::blocking::run(state.runtime, stream.async_close());
   state.stop();
}

BOOST_AUTO_TEST_CASE(iwant_followup_duration_is_representable_and_deadline_saturates_without_early_expiry) {
   using router = p2p::detail::pubsub_router;
   const auto maximum_duration = std::chrono::duration_cast<std::chrono::milliseconds>(router::clock::duration::max());
   auto config = ps::options{};
   config.limits.iwant_followup_time = maximum_duration;
   BOOST_CHECK_NO_THROW(ps::validate(config));
   if (maximum_duration < std::chrono::milliseconds::max()) {
      config.limits.iwant_followup_time = maximum_duration + 1ms;
      BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
      config.limits.iwant_followup_time = std::chrono::milliseconds::max();
      BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
   }
   for (const auto invalid : {0ms, -1ms}) {
      config.limits.iwant_followup_time = invalid;
      BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
   }
   const auto start = router::clock::time_point{};
   BOOST_CHECK(router::deadline(start, 3s) == start + 3s);
   BOOST_CHECK(router::deadline(start, maximum_duration) == start +
      std::chrono::duration_cast<router::clock::duration>(maximum_duration));
   const auto latest = router::clock::time_point::max();
   BOOST_CHECK(router::deadline(latest - 1ms, 2ms) == latest);
   BOOST_CHECK(router::deadline(latest, 1ms) == latest);
   BOOST_CHECK(router::deadline(latest, std::chrono::milliseconds::max()) == latest);
   BOOST_CHECK(router::deadline(router::clock::time_point::min(), 1ms) == router::clock::time_point::min() + 1ms);
   auto promises = router{1, 7};
   BOOST_REQUIRE(promises.promise(peer(1), "saturated", router::deadline(latest - 1ms, 2ms)));
   BOOST_TEST(promises.expire(latest - router::clock::duration{1}).empty());
   const auto expired = promises.expire(latest);
   BOOST_REQUIRE_EQUAL(expired.size(), 1U);
   BOOST_TEST(expired.at(peer(1)) == 1U);
   BOOST_TEST(promises.pending() == 0U);
}

BOOST_AUTO_TEST_CASE(unpublished_connection_preparation_failure_settles_coalesced_waiter_and_allows_retry) {
   using registry_type = p2p::detail::connection_singleflight_registry;
   struct throwing_callback {
      throwing_callback() = default;
      throwing_callback(const throwing_callback&) { throw std::bad_alloc{}; }
      boost::asio::awaitable<void> operator()() const { co_return; }
   };
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   for (const auto before_shared_owner : {true, false}) {
      auto registry = registry_type{};
      auto close = boost::scope::scope_exit{[&registry] { registry.close(); }};
      auto other = registry_type::lease{};
      auto failed_prepare = [&] {
         auto participant = registry_type::lease{};
         auto start = std::optional<registry_type::operation>{};
         auto active = std::shared_ptr<registry_type::operation>{};
         auto rollback = [&registry, &participant, &start, &active](void*) noexcept {
            if (start) { registry.rollback_unpublished(active ? *active : *start, participant); }
         };
         auto guard = std::unique_ptr<void, decltype(rollback)>{&registry, std::move(rollback)};
         auto joined = registry.join(peer(1), runtime.context().get_executor());
         participant = std::move(joined.participant);
         start = std::move(joined.start);
         auto leave = [&registry, &participant, &start, &guard](void*) noexcept {
            if (!start || !guard) { registry.leave(participant); }
         };
         auto participant_guard = std::unique_ptr<void, decltype(leave)>{&registry, std::move(leave)};
         auto coalesced = registry.join(peer(1), runtime.context().get_executor());
         other = std::move(coalesced.participant);
         BOOST_REQUIRE(start);
         if (before_shared_owner) {
            // Exact post-join/pre-make_shared boundary; this is an explicit allocation-failure model.
            throw std::bad_alloc{};
         }
         active = std::make_shared<registry_type::operation>(std::move(*start));
         const auto callback = throwing_callback{};
         auto prepared = std::function<boost::asio::awaitable<void>()>{callback};
         static_cast<void>(prepared);
      };
      BOOST_CHECK_THROW(failed_prepare(), std::bad_alloc);
      const auto result = forge::asio::blocking::run(runtime, other.wait());
      BOOST_TEST(!result.succeeded);
      BOOST_CHECK(result.error == p2p::exceptions::code::internal);
      registry.leave(other);
      BOOST_TEST(registry.size() == 0U);
      auto retry = registry.join(peer(1), runtime.context().get_executor());
      BOOST_REQUIRE(retry.start);
      registry.succeed(*retry.start);
      registry.leave(retry.participant);
      BOOST_TEST(registry.size() == 0U);
   }
}

BOOST_AUTO_TEST_CASE(outbound_reference_guard_cannot_allocate_after_admission_and_releases_on_prepare_failure) {
   auto budget = p2p::detail::pubsub_outbound_budget{};
   const auto owner = peer(1);
   auto bytes = std::size_t{16};
   const auto failed_prepare = [&] {
      BOOST_REQUIRE(budget.reserve(owner, bytes, 16));
      auto release = [&budget, &owner, &bytes](void*) noexcept { budget.release(owner, bytes); };
      using guard_type = std::unique_ptr<void, decltype(release)>;
      static_assert(std::is_nothrow_move_constructible_v<decltype(release)>);
      static_assert(std::is_nothrow_constructible_v<guard_type, void*, decltype(release)&&>);
      auto guard = guard_type{&budget, std::move(release)};
      throw std::bad_alloc{};
   };
   BOOST_CHECK_THROW(failed_prepare(), std::bad_alloc);
   BOOST_TEST(budget.total() == 0U);
   BOOST_TEST(budget.peers() == 0U);
   BOOST_REQUIRE(budget.reserve(peer(2), 16, 16));
   budget.release(peer(2), 16);
}

BOOST_AUTO_TEST_CASE(outbound_budget_throwing_admission_is_not_noexcept_and_capacity_is_transactional) {
   auto budget = p2p::detail::pubsub_outbound_budget{};
   static_assert(!noexcept(budget.reserve(peer(1), 8, 16)));
   BOOST_REQUIRE(budget.reserve(peer(1), 8, 16));
   BOOST_TEST(!budget.reserve(peer(2), 9, 16));
   BOOST_TEST(budget.peers() == 1U);
   BOOST_TEST(budget.total() == 8U);
   budget.release(peer(1), 8);
   BOOST_TEST(budget.peers() == 0U);
   BOOST_REQUIRE(budget.reserve(peer(2), 16, 16));
}

BOOST_AUTO_TEST_SUITE_END()
