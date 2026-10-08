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
import forge.net.yamux.session;

#include "../../libraries/net/p2p/details/node_impl.hxx"
#include "../../libraries/net/p2p/details/pubsub_peer_score.hxx"
#include "../../libraries/net/p2p/details/pubsub_router.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;

node_session_fixture::throwing_handler_copy::throwing_handler_copy(
    std::shared_ptr<std::atomic_bool> value, std::function<void()> callback)
    : armed(std::move(value)), observe(std::move(callback)) {}

node_session_fixture::throwing_handler_copy::throwing_handler_copy(const throwing_handler_copy& other)
    : armed(other.armed), observe(other.observe) {
   if (armed->load()) { observe(); throw std::bad_alloc{}; }
}

boost::asio::awaitable<pubsub::validation_result> node_session_fixture::throwing_handler_copy::operator()(pubsub::event) const {
   co_return pubsub::validation_result::accept;
}

namespace {

pubsub::message signed_message(const peer_id& author, const forge::crypto::asymmetric::private_key& key,
                               std::uint8_t sequence) {
   auto value = pubsub::message{.from = author, .data = {sequence},
      .seqno = {0, 0, 0, 0, 0, 0, 0, sequence}, .subject = {"forge.pubsub.terminal"}};
   pubsub::codec::sign_message(value, key);
   return value;
}

} // namespace

void node_session_fixture::preclaim_and_pressure() {
   for (const auto full : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto owner = node{runtime, options("pubsub-preclaim-order")};
      const auto self = owner.impl_;
      const auto remote = peer(7);
      seed(owner, remote, 1);
      const auto value = signed_message(owner.local_peer(), *self->identity.private_key, 1);
      BOOST_REQUIRE(pubsub::codec::verify_message(value));
      const auto id = pubsub::codec::message_id(value);
      const auto key = bytes_key(id);
      auto request = std::optional<node::impl::pubsub_state::request>{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->pubsub_value.router->fulfill(key); // Verified arrival's first critical section.
         request = self->stage_pubsub_request_locked(remote, {id}); // Deterministic pre-claim interleaving.
         BOOST_REQUIRE(request);
         if (full) {
            auto filler = signed_message(owner.local_peer(), *self->identity.private_key, 2);
            const auto filler_key = bytes_key(pubsub::codec::message_id(filler));
            self->remember_local_pubsub_message_locked(filler_key, std::move(filler));
         }
      }
      const auto claim = self->claim_pubsub_message(remote, key, value, false);
      BOOST_CHECK(claim.status == (full ? node::impl::pubsub_state::claim_status::backpressured
                                      : node::impl::pubsub_state::claim_status::claimed));
      self->finish_pubsub_request(*request, true); // Late native write completion cannot recreate the fulfilled token.
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_TEST(self->pubsub_value.router->pending() == 0U);
         BOOST_TEST(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
         if (full) {
            BOOST_TEST(!self->stage_pubsub_request_locked(remote, {id}).has_value());
            BOOST_TEST(self->pubsub_value.router->pending() == 0U);
         }
      }
      if (!full) {
         BOOST_REQUIRE(self->complete_pubsub_message(key, claim.generation, pubsub::validation_result::accept));
      } else {
         // Invoke the real heartbeat/history transition, not a cache clear or a test clock.
         forge::asio::blocking::run(runtime, self->pubsub_heartbeat_once());
         BOOST_TEST(owner.pubsub_snapshot().cached_messages == 0U);
         {
            const auto lock = std::scoped_lock{self->mutex};
            request = self->stage_pubsub_request_locked(remote, {id});
            BOOST_REQUIRE(request);
         }
         self->finish_pubsub_request(*request, true);
         const auto lock = std::scoped_lock{self->mutex};
         const auto expired = self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h);
         BOOST_REQUIRE_EQUAL(expired.size(), 1U);
         BOOST_TEST(expired.at(remote) == 1U);
      }
      BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
      forge::asio::blocking::run(runtime, owner.async_stop());
   }
}

void node_session_fixture::refusals() {
   for (const auto scoring_pressure : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto config = options("pubsub-claim-refusal");
      config.limits.pubsub.limits.max_messages = 4;
      config.limits.pubsub.limits.max_validation_queue = scoring_pressure ? 4 : 1;
      config.limits.pubsub.scoring->topics.begin()->second.time_in_mesh_weight = 0;
      if (scoring_pressure) { config.limits.pubsub.scoring->limits.max_messages = 1; }
      auto owner = node{runtime, std::move(config)};
      const auto self = owner.impl_;
      const auto remote = peer(8);
      seed(owner, remote, 1);
      const auto first = signed_message(owner.local_peer(), *self->identity.private_key, 3);
      const auto second = signed_message(owner.local_peer(), *self->identity.private_key, 4);
      BOOST_REQUIRE(pubsub::codec::verify_message(second));
      const auto first_key = bytes_key(pubsub::codec::message_id(first));
      const auto second_id = pubsub::codec::message_id(second);
      const auto second_key = bytes_key(second_id);
      const auto active = self->claim_pubsub_message(remote, first_key, first, true);
      BOOST_REQUIRE(active.status == node::impl::pubsub_state::claim_status::claimed);
      auto request = std::optional<node::impl::pubsub_state::request>{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         request = self->stage_pubsub_request_locked(remote, {second_id});
         BOOST_REQUIRE(request);
      }
      const auto refused = self->claim_pubsub_message(remote, second_key, second, true);
      BOOST_REQUIRE(refused.status == node::impl::pubsub_state::claim_status::backpressured);
      self->finish_pubsub_request(*request, true);
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_TEST(self->pubsub_value.router->pending() == 0U);
         BOOST_TEST(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
         BOOST_TEST(self->pubsub_value.active_validations == 1U);
         BOOST_TEST(self->pubsub_value.active_validations_by_peer.at(remote) == 1U);
         BOOST_CHECK(self->pubsub_value.validations.at(second_key).state ==
                     node::impl::pubsub_state::validation::status::retryable);
      }
      BOOST_REQUIRE(self->complete_pubsub_message(first_key, active.generation, pubsub::validation_result::ignore));
      self->finish_pubsub_validation(remote);
      BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
      BOOST_TEST(owner.pubsub_scores().peers.front().behaviour_penalty == 0.0);
      forge::asio::blocking::run(runtime, owner.async_stop());
   }
}

void node_session_fixture::handler_copy_failure() {
   for (const auto fail_stage : {false, true}) {
      const auto source_options = options("pubsub-handler-copy-source");
      const auto source_identity = make_libp2p_identity_material(source_options);
      const auto remote = make_peer_id_from_certificate_pem(source_options.certificate_pem);
      BOOST_REQUIRE(source_identity.private_key);
      auto weak_owner = std::weak_ptr<node::impl>{};
      {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
         auto owner = node{runtime, options("pubsub-handler-copy")};
         const auto self = owner.impl_;
         weak_owner = self;
         seed(owner, remote, 1);
         const auto value = signed_message(remote, *source_identity.private_key, 5);
         BOOST_REQUIRE(value.from && *value.from == remote && remote != owner.local_peer());
         BOOST_TEST(value.seqno.size() == 8U);
         BOOST_REQUIRE(pubsub::codec::verify_message(value));
         const auto id = pubsub::codec::message_id(value);
         auto armed = std::make_shared<std::atomic_bool>(false);
         auto pending_at_copy = std::make_shared<std::atomic_size_t>(999);
         auto request = std::optional<node::impl::pubsub_state::request>{};
         const auto session = self->session_for(remote);
         BOOST_REQUIRE(session && session->info.remote_peer == remote);
         const auto peer_generation = self->pubsub_value.peers.at(remote).generation;
         auto cleanup_started = false;
         const auto cleanup = [&] {
            cleanup_started = true;
            armed->store(false);
            {
               const auto lock = std::scoped_lock{self->mutex};
               self->pubsub_value.handlers.clear();
            }
            forge::asio::blocking::run(runtime, owner.async_stop());
         };
         try {
            auto staging_failed = false;
            auto staging_calls = std::size_t{};
            {
               const auto lock = std::scoped_lock{self->mutex};
               self->pubsub_value.handlers.emplace(value.subject.value, throwing_handler_copy{armed,
                  [router = self->pubsub_value.router, pending_at_copy] {
                     // Observe inside native handler copy without an impl -> handler -> impl ownership cycle.
                     pending_at_copy->store(router->pending());
                  }});
               if (fail_stage) {
                  auto guard = forge::tests::p2p::pubsub_claim_allocation{1};
                  try { request = self->stage_pubsub_request_locked(remote, {id}); }
                  catch (const std::bad_alloc&) { staging_failed = true; }
                  staging_calls = guard.calls();
               } else {
                  request = self->stage_pubsub_request_locked(remote, {id});
               }
            }
            if (fail_stage) {
               BOOST_REQUIRE(staging_failed);
               BOOST_TEST(staging_calls == 1U);
               BOOST_TEST(!request);
            } else {
               BOOST_REQUIRE(request);
               self->finish_pubsub_request(*request, true);
               armed->store(true);
               BOOST_CHECK_THROW(forge::asio::blocking::run(runtime,
                  self->handle_pubsub(session, input(value), builtins::meshsub_v11)), std::bad_alloc);
               armed->store(false);
               BOOST_TEST(pending_at_copy->load() == 0U);
               self->finish_pubsub_request(*request, true);
            }
            {
               const auto lock = std::scoped_lock{self->mutex};
               BOOST_TEST(self->pubsub_value.router->pending() == 0U);
               BOOST_TEST(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
               BOOST_TEST(self->pubsub_value.active_validations == 0U);
               BOOST_TEST(self->pubsub_value.cache.empty());
               BOOST_TEST(self->sessions.contains(1)); // Disconnect cannot mask an unfulfilled promise.
               BOOST_CHECK(self->sessions.at(1) == session && !session->closed);
               BOOST_TEST(self->pubsub_value.peers.at(remote).generation == peer_generation);
            }
            cleanup();
         } catch (...) {
            if (!cleanup_started) { cleanup(); }
            throw;
         }
      }
      BOOST_TEST(weak_owner.expired());
   }
}

void node_session_fixture::claim_allocation_rollback() {
   const auto config = options("pubsub-claim-allocation");
   auto bound = std::size_t{};
   auto exercised = std::size_t{};
   for (auto ordinal = std::size_t{}; ordinal == 0 || ordinal <= bound; ++ordinal) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto iteration_config = config;
      // Stopping an owner closes its persistence; fault-injection iterations must not share it.
      iteration_config.peer_state.persistence = peer_store::make_memory_persistence();
      auto owner = node{runtime, std::move(iteration_config)};
      const auto self = owner.impl_;
      const auto remote = peer(10);
      seed(owner, remote, 1);
      const auto value = signed_message(owner.local_peer(), *self->identity.private_key, 6);
      BOOST_REQUIRE(pubsub::codec::verify_message(value));
      const auto id = pubsub::codec::message_id(value);
      const auto key = bytes_key(id);
      auto request = std::optional<node::impl::pubsub_state::request>{};
      {
         const auto lock = std::scoped_lock{self->mutex};
         request = self->stage_pubsub_request_locked(remote, {id});
         BOOST_REQUIRE(request);
      }
      auto injected = false;
      auto calls = std::size_t{};
      auto claim = node::impl::pubsub_state::claim{};
      {
         auto guard = forge::tests::p2p::pubsub_claim_allocation{ordinal};
         try { claim = self->claim_pubsub_message(remote, key, value, true); }
         catch (const std::bad_alloc&) { injected = true; }
         calls = guard.calls();
      }
      if (ordinal == 0) {
         bound = calls;
         BOOST_REQUIRE(bound > 0U);
         BOOST_REQUIRE(bound <= 64U);
         BOOST_REQUIRE(claim.status == node::impl::pubsub_state::claim_status::claimed);
         BOOST_REQUIRE(self->complete_pubsub_message(key, claim.generation, pubsub::validation_result::accept));
         self->finish_pubsub_validation(remote);
      } else {
         BOOST_REQUIRE_MESSAGE(injected, "native claim allocation ordinal " << ordinal << " was not exercised");
         {
            const auto lock = std::scoped_lock{self->mutex};
            BOOST_TEST(self->pubsub_value.router->pending() == 0U);
            BOOST_TEST(self->pubsub_value.cache.empty());
            BOOST_TEST(self->pubsub_value.validations.empty());
            BOOST_TEST(self->pubsub_value.history.empty());
            BOOST_TEST(self->pubsub_value.active_validations == 0U);
            BOOST_TEST(self->pubsub_value.active_validations_by_peer.empty());
         }
         BOOST_TEST(owner.pubsub_scores().pending_validations == 0U);
         const auto retry = self->claim_pubsub_message(remote, key, value, true);
         BOOST_REQUIRE(retry.status == node::impl::pubsub_state::claim_status::claimed);
         BOOST_REQUIRE(self->complete_pubsub_message(key, retry.generation, pubsub::validation_result::accept));
         self->finish_pubsub_validation(remote);
      }
      self->finish_pubsub_request(*request, true);
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_TEST(self->pubsub_value.router->pending() == 0U);
         BOOST_TEST(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
      }
      BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 0U);
      forge::asio::blocking::run(runtime, owner.async_stop());
      if (ordinal != 0) { ++exercised; }
   }
   BOOST_TEST(exercised == bound);
}

void node_session_fixture::invalid_signature() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto owner = node{runtime, options("pubsub-invalid-signature-boundary")};
   const auto self = owner.impl_;
   const auto remote = peer(11);
   seed(owner, remote, 1);
   auto value = signed_message(owner.local_peer(), *self->identity.private_key, 7);
   const auto id = pubsub::codec::message_id(value);
   auto request = std::optional<node::impl::pubsub_state::request>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      request = self->stage_pubsub_request_locked(remote, {id});
      BOOST_REQUIRE(request);
   }
   self->finish_pubsub_request(*request, true);
   BOOST_REQUIRE(!value.signature.empty());
   value.signature.front() ^= 1;
   BOOST_TEST(!pubsub::codec::verify_message(value));
   forge::asio::blocking::run(runtime, self->handle_pubsub(self->session_for(remote), input(value), builtins::meshsub_v11));
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_TEST(self->pubsub_value.router->pending() == 1U);
      BOOST_TEST(self->pubsub_value.cache.empty());
      const auto expired = self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h);
      BOOST_REQUIRE_EQUAL(expired.size(), 1U);
      BOOST_TEST(expired.at(remote) == 1U);
      BOOST_TEST(self->pubsub_value.router->expire(std::chrono::steady_clock::now() + 1h).empty());
   }
   BOOST_TEST(owner.pubsub_snapshot().invalid_messages == 1U);
   forge::asio::blocking::run(runtime, owner.async_stop());
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_SUITE(pubsub_terminal)
BOOST_AUTO_TEST_CASE(p7_preclaim_fulfillment_late_activation_and_cache_capacity_recovery) {
   forge::net::p2p::node_session_fixture::preclaim_and_pressure();
}
BOOST_AUTO_TEST_CASE(p7_validation_queue_and_score_admission_refusals_do_not_resurrect) {
   forge::net::p2p::node_session_fixture::refusals();
}
BOOST_AUTO_TEST_CASE(p7_verified_arrival_fulfills_before_throwing_handler_copy) {
   forge::net::p2p::node_session_fixture::handler_copy_failure();
}
BOOST_AUTO_TEST_CASE(p7_claim_allocation_rollback_never_restores_fulfilled_request) {
   forge::net::p2p::node_session_fixture::claim_allocation_rollback();
}
BOOST_AUTO_TEST_CASE(p7_invalid_signature_preserves_unfulfilled_promise) {
   forge::net::p2p::node_session_fixture::invalid_signature();
}
BOOST_AUTO_TEST_SUITE_END()
