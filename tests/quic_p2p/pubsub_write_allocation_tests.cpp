module;

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <stop_token>
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
#include <boost/asio/detached.hpp>
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
#include <boost/system/system_error.hpp>
#include <boost/compat/move_only_function.hpp>
#include "libp2p_identity_fixture.hxx"
#include "pubsub_write_allocation_fixture.hxx"

module forge.net.p2p.node;

import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.connection_gater;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
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

namespace forge::net::p2p {

// This friendship is confined to the isolated executable, which does not link node_session_tests.cpp.
struct node_session_fixture {
   static std::uint64_t outbound_bytes(const void* owner) {
      const auto self = static_cast<const node*>(owner)->impl_;
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.outbound_budget.total();
   }

   static void trace_guard_preserves_preheld_failure(bool enabled) {
      using fixture = forge::tests::p2p::pubsub_write_allocation_fixture;
      using namespace std::chrono_literals;
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      const auto identity = forge::tests::p2p::make_identity_fixture("trace-guard-unit");
      auto options = node::options{.certificate_pem = identity.certificate_pem,
          .private_key_pem = identity.private_key_pem};
      options.peer_state.persistence = peer_store::make_memory_persistence();
      options.dht_profiles.clear();
      auto callbacks = std::size_t{};
      if (enabled) { options.limits.pubsub.tracer = [&](const auto&) { ++callbacks; }; }
      auto owner = node{runtime, std::move(options)};
      const auto stop_owner = [&](void*) noexcept {
         try {
            auto stop = boost::asio::co_spawn(runtime.context(), owner.async_stop(), boost::asio::use_future);
            if (stop.wait_for(5s) != std::future_status::ready) { std::_Exit(86); }
            stop.get();
         } catch (...) { std::_Exit(86); }
      };
      auto cleanup = std::unique_ptr<void, decltype(stop_owner)>{&owner, stop_owner};
      auto original = std::exception_ptr{};
      try { throw std::runtime_error{"preheld original operation failure"}; }
      catch (...) { original = std::current_exception(); }
      auto builder_called = false;
      auto rejected = fixture::allocator::none;
      {
         const auto injection = fixture::allocation_scope{};
         owner.impl_->trace_pubsub([&] {
            builder_called = true;
            return pubsub::trace_event{.kind = pubsub::trace_kind::rpc_write, .peer = owner.local_peer(),
                .protocol = builtins::meshsub_v11};
         });
         rejected = injection.rejected_by();
      }
      BOOST_TEST(builder_called == enabled);
      BOOST_CHECK(rejected == (enabled ? fixture::allocator::scalar_new : fixture::allocator::none));
      BOOST_TEST(callbacks == 0U);
      BOOST_TEST(owner.pubsub_snapshot().trace_failures == (enabled ? 1U : 0U));
      // Unit guard proof only: this is not an OOM injected into a failing native cached write.
      auto original_rethrown = false;
      try { std::rethrow_exception(original); }
      catch (const std::runtime_error&) {
         original_rethrown = true;
         BOOST_CHECK(std::current_exception() == original);
      }
      BOOST_TEST(original_rethrown);
      cleanup.reset();
   }
};

} // namespace forge::net::p2p

namespace {

using fixture = forge::tests::p2p::pubsub_write_allocation_fixture;

void check_factory(fixture::factory selected) {
   const auto result = fixture::observe(selected);
   BOOST_REQUIRE(result.ordinary_thread);
   BOOST_REQUIRE(result.rejected_by != fixture::allocator::none);
   BOOST_REQUIRE(result.construction_error);
   auto original_bad_alloc = false;
   try { std::rethrow_exception(result.construction_error); }
   catch (const std::bad_alloc&) {
      original_bad_alloc = true;
      // Asio may use Boost's wrapexcept<bad_alloc>; verify the original exception object, not a class alias.
      BOOST_CHECK(std::current_exception() == result.construction_error);
   }
   BOOST_REQUIRE(original_bad_alloc);
   BOOST_TEST(result.factories_after_failure == 1U); // Both public facades reached the real lower factory.
   BOOST_TEST(result.bodies_after_failure == 0U);
   BOOST_TEST(result.factories_before_recovery_execution == 2U);
   BOOST_TEST(result.bodies_before_recovery_execution == 0U);
   BOOST_TEST(result.factories_after_recovery == 2U);
   BOOST_TEST(result.bodies_after_recovery == 1U);
   BOOST_TEST(result.payload_preserved);
   BOOST_TEST(result.selected_factory_preserved);
}

} // namespace

BOOST_AUTO_TEST_SUITE(pubsub_write_allocation)

BOOST_AUTO_TEST_CASE(chunk_factory_real_frame_allocation_failure_precedes_body_and_recovers) {
   check_factory(fixture::factory::chunk);
}

BOOST_AUTO_TEST_CASE(frame_chunk_factory_real_frame_allocation_failure_precedes_body_and_recovers) {
   check_factory(fixture::factory::frame_chunk);
}

BOOST_AUTO_TEST_CASE(native_trace_peer_copy_allocation_failure_preserves_completed_write_and_releases_bytes) {
   const auto result = fixture::observe_native_trace(&forge::net::p2p::node_session_fixture::outbound_bytes);
   if (result.native_error) {
      try { std::rethrow_exception(result.native_error); }
      catch (const std::exception& error) { BOOST_FAIL("native trace allocation regression: " << error.what()); }
   }
   BOOST_REQUIRE(result.authenticated);
   BOOST_REQUIRE(result.heap_peer_id);
   BOOST_REQUIRE(result.snapshot_observed);
   BOOST_REQUIRE(result.rejected_by == fixture::allocator::scalar_new);
   BOOST_TEST(result.subscription_succeeded);
   BOOST_TEST(result.write_callbacks == 1U);
   BOOST_TEST(result.native_subscription_frames == 2U);
   BOOST_TEST(result.trace_failures == 1U);
   BOOST_TEST(result.outbound_bytes_after_failure == 0U);
   BOOST_TEST(result.outbound_bytes_after_recovery == 0U);
   BOOST_TEST(result.stream_memory_after_failure == 0U);
   BOOST_TEST(result.stream_memory_after_recovery == 0U);
   BOOST_TEST(result.same_native_stream);
   BOOST_TEST(result.recovery_delivered);
   BOOST_TEST(result.messages_published == 1U);
   BOOST_TEST(result.messages_delivered == 1U);
   BOOST_TEST(result.native_failures_unchanged);
}

BOOST_AUTO_TEST_CASE(trace_guard_peer_copy_failure_preserves_preheld_exception_identity) {
   forge::net::p2p::node_session_fixture::trace_guard_preserves_preheld_failure(true);
}

BOOST_AUTO_TEST_CASE(disabled_trace_skips_builder_and_allocation) {
   forge::net::p2p::node_session_fixture::trace_guard_preserves_preheld_failure(false);
}

BOOST_AUTO_TEST_CASE(partial_rpc_budget_rejects_before_materializing_large_body) {
   namespace ps = forge::net::p2p::pubsub;
   constexpr auto body_size = std::size_t{256 * 1024};
   auto rpc = ps::rpc{};
   rpc.partial.emplace();
   rpc.partial->data.emplace(body_size, 0x5a);
   auto opts = ps::options{};
   for (int prefix = 0; prefix != 4; ++prefix) {
      rpc.subscriptions.clear();
      rpc.control_value.reset();
      if ((prefix & 1) != 0) { rpc.subscriptions.push_back({.subject = {.value = "t"}}); }
      if ((prefix & 2) != 0) {
         rpc.control_value.emplace();
         rpc.control_value->grafts.push_back({.subject = {.value = "t"}});
      }
      // The data field and enclosing Partial each use a key and a three-byte length.
      // A nonempty prefix consumes space even when the Partial alone would fit exactly.
      opts.limits.max_rpc_size = prefix == 0 ? 64 : body_size + 8;
      auto invalid = false;
      auto unexpected = std::exception_ptr{};
      auto rejected = fixture::allocator::none;
      {
         // Allow small protobuf prefixes and exception diagnostics, but fail any body-sized allocation.
         const auto injection = fixture::allocation_scope{body_size / 2};
         try { static_cast<void>(ps::codec::encode(rpc, opts)); }
         catch (const forge::net::p2p::exceptions::invalid_options&) { invalid = true; }
         catch (...) { unexpected = std::current_exception(); }
         rejected = injection.rejected_by();
      }
      if (unexpected) { std::rethrow_exception(unexpected); }
      BOOST_CHECK(invalid);
      BOOST_CHECK(rejected == fixture::allocator::none);
   }

   // Positive injection control: an admitted Partial really reaches the same allocator.
   rpc.subscriptions.clear();
   rpc.control_value.reset();
   opts.limits.max_rpc_size = body_size + 8;
   auto allocation_failed = false;
   auto rejected = fixture::allocator::none;
   {
      const auto injection = fixture::allocation_scope{body_size / 2};
      try { static_cast<void>(ps::codec::encode(rpc, opts)); }
      catch (const std::bad_alloc&) { allocation_failed = true; }
      rejected = injection.rejected_by();
   }
   BOOST_CHECK(allocation_failed);
   BOOST_CHECK(rejected == fixture::allocator::scalar_new);
   const auto decoded = ps::codec::decode(ps::codec::encode(rpc, opts), opts);
   BOOST_REQUIRE(decoded.partial && decoded.partial->data);
   BOOST_CHECK_EQUAL(decoded.partial->data->size(), body_size);
   BOOST_CHECK(std::ranges::equal(*decoded.partial->data, *rpc.partial->data));
}

BOOST_AUTO_TEST_SUITE_END()
