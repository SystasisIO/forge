module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.exceptions;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.multiformats.multihash;
import forge.net.p2p.dht;
import forge.net.p2p.protocol;
import forge.net.p2p.identity;
import forge.raw.raw;
import forge.swarm.core.identity;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.net.p2p.provider_registration;
import forge.net.p2p.ipns;
import forge.chrono.timestamp;
import forge.api.core.handle;
import forge.swarm.runtime.capabilities;
#include "../../plugins/swarm/node/details/capability_context.hxx"
#include "../../plugins/swarm/node/details/capability_call.hxx"
#include "../../plugins/swarm/node/details/provider_key.hxx"
#include "../../plugins/swarm/node/details/provider_registration.hxx"

namespace {
namespace sn = forge::plugins::swarm::node;
using namespace std::chrono_literals;

boost::asio::awaitable<int> launched(unsigned& count, bool fail = false) {
   ++count;
   if (fail) throw std::logic_error{"native primary"};
   co_return 7;
}
boost::asio::awaitable<int> terminal_before_stop(std::stop_source stop, bool fail) {
   const auto executor = co_await boost::asio::this_coro::executor;
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   boost::asio::post(executor, [stop]() mutable { stop.request_stop(); });
   if (fail) throw std::logic_error{"first native failure"};
   co_return 7;
}
boost::asio::awaitable<int> interrupted_timer(bool& interrupted) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_after(1h);
   try { co_await timer.async_wait(boost::asio::use_awaitable); }
   catch (const boost::system::system_error& error) {
      interrupted = error.code() == boost::asio::error::operation_aborted;
      throw;
   }
   co_return 0;
}
struct late_state {
   forge::asio::notification changed;
   bool entered = false;
   bool released = false;
   bool acquired = false;
   bool cleaned = false;
   unsigned native_stops = 0;
   unsigned native_withdrawals = 0;
};
boost::asio::awaitable<int> late_native(std::shared_ptr<late_state> state) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   state->entered = true;
   state->changed.notify();
   while (true) {
      const auto epoch = state->changed.epoch();
      if (state->released) break;
      (void)co_await state->changed.async_wait(epoch);
   }
   state->acquired = true;
   co_return 7;
}
boost::asio::awaitable<void> late_cleanup(std::shared_ptr<late_state> state) {
   BOOST_CHECK(state->acquired);
   state->cleaned = true;
   state->changed.notify();
   co_return;
}
boost::asio::awaitable<void> check_late_completion() {
   auto state = std::make_shared<late_state>();
   auto stop = std::stop_source{};
   auto done = false;
   auto failure = std::exception_ptr{};
   const auto executor = co_await boost::asio::this_coro::executor;
   boost::asio::co_spawn(executor, sn::capability_call<int>::run(late_native(state), stop.get_token(), {},
      std::chrono::steady_clock::now() + 5s, [state] { return late_cleanup(state); }),
      [&](std::exception_ptr error, int) { failure = error; done = true; state->changed.notify(); });
   while (true) {
      const auto epoch = state->changed.epoch();
      if (state->entered) break;
      (void)co_await state->changed.async_wait(epoch);
   }
   auto foreign_stop = std::thread{[&] { stop.request_stop(); }};
   foreign_stop.join();
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_CHECK(!done);
   BOOST_CHECK(!state->cleaned);
   state->released = true;
   state->changed.notify();
   while (true) {
      const auto epoch = state->changed.epoch();
      if (done) break;
      (void)co_await state->changed.async_wait(epoch);
   }
   BOOST_CHECK(state->cleaned);
   BOOST_REQUIRE(failure);
   BOOST_CHECK_THROW(std::rethrow_exception(failure), sn::exceptions::canceled);
}

class late_dht final : public forge::plugins::net::p2p::node::dht_api {
 public:
   explicit late_dht(std::shared_ptr<late_state> state) : state{std::move(state)} {}
   std::shared_ptr<late_state> state;
   boost::asio::awaitable<forge::net::p2p::provider_registration> provide(forge::net::p2p::protocol_id profile,
      forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options) override {
      (void)co_await late_native(state);
      co_return forge::net::p2p::detail::provider_registration_access::make(std::move(profile), std::move(key),
         [] { return true; }, [state = state] { ++state->native_stops; },
         [state = state]() -> boost::asio::awaitable<void> { ++state->native_withdrawals; co_return; });
   }
   boost::asio::awaitable<forge::net::p2p::dht::query_result> find_peer(forge::net::p2p::protocol_id,
      forge::net::p2p::peer_id, forge::net::p2p::dht::query_options) override {
      throw std::logic_error{"unused"}; co_return forge::net::p2p::dht::query_result{};
   }
   boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>> find_providers(forge::net::p2p::protocol_id,
      forge::net::p2p::dht::key, forge::net::p2p::dht::query_options) override {
      throw std::logic_error{"unused"}; co_return std::vector<forge::net::p2p::dht::peer>{};
   }
   boost::asio::awaitable<forge::net::p2p::dht::value_put_result> put_value(forge::net::p2p::protocol_id,
      forge::net::p2p::dht::record, forge::net::p2p::dht::query_options) override {
      throw std::logic_error{"unused"}; co_return forge::net::p2p::dht::value_put_result{};
   }
   boost::asio::awaitable<forge::net::p2p::dht::value_get_result> get_value(forge::net::p2p::protocol_id,
      forge::net::p2p::dht::key, forge::net::p2p::dht::query_options) override {
      throw std::logic_error{"unused"}; co_return forge::net::p2p::dht::value_get_result{};
   }
   forge::net::p2p::ipns::record create_ipns_record(std::span<const std::uint8_t>, std::uint64_t,
      forge::chrono::timestamp, std::chrono::nanoseconds, forge::net::p2p::ipns::create_options) const override {
      throw std::logic_error{"unused"};
   }
};

boost::asio::awaitable<void> check_late_provider() {
   auto state = std::make_shared<late_state>();
   auto stop = std::stop_source{};
   auto context = std::make_shared<const sn::capability_context>(sn::capability_context{
      .dht = std::make_shared<late_dht>(state), .profile = {"/test/providers/1"}});
   auto failure = std::exception_ptr{};
   auto done = false;
   const auto executor = co_await boost::asio::this_coro::executor;
   boost::asio::co_spawn(executor, sn::provider_registration::open(context, {}, stop.get_token(),
      std::chrono::steady_clock::now() + 5s), [&](std::exception_ptr error, std::unique_ptr<forge::swarm::runtime::capabilities::registration>) {
         failure = error; done = true; state->changed.notify();
      });
   while (true) {
      const auto epoch = state->changed.epoch();
      if (state->entered) break;
      (void)co_await state->changed.async_wait(epoch);
   }
   stop.request_stop();
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_CHECK(!done);
   state->released = true; state->changed.notify();
   while (true) {
      const auto epoch = state->changed.epoch();
      if (done) break;
      (void)co_await state->changed.async_wait(epoch);
   }
   BOOST_REQUIRE(failure);
   BOOST_CHECK_THROW(std::rethrow_exception(failure), sn::exceptions::canceled);
   BOOST_CHECK_EQUAL(state->native_stops, 1U);
   BOOST_CHECK_EQUAL(state->native_withdrawals, 1U);
}

struct connection_setup_state {
   forge::asio::notification changed;
   std::chrono::steady_clock::time_point deadline;
   bool setup_entered = false;
   bool release_setup = false;
   bool native_cancellation_received = false;
   bool setup_finished = false;
   bool active = false;
   bool close_entered = false;
   bool release_close = false;
   bool closed = false;
   bool destroyed_active = false;
   bool caller_done = false;
   bool unexpected_success = false;
   bool history_preserved = false;
   bool policy_preserved = false;
   bool filters_preserved = false;
   bool next_timer_canceled = false;
   unsigned opens = 0;
   unsigned closes = 0;
   unsigned destructions = 0;
   unsigned parent_incoming = 0;
   unsigned parent_outgoing = 0;
   std::exception_ptr failure;
   std::exception_ptr caller_failure;
};

template <typename Predicate>
boost::asio::awaitable<void> wait_connection_phase(std::shared_ptr<connection_setup_state> state,
   Predicate predicate, const char* phase) {
   while (true) {
      const auto epoch = state->changed.epoch();
      if (predicate()) co_return;
      if (std::chrono::steady_clock::now() >= state->deadline)
         throw std::runtime_error{std::string{"late connection fixture timed out: "} + phase};
      (void)co_await state->changed.async_wait_until(epoch, state->deadline);
   }
}

// Resource double, not a transport/authentication proof. The facade is allocated
// before launch, then acquires its resource late and has a separately gated drain.
class late_owned_connection final : public forge::swarm::runtime::capabilities::connection {
 public:
   explicit late_owned_connection(std::shared_ptr<connection_setup_state> state) : _state{std::move(state)} {}
   ~late_owned_connection() override {
      ++_state->destructions;
      _state->destroyed_active = _state->active;
      _state->changed.notify();
   }
   forge::api::core::handle<forge::swarm::api::peer> remote() const override { return {}; }
   forge::api::auth::authenticated_caller remote_identity() const override { return {}; }
   void cancel() noexcept override {}
   boost::asio::awaitable<void> async_close() override { return close_owned(_state); }
 private:
   static boost::asio::awaitable<void> close_owned(std::shared_ptr<connection_setup_state> state) {
      state->close_entered = true;
      state->changed.notify();
      auto failure = std::exception_ptr{};
      try {
         co_await wait_connection_phase(state, [state] { return state->release_close; }, "connection close release");
      } catch (...) { failure = std::current_exception(); }
      state->active = false;
      state->closed = true;
      ++state->closes;
      state->changed.notify();
      if (failure) std::rethrow_exception(failure);
   }
   std::shared_ptr<connection_setup_state> _state;
};

boost::asio::awaitable<int> paused_connection_setup(std::shared_ptr<connection_setup_state> state) {
   // Observe actual cancellation delivery but deliberately complete late, as an
   // external operation can do. Only this isolated native child changes filters.
   co_await boost::asio::this_coro::reset_cancellation_state(
      [state](boost::asio::cancellation_type_t type) {
         if (type != boost::asio::cancellation_type::none) {
            state->native_cancellation_received = true;
            state->changed.notify();
         }
         return boost::asio::cancellation_type::none;
      }, boost::asio::disable_cancellation());
   state->setup_entered = true;
   state->changed.notify();
   co_await wait_connection_phase(state, [state] { return state->release_setup; }, "native setup release");
   state->active = true;
   ++state->opens;
   state->setup_finished = true;
   state->changed.notify();
   co_return 0;
}

boost::asio::awaitable<void> connection_setup_caller(std::shared_ptr<connection_setup_state> state,
   std::shared_ptr<boost::asio::cancellation_signal> parent, std::stop_token lifetime, bool cancel_parent) {
   const auto accepted = boost::asio::cancellation_type::terminal | boost::asio::cancellation_type::partial;
   co_await boost::asio::this_coro::reset_cancellation_state(
      [state](boost::asio::cancellation_type_t type) {
         ++state->parent_incoming; return type & accepted;
      },
      [state](boost::asio::cancellation_type_t type) {
         ++state->parent_outgoing; return type & accepted;
      });
   const auto throw_policy = !cancel_parent;
   co_await boost::asio::this_coro::throw_if_cancelled(throw_policy);
   auto owner = std::make_unique<late_owned_connection>(state);
   try {
      (void)co_await sn::capability_call<int>::run(paused_connection_setup(state), {}, lifetime, state->deadline,
         [connection = owner.get()] { return connection->async_close(); });
      state->unexpected_success = true;
   } catch (...) { state->failure = std::current_exception(); }
   owner.reset();

   const auto expected_history = cancel_parent ? boost::asio::cancellation_type::terminal : boost::asio::cancellation_type::none;
   state->history_preserved = (co_await boost::asio::this_coro::cancellation_state).cancelled() == expected_history;
   state->policy_preserved = (co_await boost::asio::this_coro::throw_if_cancelled()) == throw_policy;
   const auto expected_filters = cancel_parent ? 1U : 0U;
   state->filters_preserved = state->parent_incoming == expected_filters && state->parent_outgoing == expected_filters;

   // A second, different cancellation kind must still traverse the original
   // caller filters and reach its next timer; cleanup must not consume history.
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor, 500ms};
   boost::asio::post(executor, [parent] { parent->emit(boost::asio::cancellation_type::partial); });
   auto error = boost::system::error_code{};
   co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   state->next_timer_canceled = error == boost::asio::error::operation_aborted;
   state->filters_preserved = state->filters_preserved && state->parent_incoming == expected_filters + 1U &&
      state->parent_outgoing == expected_filters + 1U;
   state->policy_preserved = state->policy_preserved && (co_await boost::asio::this_coro::throw_if_cancelled()) == throw_policy;
   state->history_preserved = state->history_preserved &&
      (co_await boost::asio::this_coro::cancellation_state).cancelled() == boost::asio::cancellation_type::partial;
}

bool is_call_cancellation(const std::exception_ptr& failure) {
   try { std::rethrow_exception(failure); }
   catch (const sn::exceptions::canceled&) { return true; }
   catch (const boost::system::system_error& error) { return error.code() == boost::asio::error::operation_aborted; }
   catch (...) { return false; }
}

boost::asio::awaitable<void> check_owned_connection_cancellation(bool cancel_parent) {
   auto state = std::make_shared<connection_setup_state>();
   state->deadline = std::chrono::steady_clock::now() + 5s;
   auto parent = std::make_shared<boost::asio::cancellation_signal>();
   auto lifetime = std::stop_source{};
   // Same lifetime token position used by the official capability_context, not
   // the per-call stop token exercised by the older foreign-stop regression.
   const auto context = sn::capability_context{.lifetime = lifetime.get_token()};
   const auto executor = co_await boost::asio::this_coro::executor;
   boost::asio::co_spawn(executor, connection_setup_caller(state, parent, context.lifetime, cancel_parent),
      boost::asio::bind_cancellation_slot(parent->slot(), [state](std::exception_ptr failure) {
         state->caller_failure = failure;
         state->caller_done = true;
         state->changed.notify();
      }));
   auto observation_failure = std::exception_ptr{};
   try {
      co_await wait_connection_phase(state, [state] { return state->setup_entered || state->caller_done; }, "setup entry");
      BOOST_CHECK(state->setup_entered);
      if (cancel_parent) parent->emit(boost::asio::cancellation_type::terminal);
      else lifetime.request_stop();
      co_await wait_connection_phase(state, [state] {
         return state->native_cancellation_received || state->caller_done;
      }, "native cancellation delivery");
      BOOST_CHECK(state->native_cancellation_received);
      BOOST_CHECK(!state->caller_done);
      BOOST_CHECK(!state->active);
      BOOST_CHECK(!state->close_entered);
      BOOST_CHECK_EQUAL(state->destructions, 0U);
      state->release_setup = true;
      state->changed.notify();
      co_await wait_connection_phase(state, [state] { return state->close_entered || state->caller_done; }, "late connection cleanup");
      BOOST_CHECK(state->setup_finished);
      BOOST_CHECK(state->close_entered);
      BOOST_CHECK(state->active);
      BOOST_CHECK(!state->closed);
      BOOST_CHECK(!state->caller_done);
      BOOST_CHECK_EQUAL(state->destructions, 0U);
   } catch (...) { observation_failure = std::current_exception(); }
   // Release both gates even after an observation failure, then unconditionally
   // join the caller. A failing assertion must not detach an accepted child.
   state->release_setup = true;
   state->release_close = true;
   state->changed.notify();
   try {
      co_await wait_connection_phase(state, [state] { return state->caller_done; }, "owned caller join");
   } catch (...) {
      std::fputs("late connection cancellation failed to join its owned caller\n", stderr);
      std::abort();
   }
   if (observation_failure) std::rethrow_exception(observation_failure);
   BOOST_REQUIRE(!state->caller_failure);
   BOOST_REQUIRE(state->failure);
   BOOST_CHECK(is_call_cancellation(state->failure));
   BOOST_CHECK(!state->unexpected_success);
   BOOST_CHECK_EQUAL(state->opens, 1U);
   BOOST_CHECK_EQUAL(state->closes, 1U);
   BOOST_CHECK_EQUAL(state->destructions, 1U);
   BOOST_CHECK(!state->active);
   BOOST_CHECK(state->closed);
   BOOST_CHECK(!state->destroyed_active);
   BOOST_CHECK(state->history_preserved);
   BOOST_CHECK(state->policy_preserved);
   BOOST_CHECK(state->filters_preserved);
   BOOST_CHECK(state->next_timer_canceled);
   BOOST_CHECK_EQUAL(lifetime.stop_requested(), !cancel_parent);
}
} // namespace

BOOST_AUTO_TEST_CASE(swarm_provider_key_is_raw_domain_realm_content_sha256_multihash) {
   const auto id = forge::swarm::core::swarm_id{};
   const auto key = sn::provider_key(id, "realm-a");
   const auto decoded = forge::multiformats::multihash::decode(key.bytes);
   BOOST_CHECK_EQUAL(decoded.code, 0x12U);
   BOOST_CHECK_EQUAL(decoded.digest_hex(), "0dbd9aa1dfb6d3c42ee156e3d88c8a7d8fe20e4b8cb5cc41f5444507044a8d57");
   BOOST_CHECK(key.bytes != sn::provider_key(id, "realm-b").bytes);
   BOOST_CHECK(key.bytes != sn::provider_key({forge::swarm::core::sha256::hash(std::string{"other"})}, "realm-a").bytes);
   const auto other_domain = forge::raw::pack(std::tuple{std::string{"forge.swarm.providers/v2"}, std::string{"realm-a"}, id});
   BOOST_CHECK(key.bytes != forge::multiformats::multihash::sha2_256(other_domain).encode());
}

BOOST_AUTO_TEST_CASE(swarm_capability_pre_stop_and_pre_expiry_never_launch_native_call) {
   auto runtime = forge::asio::runtime{};
   auto count = 0U;
   auto stop = std::stop_source{};
   stop.request_stop();
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, sn::capability_call<int>::run(
      launched(count), stop.get_token(), {}, std::chrono::steady_clock::now() + 1s)), sn::exceptions::canceled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, sn::capability_call<int>::run(
      launched(count), {}, {}, std::chrono::steady_clock::now())), sn::exceptions::timeout);
   BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(swarm_capability_deadline_reaches_native_asio_operation) {
   auto runtime = forge::asio::runtime{};
   auto interrupted = false;
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, sn::capability_call<int>::run(
      interrupted_timer(interrupted), {}, {}, std::chrono::steady_clock::now() + 20ms)), sn::exceptions::timeout);
   BOOST_CHECK(interrupted);
}

BOOST_AUTO_TEST_CASE(swarm_capability_first_native_success_and_failure_survive_later_stop) {
   auto runtime = forge::asio::runtime{};
   auto successful = std::stop_source{};
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(runtime, sn::capability_call<int>::run(
      terminal_before_stop(successful, false), successful.get_token(), {}, std::chrono::steady_clock::now() + 5s)), 7);
   auto failed = std::stop_source{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, sn::capability_call<int>::run(
      terminal_before_stop(failed, true), failed.get_token(), {}, std::chrono::steady_clock::now() + 5s)), std::logic_error);
}

BOOST_AUTO_TEST_CASE(swarm_capability_foreign_stop_joins_and_cleans_late_native_resource) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, check_late_completion());
}

BOOST_AUTO_TEST_CASE(swarm_provider_adapter_joins_and_withdraws_registration_accepted_after_stop) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, check_late_provider());
}

BOOST_AUTO_TEST_CASE(swarm_capability_parent_asio_cancellation_joins_late_connection_and_preserves_caller_history) {
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   forge::asio::blocking::run(runtime, check_owned_connection_cancellation(true));
}

BOOST_AUTO_TEST_CASE(swarm_capability_plugin_lifetime_stop_joins_and_closes_late_connection) {
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   forge::asio::blocking::run(runtime, check_owned_connection_cancellation(false));
}

BOOST_AUTO_TEST_CASE(swarm_capability_fast_failure_preserves_primary_and_cleanup_failure) {
   auto runtime = forge::asio::runtime{};
   auto count = 0U;
   auto cleaned = false;
   const auto cleanup = [&]() -> boost::asio::awaitable<void> { cleaned = true; throw std::runtime_error{"cleanup"}; co_return; };
   try {
      forge::asio::blocking::run(runtime, sn::capability_call<int>::run(
         launched(count, true), {}, {}, std::chrono::steady_clock::now() + 1s, cleanup));
      BOOST_FAIL("expected native failure");
   } catch (const sn::exceptions::operation_failed& error) {
      BOOST_REQUIRE(error.causes().primary);
      BOOST_REQUIRE(error.causes().cleanup);
      BOOST_CHECK_THROW(std::rethrow_exception(error.causes().primary), std::logic_error);
      BOOST_CHECK_THROW(std::rethrow_exception(error.causes().cleanup), std::runtime_error);
   }
   BOOST_CHECK_EQUAL(count, 1U);
   BOOST_CHECK(cleaned);
}
