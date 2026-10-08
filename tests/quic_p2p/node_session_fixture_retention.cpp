module;

#include <forge/exceptions/macros.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
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
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/compat/move_only_function.hpp>
#include "libp2p_identity_fixture.hxx"
#include "pubsub_claim_allocation.hxx"

module forge.net.p2p.node;
import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.multiformats.multiaddr;
import forge.net.transport.exceptions;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.exceptions;
import forge.net.yamux.session;

#include "../../libraries/net/p2p/details/node_impl.hxx"
#include "../../libraries/net/p2p/details/pubsub_peer_score.hxx"
#include "../../libraries/net/p2p/details/pubsub_router.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;

void node_session_fixture::positive_retention(retirement kind) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto config = options("pubsub-p1-retirement");
   config.limits.pubsub.scoring->limits.max_connected_peers = 1;
   config.limits.pubsub.scoring->limits.max_retained_peers = 1;
   if (kind == retirement::admission) {
      config.limits.max_sessions = 1;
      config.limits.session_low_watermark = 1;
   }
   auto owner = node{runtime, std::move(config)};
   const auto self = owner.impl_;
   const auto old = peer(1);
   const auto next = peer(2);
   seed(owner, old, 1);
   outbound(owner, old, 1);
   const auto before = owner.pubsub_scores();
   BOOST_REQUIRE_EQUAL(before.peers.size(), 1U);
   BOOST_REQUIRE(before.peers.front().value > 0.0);
   BOOST_TEST(before.peers.front().app_specific_score == 0.0);
   BOOST_TEST(before.peers.front().topics.front().first_message_deliveries == 0.0);
   switch (kind) {
   case retirement::peer: self->forget_session(old); break;
   case retirement::single: self->forget_session(self->session_for(old)); break;
   case retirement::topology:
      forge::asio::blocking::run(runtime, self->async_close_topology_sessions({1, 1, 999}));
      break;
   case retirement::admission: admit(runtime, owner, next, 2); break;
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(!self->pubsub_value.peers.contains(old));
      BOOST_TEST(!self->pubsub_value.scores.contains(old));
      BOOST_TEST(!self->sessions.contains(1));
   }
   BOOST_TEST(!self->pubsub_value.scoring->inspect(old, std::chrono::steady_clock::now()).has_value());
   if (kind != retirement::admission) { admit(runtime, owner, next, 2); }
   const auto after = owner.pubsub_scores();
   BOOST_TEST(after.retained_peers == 0U);
   BOOST_REQUIRE_EQUAL(after.peers.size(), 1U);
   BOOST_CHECK(after.peers.front().peer == next);
   BOOST_TEST(after.peers.front().connected);
   forge::asio::blocking::run(runtime, owner.async_stop());
}

void node_session_fixture::whole_batch(bool admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto config = options("pubsub-p1-whole-batch");
   config.limits.max_sessions = 3;
   config.limits.session_low_watermark = 1;
   auto owner = node{runtime, std::move(config)};
   const auto self = owner.impl_;
   const auto old = peer(3);
   const auto survivor = peer(4);
   seed(owner, old, 1);
   seed(owner, old, 2);
   outbound(owner, old, 1);
   BOOST_REQUIRE(owner.pubsub_scores().peers.front().value > 0.0);
   if (admission) {
      seed(owner, survivor, 3);
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->connections.protect(survivor, "pubsub-test-survivor");
      }
      admit(runtime, owner, survivor, 4);
   } else {
      forge::asio::blocking::run(runtime, self->async_close_topology_sessions({2, 1, 2, 999}));
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(!self->sessions.contains(1));
      BOOST_TEST(!self->sessions.contains(2));
      BOOST_TEST(!self->pubsub_value.peers.contains(old));
      if (admission) {
         BOOST_TEST(self->sessions.contains(3));
         BOOST_TEST(self->sessions.contains(4));
         BOOST_TEST(self->pubsub_value.peers.at(survivor).connected);
      }
   }
   BOOST_TEST(!self->pubsub_value.scoring->inspect(old, std::chrono::steady_clock::now()).has_value());
   BOOST_TEST(owner.pubsub_scores().retained_peers == 0U);
   forge::asio::blocking::run(runtime, owner.async_stop());
}

void node_session_fixture::surviving_session(bool replacement) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto config = options("pubsub-same-peer-survivor");
   if (replacement) {
      config.limits.max_sessions = 1;
      config.limits.session_low_watermark = 1;
   }
   auto owner = node{runtime, std::move(config)};
   const auto self = owner.impl_;
   const auto remote = peer(5);
   seed(owner, remote, 1);
   outbound(owner, remote, 1);
   BOOST_REQUIRE(owner.pubsub_scores().peers.front().value > 0.0);
   const auto generation = self->pubsub_value.peers.at(remote).generation;
   if (replacement) {
      admit(runtime, owner, remote, 2);
   } else {
      seed(owner, remote, 2);
      const auto selected = self->sessions.at(1);
      self->forget_session(selected);
   }
   const auto after = owner.pubsub_scores();
   BOOST_TEST(after.connected_peers == 1U);
   BOOST_TEST(after.retained_peers == 0U);
   BOOST_REQUIRE_EQUAL(after.peers.size(), 1U);
   BOOST_TEST(after.peers.front().connected);
   BOOST_TEST(!after.peers.front().retain_until.has_value());
   BOOST_TEST(!after.peers.front().topics.front().in_mesh); // The selected outbound still performs normal PRUNE.
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.peers.at(remote).generation == generation);
      BOOST_TEST(self->sessions.contains(2));
      BOOST_TEST(!self->sessions.contains(1));
   }
   forge::asio::blocking::run(runtime, owner.async_stop());
}

void node_session_fixture::frozen_negative() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto config = options("pubsub-negative-terminal");
   auto& topic = config.limits.pubsub.scoring->topics.begin()->second;
   topic.time_in_mesh_weight = 0;
   topic.mesh_message_deliveries_weight = -2;
   topic.mesh_message_deliveries_threshold = 3;
   topic.mesh_message_deliveries_activation = 1s;
   topic.mesh_failure_penalty_weight = -3;
   topic.mesh_failure_penalty_decay = 0.5;
   auto owner = node{runtime, std::move(config)};
   const auto self = owner.impl_;
   const auto remote = peer(6);
   seed(owner, remote, 1);
   outbound(owner, remote, 1);
   BOOST_REQUIRE(owner.pubsub_scores().peers.front().value < 0.0);
   self->forget_session(remote);
   const auto before = owner.pubsub_scores();
   BOOST_REQUIRE_EQUAL(before.peers.size(), 1U);
   BOOST_TEST(!before.peers.front().connected);
   BOOST_TEST(before.peers.front().topics.front().mesh_failure_penalty == 9.0);
   const auto expiry = before.peers.front().retain_until;
   const auto later = std::chrono::steady_clock::now() + 1s;
   const auto subject = std::string{"forge.pubsub.terminal"};
   auto allocations = std::size_t{};
   auto repeated_disconnect = true;
   {
      const auto lock = std::scoped_lock{self->mutex};
      auto guard = forge::tests::p2p::pubsub_claim_allocation{1};
      self->disconnect_pubsub_peer_locked(remote, later);
      self->forget_pubsub_peer_locked(remote);
      self->prune_pubsub_peer_locked(subject, remote);
      repeated_disconnect = self->pubsub_value.scoring->disconnect(remote, later);
      self->pubsub_value.scoring->tick(later);
      allocations = guard.calls();
   }
   BOOST_TEST(allocations == 0U);
   BOOST_TEST(!repeated_disconnect);
   const auto after = self->pubsub_value.scoring->inspect(remote, later);
   BOOST_REQUIRE(after);
   BOOST_TEST(after->value == before.peers.front().value);
   BOOST_TEST(after->topics.front().mesh_failure_penalty == 9.0);
   BOOST_CHECK(after->retain_until == expiry);
   BOOST_CHECK(self->pubsub_value.peers.at(remote).retain_until == *expiry);
   forge::asio::blocking::run(runtime, owner.async_stop());
}

void node_session_fixture::fail_terminal_join() noexcept {
   std::fputs("FATAL: terminal taxonomy fixture has an unjoined native stop owner\n", stderr);
   std::fflush(stderr);
   std::_Exit(86);
}

void node_session_fixture::terminal_close(terminal_result result) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto owner = node{runtime, options("pubsub-terminal-close-taxonomy")};
   const auto self = owner.impl_;
   const auto remote = peer(13);
   const auto baseline = self->resources.current();
   seed(owner, remote, 1);
   const auto session = self->session_for(remote);
   BOOST_REQUIRE(session);
   auto resource = self->resources.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(resource);
   session->resource = std::move(*resource);
   BOOST_REQUIRE(session->resource.establish({.peer = remote}) == resource_manager::transition_result::accepted);
   constexpr auto memory_bytes = std::uint64_t{4096};
   auto memory = session->resource.reserve_memory(memory_bytes);
   auto descriptor = session->resource.reserve_file_descriptors(1);
   BOOST_REQUIRE(memory);
   BOOST_REQUIRE(descriptor);
   BOOST_REQUIRE(session->retirement.track(self->teardown.track()));
   auto native_ticket = std::make_shared<detail::session_teardown::ticket>(self->teardown.track());
   BOOST_REQUIRE(native_ticket->active());
   const auto weak_native = std::weak_ptr{native_ticket};
   session->native_lifetime = native_ticket;
   auto entered = std::make_shared<std::promise<void>>();
   auto close_entered = entered->get_future();
   auto barrier = std::make_shared<forge::asio::notification>();
   auto barrier_passed = std::make_shared<std::atomic_bool>(false);
   auto model = std::make_shared<terminal_transport>(result, entered, barrier, barrier_passed,
       std::move(native_ticket), std::move(*memory), std::move(*descriptor));
   const auto weak_model = std::weak_ptr{model};
   session->connection = forge::net::transport::detail::session_access::make(std::move(model));
   auto stop = std::future<void>{};
   auto failure = std::exception_ptr{};
   auto launch_attempted = false;
   auto stop_joined = false;
   const auto join_stop = [&] {
      barrier->notify();
      if (stop_joined) { return; }
      if (!stop.valid()) {
         if (launch_attempted) { fail_terminal_join(); }
         return;
      }
      if (stop.wait_for(5s) != std::future_status::ready) { fail_terminal_join(); }
      try { stop.get(); }
      catch (...) { failure = std::current_exception(); }
      stop_joined = true;
   };
   // Assertions or launch failures must never unwind an owner with a live native close coroutine.
   auto terminal_guard = boost::scope::scope_exit{[&]() noexcept {
      try { join_stop(); }
      catch (...) { fail_terminal_join(); }
   }};
   launch_attempted = true;
   stop = boost::asio::co_spawn(runtime.context(), owner.async_stop(), boost::asio::use_future);
   const auto entered_before_deadline = close_entered.wait_for(5s) == std::future_status::ready;
   // Release even on a failed observation. The captured epoch also handles a late native close entry.
   BOOST_CHECK(entered_before_deadline);
   BOOST_CHECK(stop.wait_for(0s) == std::future_status::timeout);
   BOOST_TEST(!barrier_passed->load());
   BOOST_TEST(!weak_model.expired());
   BOOST_TEST(!weak_native.expired());
   const auto held = self->resources.current();
   BOOST_TEST(held.system.memory == baseline.system.memory + memory_bytes);
   BOOST_TEST(held.system.file_descriptors == baseline.system.file_descriptors + 1U);
   BOOST_TEST(held.system.outbound_connections == baseline.system.outbound_connections + 1U);
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(!session->retirement.terminal());
      BOOST_TEST(session->retirement.tracked());
      BOOST_TEST(session->resource.active());
      BOOST_TEST(self->retiring_sessions.contains(1));
   }
   join_stop();
   BOOST_TEST(barrier_passed->load());
   BOOST_TEST(weak_model.expired());
   BOOST_TEST(weak_native.expired());
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->sessions.empty());
      BOOST_TEST(self->retiring_sessions.empty());
      BOOST_TEST(session->retirement.terminal());
      BOOST_TEST(!session->retirement.tracked());
      BOOST_TEST(!session->connection.valid());
      BOOST_TEST(!session->resource.active());
      BOOST_TEST(!session->native_lifetime);
   }
   const auto released = self->resources.current();
   BOOST_TEST(released.system.memory == baseline.system.memory);
   BOOST_TEST(released.system.file_descriptors == baseline.system.file_descriptors);
   BOOST_TEST(released.system.outbound_connections == baseline.system.outbound_connections);
   BOOST_TEST(released.invalid_transitions == baseline.invalid_transitions);
   const auto accepted = result == terminal_result::success || result == terminal_result::transport_closed ||
       result == terminal_result::transport_canceled || result == terminal_result::yamux_closed ||
       result == terminal_result::yamux_canceled;
   if (accepted) {
      BOOST_CHECK_MESSAGE(!failure, "an orderly terminal close must be accepted after cleanup");
      return;
   }
   BOOST_REQUIRE_MESSAGE(failure, "a non-terminal close failure must survive cleanup");
   try {
      std::rethrow_exception(failure);
   } catch (const forge::net::yamux::exceptions::protocol_error& error) {
      BOOST_CHECK(result == terminal_result::yamux_protocol);
      BOOST_CHECK(forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::protocol_error));
   } catch (const forge::net::transport::exceptions::protocol_error& error) {
      BOOST_CHECK(result == terminal_result::transport_protocol);
      BOOST_CHECK(forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::protocol_error));
   } catch (const std::system_error& error) {
      BOOST_CHECK(result == terminal_result::io);
      BOOST_CHECK(error.code() == std::make_error_code(std::errc::io_error));
   } catch (const std::bad_alloc&) {
      BOOST_CHECK(result == terminal_result::allocation);
   } catch (...) {
      BOOST_ERROR("terminal close failure changed its native exception type/category");
   }
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_SUITE(pubsub_terminal)
BOOST_AUTO_TEST_CASE(p1_positive_peer_forget_frees_retention_slot) {
   forge::net::p2p::node_session_fixture::positive_retention(forge::net::p2p::node_session_fixture::retirement::peer);
}
BOOST_AUTO_TEST_CASE(p1_positive_single_forget_frees_retention_slot) {
   forge::net::p2p::node_session_fixture::positive_retention(forge::net::p2p::node_session_fixture::retirement::single);
}
BOOST_AUTO_TEST_CASE(p1_positive_topology_retirement_frees_retention_slot) {
   forge::net::p2p::node_session_fixture::positive_retention(forge::net::p2p::node_session_fixture::retirement::topology);
}
BOOST_AUTO_TEST_CASE(p1_positive_admission_prune_frees_retention_slot) {
   forge::net::p2p::node_session_fixture::positive_retention(forge::net::p2p::node_session_fixture::retirement::admission);
}
BOOST_AUTO_TEST_CASE(topology_prefinalizes_both_selected_same_peer_sessions) {
   forge::net::p2p::node_session_fixture::whole_batch(false);
}
BOOST_AUTO_TEST_CASE(admission_prefinalizes_entire_selected_batch) {
   forge::net::p2p::node_session_fixture::whole_batch(true);
}
BOOST_AUTO_TEST_CASE(one_of_two_sessions_keeps_peer_connected_and_normal_prune) {
   forge::net::p2p::node_session_fixture::surviving_session(false);
}
BOOST_AUTO_TEST_CASE(newly_published_admission_replacement_is_a_real_survivor) {
   forge::net::p2p::node_session_fixture::surviving_session(true);
}
BOOST_AUTO_TEST_CASE(disconnected_negative_score_is_frozen_p3b_once_and_no_allocation) {
   forge::net::p2p::node_session_fixture::frozen_negative();
}
BOOST_AUTO_TEST_CASE(engine_disconnect_prune_repeat_trace_is_allocation_free_and_p3b_once) {
   auto config = forge::net::p2p::pubsub::scoring_params{};
   const auto subject = forge::net::p2p::pubsub::topic{"forge.pubsub.terminal"};
   auto params = forge::net::p2p::pubsub::topic_score_params{};
   params.mesh_message_deliveries_weight = -2;
   params.mesh_message_deliveries_threshold = 3;
   params.mesh_message_deliveries_activation = std::chrono::seconds{1};
   params.mesh_failure_penalty_weight = -3;
   config.topics.emplace(subject, params);
   const auto start = std::chrono::steady_clock::time_point{};
   auto scores = forge::net::p2p::detail::pubsub_peer_score{config, start};
   const auto remote = forge::net::p2p::node_session_fixture::peer(12);
   BOOST_REQUIRE(scores.connect(remote, {}, start));
   BOOST_REQUIRE(scores.graft(remote, subject, start));
   const auto activation = start + params.mesh_message_deliveries_activation;
   const auto active = activation + config.decay_interval;
   scores.tick(activation);
   BOOST_TEST(scores.score(remote, activation) == 0.0); // Activation is strictly after the threshold.
   scores.tick(active);
   BOOST_TEST(scores.score(remote, active) == -18.0);
   auto disconnected = false;
   auto pruned_again = true;
   auto disconnected_again = true;
   auto allocations = std::size_t{};
   auto value = double{};
   {
      auto guard = forge::tests::p2p::pubsub_claim_allocation{1};
      disconnected = scores.disconnect(remote, active);
      pruned_again = scores.prune(remote, subject, active);
      disconnected_again = scores.disconnect(remote, active + config.decay_interval);
      scores.tick(active + 2 * config.decay_interval);
      value = scores.score(remote, active + 2 * config.decay_interval);
      allocations = guard.calls();
   }
   BOOST_TEST(disconnected);
   BOOST_TEST(!pruned_again);
   BOOST_TEST(!disconnected_again);
   BOOST_TEST(allocations == 0U);
   BOOST_TEST(value == -27.0);
   BOOST_TEST(scores.inspect(remote, active + 2 * config.decay_interval)->topics.front().mesh_failure_penalty == 9.0);
}
BOOST_AUTO_TEST_CASE(terminal_stop_success_waits_for_native_barrier_and_releases_owners) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::success);
}
BOOST_AUTO_TEST_CASE(terminal_stop_transport_closed_is_accepted_after_barrier) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::transport_closed);
}
BOOST_AUTO_TEST_CASE(terminal_stop_transport_canceled_is_accepted_after_barrier) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::transport_canceled);
}
BOOST_AUTO_TEST_CASE(terminal_stop_yamux_closed_is_accepted_after_barrier) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::yamux_closed);
}
BOOST_AUTO_TEST_CASE(terminal_stop_yamux_canceled_is_accepted_after_barrier) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::yamux_canceled);
}
BOOST_AUTO_TEST_CASE(terminal_stop_yamux_protocol_error_is_preserved_after_cleanup) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::yamux_protocol);
}
BOOST_AUTO_TEST_CASE(terminal_stop_transport_protocol_error_is_preserved_after_cleanup) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::transport_protocol);
}
BOOST_AUTO_TEST_CASE(terminal_stop_unrelated_io_error_is_preserved_after_cleanup) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::io);
}
BOOST_AUTO_TEST_CASE(terminal_stop_allocation_error_is_preserved_after_cleanup) {
   forge::net::p2p::node_session_fixture::terminal_close(forge::net::p2p::node_session_fixture::terminal_result::allocation);
}
BOOST_AUTO_TEST_SUITE_END()
