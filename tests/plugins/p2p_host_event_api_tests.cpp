#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import forge.api.core.descriptor;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.plugins.net.p2p.diagnostics.api;
import forge.plugins.net.p2p.diagnostics.events_api;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.diagnostics.plugin;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.host_event_source;
import forge.plugins.net.p2p.node.plugin;

namespace {

namespace p2p = forge::net::p2p;
namespace node_plugin = forge::plugins::net::p2p::node;
namespace diagnostics_plugin = forge::plugins::net::p2p::diagnostics;
using namespace std::chrono_literals;

template <typename T>
T joined(std::future<T>& result) {
   if (result.wait_for(5s) != std::future_status::ready) {
      // Never unwind a fixture while its native coroutine/thread can still use it.
      std::cerr << "P2P host-event test owner failed to join within five seconds\n";
      std::terminate();
   }
   return result.get();
}

struct plugin_environment {
   forge::asio::runtime runtime{};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};

   ~plugin_environment() {
      run(scheduler.shutdown());
   }

   void run(boost::asio::awaitable<void> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      joined(result);
   }
};

struct host_fixture {
   plugin_environment environment;
   node_plugin::plugin node;
   diagnostics_plugin::plugin diagnostics;

   host_fixture() {
      auto config = forge::config::core::document{};
      // No sockets or test authentication: this fixture exercises the real local host lifecycle only.
      const auto peer = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
                                          .data = std::vector<std::uint8_t>(32, 73)});
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.peer-id", peer.value);
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      environment.run(node.configure({config, "plugins.net.p2p.node"}));
      auto provider = forge::api::core::installer{environment.apis};
      environment.run(node.provide(provider));
      environment.run(diagnostics.provide(provider));
      environment.run(node.initialize(environment.context));
      environment.run(node.after_initialize());
      environment.run(diagnostics.initialize(environment.context));
   }

   ~host_fixture() {
      diagnostics.request_stop();
      node.request_stop();
      environment.run(diagnostics.shutdown());
      environment.run(node.shutdown());
   }

   void start() {
      environment.run(node.startup());
      environment.run(diagnostics.startup());
   }

   auto source() {
      return environment.apis.get<node_plugin::host_event_source>(
          {.id = {"forge.plugins.net.p2p.node.host_event_source"}, .major = 1, .min_revision = 0});
   }

   auto facade() {
      return environment.apis.get<diagnostics_plugin::events_api>(
          {.id = {"forge.plugins.net.p2p.diagnostics.events"}, .major = 1, .min_revision = 0});
   }

   auto snapshots() {
      return environment.apis.get<diagnostics_plugin::api>(
          {.id = {"forge.plugins.net.p2p.diagnostics"}, .major = 2, .min_revision = 0});
   }
};

std::optional<p2p::host_event> read_result(boost::asio::io_context& io,
                                         p2p::host_event_subscription& subscription,
                                         std::future<std::optional<p2p::host_event>>& result) {
   io.restart();
   io.run_for(250ms);
   if (result.wait_for(1s) != std::future_status::ready) {
      subscription.close();
      io.restart();
      io.run_for(1s);
      (void)joined(result);
      BOOST_FAIL("native host-event read did not complete before the bounded read deadline");
   }
   return joined(result);
}

// A blocked source callback isolates facade ownership/locking from native mailbox tests.
struct snapshot_barrier {
   std::promise<p2p::diagnostics::options> entered;
   std::promise<void> release;
   std::shared_future<void> released{release.get_future().share()};
};

class blocked_snapshot_source final : public node_plugin::diagnostics_source {
 public:
   explicit blocked_snapshot_source(std::shared_ptr<snapshot_barrier> barrier) : barrier_{std::move(barrier)} {}

   p2p::diagnostics::snapshot snapshot(p2p::diagnostics::options options) const override {
      barrier_->entered.set_value(options);
      if (barrier_->released.wait_for(5s) != std::future_status::ready) {
         throw std::runtime_error{"snapshot callback was not released"};
      }
      auto result = p2p::diagnostics::snapshot{};
      result.network.stopped = true;
      return result;
   }

 private:
   std::shared_ptr<snapshot_barrier> barrier_;
};

struct event_barrier {
   std::promise<void> entered;
   std::promise<void> release;
   std::shared_future<void> released{release.get_future().share()};

   void wait() {
      entered.set_value();
      if (released.wait_for(5s) != std::future_status::ready) {
         throw std::runtime_error{"event source callback was not released"};
      }
   }
};

class blocked_host_event_source final : public node_plugin::host_event_source {
 public:
   blocked_host_event_source(std::shared_ptr<node_plugin::host_event_source> native,
                             std::shared_ptr<event_barrier> barrier)
       : native_{std::move(native)}, barrier_{std::move(barrier)} {}

   p2p::host_event reachability_status() const override {
      barrier_->wait();
      return native_->reachability_status();
   }

   p2p::host_event_subscription host_events() const override {
      barrier_->wait();
      return native_->host_events();
   }

 private:
   std::shared_ptr<node_plugin::host_event_source> native_;
   std::shared_ptr<event_barrier> barrier_;
};

template <bool Subscribe>
void check_inflight_event_call() {
   auto fixture = host_fixture{};
   fixture.start();
   const auto native = fixture.source().shared();
   const auto snapshots = fixture.environment.apis.get<node_plugin::diagnostics_source>(
       {.id = {"forge.plugins.net.p2p.node.diagnostics_source"}, .major = 2}).shared();
   auto barrier = std::make_shared<event_barrier>();
   auto source = std::make_shared<blocked_host_event_source>(native, barrier);
   const auto weak_source = std::weak_ptr<blocked_host_event_source>{source};
   fixture.environment.apis.clear();
   fixture.environment.apis.install<node_plugin::diagnostics_source>(snapshots);
   fixture.environment.apis.install<node_plugin::host_event_source>(source);
   auto provider = forge::api::core::installer{fixture.environment.apis};
   fixture.environment.run(fixture.diagnostics.provide(provider));
   fixture.environment.run(fixture.diagnostics.initialize(fixture.environment.context));
   const auto facade = fixture.facade();
   fixture.environment.apis.clear();
   source.reset();

   using result_type = std::conditional_t<Subscribe, p2p::host_event_subscription, p2p::host_event>;
   auto task = std::packaged_task<result_type()>{[facade] {
      if constexpr (Subscribe) {
         return facade->host_events();
      } else {
         return facade->reachability_status();
      }
   }};
   auto entered = barrier->entered.get_future();
   auto result = task.get_future();
   auto worker = std::thread{std::move(task)};
   auto released = false;
   auto release = [&] {
      if (!std::exchange(released, true)) {
         barrier->release.set_value();
      }
   };
   auto release_and_join = boost::scope::scope_exit{[&] {
      release();
      if (result.valid() && result.wait_for(5s) != std::future_status::ready) {
         std::cerr << "P2P host-event callback did not join\n";
         std::terminate();
      }
      if (worker.joinable()) {
         worker.join();
      }
   }};
   joined(entered);
   fixture.diagnostics.request_stop();
   fixture.environment.run(fixture.diagnostics.shutdown());
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(!weak_source.expired());
   BOOST_CHECK_THROW((void)facade->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)facade->reachability_status(), p2p::exceptions::canceled);
   release();
   auto issued = joined(result);
   worker.join();
   release_and_join.set_active(false);
   BOOST_TEST(weak_source.expired());
   if constexpr (Subscribe) {
      BOOST_TEST(issued.active());
      auto io = boost::asio::io_context{};
      auto initial = boost::asio::co_spawn(io, issued.async_read(), boost::asio::use_future);
      const auto state = read_result(io, issued, initial);
      BOOST_REQUIRE(state.has_value());
      BOOST_CHECK(state->phase == p2p::lifecycle_phase::maintenance);
      fixture.node.request_stop();
      fixture.environment.run(fixture.node.shutdown());
      auto closed = boost::asio::co_spawn(io, issued.async_read(), boost::asio::use_future);
      BOOST_TEST(!read_result(io, issued, closed).has_value());
   } else {
      BOOST_CHECK(issued.phase == p2p::lifecycle_phase::maintenance);
   }
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_host_event_api_contracts_are_additive_local_and_reject_before_startup) {
   auto environment = plugin_environment{};
   auto node = node_plugin::plugin{};
   auto diagnostics = diagnostics_plugin::plugin{};
   auto provider = forge::api::core::installer{environment.apis};
   environment.run(node.provide(provider));
   environment.run(diagnostics.provide(provider));
   const auto source = environment.apis.get<node_plugin::host_event_source>(
       {.id = {"forge.plugins.net.p2p.node.host_event_source"}, .major = 1, .min_revision = 0});
   const auto facade = environment.apis.get<diagnostics_plugin::events_api>(
       {.id = {"forge.plugins.net.p2p.diagnostics.events"}, .major = 1, .min_revision = 0});
   for (const auto& descriptor : {node_plugin::host_event_source::describe(),
                                 diagnostics_plugin::events_api::describe()}) {
      BOOST_TEST(descriptor.version.major == 1U);
      BOOST_TEST(descriptor.version.revision == 0U);
      BOOST_TEST(descriptor.methods.empty());
      BOOST_CHECK(descriptor.supported_surfaces == forge::api::core::surface::local);
   }
   BOOST_CHECK(environment.apis.describe({.id = {"forge.plugins.net.p2p.node.diagnostics_source"}, .major = 2}) != nullptr);
   BOOST_CHECK(environment.apis.describe({.id = {"forge.plugins.net.p2p.diagnostics"}, .major = 2}) != nullptr);
   BOOST_CHECK_THROW((void)source->reachability_status(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)source->host_events(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)facade->reachability_status(), diagnostics_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)facade->host_events(), diagnostics_plugin::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_delivers_the_native_initial_snapshot) {
   auto fixture = host_fixture{};
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), node_plugin::exceptions::plugin_not_initialized);
   fixture.start();
   const auto before = fixture.source()->reachability_status();
   auto direct = fixture.source()->host_events();
   auto delegated = fixture.facade()->host_events();
   const auto after = fixture.facade()->reachability_status();
   auto io = boost::asio::io_context{};
   for (auto* subscription : {&direct, &delegated}) {
      auto result = boost::asio::co_spawn(io, subscription->async_read(), boost::asio::use_future);
      const auto initial = read_result(io, *subscription, result);
      BOOST_REQUIRE(initial.has_value());
      BOOST_TEST(initial->generation >= before.generation);
      BOOST_TEST(initial->generation <= after.generation);
      BOOST_CHECK(initial->phase == p2p::lifecycle_phase::maintenance);
      BOOST_CHECK(initial->effective == after.effective);
      BOOST_CHECK(initial->autonat_v1 == after.autonat_v1);
      BOOST_TEST(initial->autonat_v2.empty());
      BOOST_TEST(initial->confirmed_addresses.empty());
      BOOST_TEST(subscription->active());
   }
   fixture.diagnostics.request_stop();
   fixture.node.request_stop();
   // Existing snapshots stay available until shutdown resets the native owner.
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot());
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot({.max_peers = 1}));
   BOOST_CHECK_THROW((void)fixture.source()->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_diagnostics_stop_does_not_close_native_subscriptions) {
   auto fixture = host_fixture{};
   fixture.start();
   auto subscription = fixture.facade()->host_events();
   auto io = boost::asio::io_context{};
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(read_result(io, subscription, initial).has_value());
   auto pending = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(pending.wait_for(0ms) != std::future_status::ready);
   fixture.diagnostics.request_stop();
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)fixture.facade()->reachability_status(), p2p::exceptions::canceled);
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot());
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot({.max_peers = 1}));
   fixture.environment.run(fixture.diagnostics.shutdown());
   BOOST_CHECK_THROW((void)fixture.snapshots()->snapshot(), diagnostics_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), p2p::exceptions::canceled);
   BOOST_TEST(subscription.active());
   io.restart();
   io.poll();
   BOOST_CHECK(pending.wait_for(0ms) != std::future_status::ready);
   // The host still admits subscriptions, independent of diagnostics' lifecycle.
   auto second = fixture.source()->host_events();
   BOOST_TEST(second.active());
   fixture.node.request_stop();
   BOOST_CHECK_THROW((void)fixture.source()->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)fixture.source()->reachability_status(), p2p::exceptions::canceled);
   fixture.environment.run(fixture.node.shutdown());
   BOOST_TEST(!read_result(io, subscription, pending).has_value());
   BOOST_TEST(!subscription.active());
   BOOST_TEST(!second.active());
   auto late = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_TEST(!read_result(io, subscription, late).has_value());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_canceled_read_keeps_the_issued_subscription_open) {
   auto fixture = host_fixture{};
   fixture.start();
   auto subscription = fixture.facade()->host_events();
   auto io = boost::asio::io_context{};
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(read_result(io, subscription, initial).has_value());
   auto cancellation = boost::asio::cancellation_signal{};
   auto canceled = boost::asio::co_spawn(io, subscription.async_read(),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   io.restart();
   io.poll();
   BOOST_CHECK(canceled.wait_for(0ms) != std::future_status::ready);
   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW(read_result(io, subscription, canceled), std::exception);
   BOOST_TEST(subscription.active());
   auto next = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(next.wait_for(0ms) != std::future_status::ready);
   fixture.node.request_stop();
   fixture.environment.run(fixture.node.shutdown());
   BOOST_TEST(!read_result(io, subscription, next).has_value());
   BOOST_TEST(!subscription.active());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_snapshot_retains_source_outside_lifecycle_lock) {
   // Exercise both old snapshot overloads against shutdown while the callback is in flight.
   for (const auto explicit_options : {false, true}) {
      auto environment = plugin_environment{};
      auto node = node_plugin::plugin{};
      auto diagnostics = diagnostics_plugin::plugin{};
      auto provider = forge::api::core::installer{environment.apis};
      environment.run(node.provide(provider));
      const auto events = environment.apis.get<node_plugin::host_event_source>(
          {.id = {"forge.plugins.net.p2p.node.host_event_source"}, .major = 1}).shared();
      environment.apis.clear();
      environment.apis.install<node_plugin::host_event_source>(events);
      auto barrier = std::make_shared<snapshot_barrier>();
      auto source = std::make_shared<blocked_snapshot_source>(barrier);
      const auto weak_source = std::weak_ptr<blocked_snapshot_source>{source};
      environment.apis.install<node_plugin::diagnostics_source>(source);
      auto config = forge::config::core::document{};
      config.set("plugins.net.p2p.diagnostics.max-peers", 7U);
      environment.run(diagnostics.configure({config, "plugins.net.p2p.diagnostics"}));
      environment.run(diagnostics.provide(provider));
      environment.run(diagnostics.initialize(environment.context));
      const auto facade = environment.apis.get<diagnostics_plugin::api>(
          {.id = {"forge.plugins.net.p2p.diagnostics"}, .major = 2});
      environment.apis.clear();
      source.reset();

      auto entered = barrier->entered.get_future();
      auto task = std::packaged_task<p2p::diagnostics::snapshot()>{[facade, explicit_options] {
         return explicit_options ? facade->snapshot({.max_peers = 3}) : facade->snapshot();
      }};
      auto result = task.get_future();
      auto worker = std::thread{std::move(task)};
      auto released = false;
      auto release = [&] {
         if (!std::exchange(released, true)) {
            barrier->release.set_value();
         }
      };
      auto release_and_join = boost::scope::scope_exit{[&] {
         release();
         if (result.valid() && result.wait_for(5s) != std::future_status::ready) {
            std::cerr << "P2P diagnostics source callback did not join\n";
            std::terminate();
         }
         if (worker.joinable()) {
            worker.join();
         }
      }};
      const auto observed_options = joined(entered);
      BOOST_TEST(observed_options.max_peers == (explicit_options ? 3U : 7U));
      // Reconfiguration and shutdown must not wait for the source callback.
      config.set("plugins.net.p2p.diagnostics.max-peers", 9U);
      environment.run(diagnostics.configure({config, "plugins.net.p2p.diagnostics"}));
      diagnostics.request_stop();
      environment.run(diagnostics.shutdown());
      BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
      BOOST_TEST(!weak_source.expired());
      BOOST_CHECK_THROW((void)facade->snapshot(), diagnostics_plugin::exceptions::plugin_not_initialized);
      release();
      const auto captured = joined(result);
      worker.join();
      release_and_join.set_active(false);
      BOOST_TEST(captured.network.stopped);
      BOOST_TEST(weak_source.expired());
   }
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_stop_before_initialize_cannot_reopen_admission) {
   auto environment = plugin_environment{};
   auto node = node_plugin::plugin{};
   auto diagnostics = diagnostics_plugin::plugin{};
   auto provider = forge::api::core::installer{environment.apis};
   environment.run(node.provide(provider));
   environment.run(diagnostics.provide(provider));
   const auto events = environment.apis.get<diagnostics_plugin::events_api>(
       {.id = {"forge.plugins.net.p2p.diagnostics.events"}, .major = 1});
   diagnostics.request_stop();
   BOOST_CHECK_THROW(environment.run(diagnostics.initialize(environment.context)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)events->reachability_status(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)events->host_events(), p2p::exceptions::canceled);
   environment.run(diagnostics.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_inflight_source_survives_diagnostics_stop) {
   check_inflight_event_call<false>();
   check_inflight_event_call<true>();
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_late_handles_and_native_read_survive_plugin_destruction) {
   auto source = std::shared_ptr<node_plugin::host_event_source>{};
   auto facade = std::shared_ptr<diagnostics_plugin::events_api>{};
   auto subscription = p2p::host_event_subscription{};
   auto deferred = std::optional<boost::asio::awaitable<std::optional<p2p::host_event>>>{};
   {
      auto fixture = host_fixture{};
      fixture.start();
      source = fixture.source().shared();
      facade = fixture.facade().shared();
      subscription = facade->host_events();
      deferred.emplace(subscription.async_read());
   }
   BOOST_CHECK_THROW((void)source->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)source->reachability_status(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)facade->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)facade->reachability_status(), p2p::exceptions::canceled);
   BOOST_TEST(!subscription.active());
   source.reset();
   facade.reset();
   auto io = boost::asio::io_context{};
   auto result = boost::asio::co_spawn(io, std::move(*deferred), boost::asio::use_future);
   BOOST_TEST(!read_result(io, subscription, result).has_value());
}
