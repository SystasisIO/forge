module;

#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.asio.runtime;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;

#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/reachability_manager.hxx"

namespace {

namespace asio = boost::asio;
namespace p2p = forge::net::p2p;
using manager = p2p::detail::reachability_manager;
using namespace std::chrono_literals;

manager::observer observer(std::uint8_t id, bool v1 = true, bool v2 = true, bool ping = false) {
   return {.peer = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
                                    .data = std::vector<std::uint8_t>(32, id)}),
           .remote = p2p::parse_endpoint("/ip4/11.0.0." + std::to_string(id) + "/tcp/4001"),
           .v1 = v1, .v2 = v2, .ping = ping};
}

struct probe_script {
   mutable std::mutex mutex;
   std::vector<manager::observer> sources;
   std::vector<std::uint64_t> exchanged_sessions;
   std::vector<p2p::endpoint> addresses{p2p::parse_endpoint("/ip4/8.8.8.8/tcp/4001")};
   std::vector<std::pair<bool, std::vector<p2p::endpoint>>> requests;
   bool released = false;
   bool terminal_hold = false;
   bool fail_v1 = false;
   bool fail_v2 = false;
   std::atomic_size_t exchanges{0};
   std::atomic_size_t pings{0};
   std::atomic_size_t cancellations{0};
   std::atomic<std::int64_t> seconds{0};
   std::condition_variable reconciled;
   std::size_t ticks = 0;
   std::size_t candidate_refreshes = 0;
   std::atomic_size_t publications{0};
   forge::asio::notification changed;
   std::weak_ptr<manager> owner;
   std::function<void()> after_snapshot;

   void set_addresses(std::vector<p2p::endpoint> values) {
      const auto lock = std::scoped_lock{mutex};
      addresses = std::move(values);
      if (const auto value = owner.lock()) { static_cast<void>(value->set_addresses(addresses)); }
   }

   void release() {
      {
         const auto lock = std::scoped_lock{mutex};
         released = true;
      }
      changed.notify();
   }

   static asio::awaitable<void> wait(std::shared_ptr<probe_script> self,
                                     std::shared_ptr<p2p::cancellation_latch> cancellation) {
      auto subscription = p2p::cancellation_latch::subscribe(cancellation, [self] {
         ++self->cancellations;
         self->changed.notify();
      });
      for (;;) {
         const auto epoch = self->changed.epoch();
         {
            const auto lock = std::scoped_lock{self->mutex};
            if (self->released || (cancellation->stop_requested() && !self->terminal_hold)) {
               break;
            }
         }
         static_cast<void>(co_await self->changed.async_wait(epoch));
      }
      if (cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(p2p::exceptions::canceled, "scripted reachability cancellation");
      }
   }

   static asio::awaitable<manager::probe_result> exchange(std::shared_ptr<probe_script> self, bool v2,
       std::vector<p2p::endpoint> addresses, std::shared_ptr<p2p::cancellation_latch> cancellation) {
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->requests.emplace_back(v2, addresses);
      }
      ++self->exchanges;
      self->changed.notify();
      co_await wait(self, std::move(cancellation));
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (v2 ? self->fail_v2 : self->fail_v1) {
            FORGE_THROW_EXCEPTION(p2p::exceptions::unsupported_protocol, "scripted unavailable AutoNAT version");
         }
      }
      co_return manager::probe_result{
          .result = {.value = v2 ? p2p::reachability::state::publicly_reachable : p2p::reachability::state::private_network,
                     .observed = addresses.front(), .status_text = v2 ? "v2-result" : "v1-result"},
          .verified_dialback = v2, .internet_scope = true};
   }

   static asio::awaitable<void> ping(std::shared_ptr<probe_script> self,
                                    std::shared_ptr<p2p::cancellation_latch> cancellation) {
      ++self->pings;
      self->changed.notify();
      co_await wait(self, std::move(cancellation));
      FORGE_THROW_EXCEPTION(p2p::exceptions::timeout, "scripted ping failure is not NAT evidence");
   }

   static asio::awaitable<void> count(std::shared_ptr<probe_script> self, std::size_t exchanges,
                                     std::size_t pings = 0, std::size_t cancellations = 0) {
      for (;;) {
         const auto epoch = self->changed.epoch();
         if (self->exchanges.load() >= exchanges && self->pings.load() >= pings &&
             self->cancellations.load() >= cancellations) {
            co_return;
         }
         static_cast<void>(co_await self->changed.async_wait(epoch));
      }
   }
};

manager::callbacks callbacks_for(const std::shared_ptr<probe_script>& script) {
   return {
       .observers = [script] {
          const auto lock = std::scoped_lock{script->mutex};
          ++script->ticks;
          script->reconciled.notify_all();
          return script->sources;
       },
       .candidates = [script] {
          auto snapshot = p2p::detail::reachability_state::candidate_snapshot{};
          auto after = std::function<void()>{};
          {
             const auto lock = std::scoped_lock{script->mutex};
             ++script->candidate_refreshes;
             if (const auto owner = script->owner.lock()) { snapshot = owner->candidates(); }
             after = std::exchange(script->after_snapshot, {});
          }
          if (after) { after(); }
          return snapshot;
       },
       .exchange = [script](manager::observer source, bool v2, std::vector<p2p::endpoint> addresses,
                            std::shared_ptr<p2p::cancellation_latch> cancellation) {
          {
             const auto lock = std::scoped_lock{script->mutex};
             script->exchanged_sessions.push_back(source.session_id);
          }
          return probe_script::exchange(script, v2, std::move(addresses), std::move(cancellation));
       },
       .ping = [script](p2p::peer_id, std::shared_ptr<p2p::cancellation_latch> cancellation) {
          return probe_script::ping(script, std::move(cancellation));
       },
       .changed = [script](p2p::host_event) {
          ++script->publications;
          if (const auto owner = script->owner.lock()) {
             // Reentrant diagnostics must not encounter a held manager mutex.
             static_cast<void>(owner->stats());
             static_cast<void>(owner->current());
          }
       },
       .now = [script] { return std::chrono::steady_clock::time_point{} + std::chrono::seconds{script->seconds.load()}; },
   };
}

template <typename T> T ready(std::future<T>& value) {
   BOOST_REQUIRE(value.wait_for(4s) == std::future_status::ready);
   return value.get();
}

struct manager_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 1}};
   p2p::detail::lifecycle_tracker tracker{runtime.context().get_executor()};
   std::shared_ptr<probe_script> script = std::make_shared<probe_script>();
   std::shared_ptr<manager> owner;

   explicit manager_fixture(p2p::reachability_policy policy = {})
       : owner(std::make_shared<manager>(runtime.context().get_executor(), policy, callbacks_for(script))) {
      script->owner = owner;
      script->set_addresses(script->addresses);
   }

   ~manager_fixture() {
      script->release();
      owner->request_stop();
      auto joined = asio::co_spawn(runtime.context(), owner->async_join(), asio::use_future);
      if (joined.wait_for(4s) != std::future_status::ready) {
         runtime.stop();
         BOOST_ERROR("reachability manager failed bounded cleanup");
         return;
      }
      try {
         joined.get();
      } catch (...) {
         BOOST_ERROR("reachability parent failed during cleanup");
      }
   }

   void barrier() {
      auto promise = std::make_shared<std::promise<void>>();
      auto future = promise->get_future();
      asio::post(runtime.context(), [promise] { promise->set_value(); });
      ready(future);
   }

   void count(std::size_t exchanges, std::size_t pings = 0, std::size_t cancellations = 0) {
      auto future = asio::co_spawn(runtime.context(), probe_script::count(script, exchanges, pings, cancellations),
                                   asio::use_future);
      ready(future);
   }
};

struct unpolled_manager_fixture {
   asio::io_context engine;
   asio::io_context caller;
   p2p::detail::lifecycle_tracker tracker{caller.get_executor()};
   std::shared_ptr<probe_script> script = std::make_shared<probe_script>();
   std::shared_ptr<manager> owner = std::make_shared<manager>(
       engine.get_executor(), p2p::reachability_policy{}, callbacks_for(script));

   unpolled_manager_fixture() {
      script->owner = owner;
      script->set_addresses(script->addresses);
   }

   // Only ready handlers are run; the finite budget also catches accidental
   // self-wakeup loops without relying on a timer or a background thread.
   static void poll_ready(asio::io_context& context) {
      context.restart();
      for (auto index = 0; index < 1024; ++index) {
         if (context.poll_one() == 0) {
            return;
         }
      }
      BOOST_FAIL("manager exceeded the ready-handler budget");
   }

   void poll() {
      engine.restart();
      caller.restart();
      for (auto index = 0; index < 1024; ++index) {
         const auto engine_count = engine.poll_one();
         const auto caller_count = caller.poll_one();
         if (engine_count == 0 && caller_count == 0) {
            return;
         }
      }
      BOOST_FAIL("manager exceeded the ready-handler budget");
   }

   ~unpolled_manager_fixture() {
      script->release();
      owner->request_stop();
      auto joined = asio::co_spawn(caller, owner->async_join(), asio::use_future);
      try {
         poll();
         if (joined.wait_for(0s) != std::future_status::ready) {
            BOOST_ERROR("unpolled manager did not drain during cleanup");
            return;
         }
         joined.get();
      } catch (...) {
         BOOST_ERROR("unpolled manager cleanup failed");
      }
   }
};

asio::awaitable<manager::statistics> stats_after_tracking(p2p::detail::lifecycle_tracker& tracker,
                                                        std::shared_ptr<manager> owner) {
   co_await tracker.wait();
   co_return owner->stats();
}

} // namespace

BOOST_AUTO_TEST_SUITE(reachability_manager_tests)

BOOST_AUTO_TEST_CASE(reachability_manager_coalesced_probe_keeps_original_session_identity) {
   auto fixture = manager_fixture{};
   auto source = observer(1);
   source.session_id = 11;
   fixture.owner->start(fixture.tracker);
   auto first = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future);
   fixture.count(1);
   source.session_id = 12;
   auto second = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future);
   fixture.barrier();
   BOOST_TEST(fixture.owner->stats().waiters == 2U);
   fixture.script->release();
   BOOST_TEST(ready(first).status_text == "v2-result");
   BOOST_TEST(ready(second).status_text == "v2-result");
   const auto lock = std::scoped_lock{fixture.script->mutex};
   // Both protocol phases belong to the first work item, not its newer waiter.
   BOOST_REQUIRE_EQUAL(fixture.script->exchanged_sessions.size(), 2U);
   BOOST_TEST(fixture.script->exchanged_sessions[0] == 11U);
   BOOST_TEST(fixture.script->exchanged_sessions[1] == 11U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_coalesces_manual_and_autonomous_probes_and_bounds_waiters) {
   auto policy = p2p::reachability_policy{};
   policy.max_observers = 4;
   policy.max_pending_probes = 1;
   auto fixture = manager_fixture{policy};
   const auto source = observer(1, false, true);
   fixture.script->sources = {source};
   fixture.owner->start(fixture.tracker);
   BOOST_CHECK_NO_THROW(fixture.owner->start(fixture.tracker));
   fixture.count(1);
   auto pending_overflow = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(2)), asio::use_future);
   BOOST_REQUIRE(pending_overflow.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(pending_overflow.get()), p2p::exceptions::backpressure_rejected);
   auto waiters = std::vector<std::future<p2p::reachability::result>>{};
   for (auto i = 0; i < 4; ++i) {
      waiters.push_back(asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future));
   }
   fixture.barrier();
   BOOST_TEST(fixture.owner->stats().waiters == 4U);
   auto overflow = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future);
   BOOST_REQUIRE(overflow.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(overflow.get()), p2p::exceptions::backpressure_rejected);
   BOOST_TEST(fixture.script->exchanges.load() == 1U);
   fixture.script->release();
   for (auto& waiter : waiters) {
      BOOST_TEST(ready(waiter).status_text == "v2-result");
   }
   BOOST_TEST(fixture.owner->stats().waiters == 0U);
   BOOST_TEST(fixture.owner->stats().pending_probes == 0U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_preserves_partial_protocol_results_without_conflating_state) {
   for (const auto fail_v1 : {false, true}) {
      auto fixture = manager_fixture{};
      fixture.script->fail_v1 = fail_v1;
      fixture.script->fail_v2 = !fail_v1;
      fixture.script->release();
      fixture.owner->start(fixture.tracker);
      auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1)), asio::use_future);
      const auto result = ready(probe);
      BOOST_TEST(result.status_text == (fail_v1 ? "v2-result" : "v1-result"));
      BOOST_TEST(fixture.script->exchanges.load() == 2U);
      const auto event = fixture.owner->current();
      BOOST_CHECK(event.autonat_v1 == p2p::reachability::state::unknown);
      BOOST_TEST(event.autonat_v2.size() == (fail_v1 ? 1U : 0U));
      BOOST_TEST(fixture.owner->stats().probe_errors == 1U);
      BOOST_CHECK(fixture.owner->stats().last_probe_error != nullptr);
   }
}

BOOST_AUTO_TEST_CASE(reachability_manager_reconciles_generation_after_exchange_before_recording) {
   auto fixture = manager_fixture{};
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   fixture.count(1);
   fixture.script->set_addresses({p2p::parse_endpoint("/ip4/8.8.4.4/tcp/5001")});
   fixture.owner->notify_addresses_changed();
   fixture.script->release();
   BOOST_TEST(ready(probe).status_text == "v2-result");
   const auto event = fixture.owner->current();
   BOOST_CHECK(event.effective == p2p::reachability::state::unknown);
   BOOST_TEST(event.autonat_v2.empty());
}

BOOST_AUTO_TEST_CASE(reachability_manager_lan_v2_positive_does_not_create_global_or_v1_evidence) {
   auto fixture = manager_fixture{};
   fixture.script->set_addresses({p2p::parse_endpoint("/ip4/10.0.0.4/tcp/4001")});
   fixture.script->release();
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1)), asio::use_future);
   BOOST_TEST(ready(probe).status_text == "v2-result");
   BOOST_TEST(fixture.script->exchanges.load() == 1U);
   const auto event = fixture.owner->current();
   BOOST_CHECK(event.effective == p2p::reachability::state::unknown);
   BOOST_CHECK(event.autonat_v1 == p2p::reachability::state::unknown);
   BOOST_REQUIRE_EQUAL(event.autonat_v2.size(), 1U);
   BOOST_CHECK(event.autonat_v2.front().value == p2p::reachability::state::publicly_reachable);
   fixture.script->seconds = 600;
   fixture.owner->notify_addresses_changed();
   BOOST_TEST(fixture.owner->current().autonat_v2.empty());
}

BOOST_AUTO_TEST_CASE(reachability_manager_suspended_probe_rejects_aba_without_parent_reconciliation) {
   auto fixture = manager_fixture{};
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   fixture.count(1);
   const auto initial = fixture.owner->candidates();
   fixture.script->set_addresses({p2p::parse_endpoint("/ip4/8.8.4.4/tcp/5001")});
   fixture.script->set_addresses(initial.addresses);
   BOOST_TEST(fixture.owner->candidates().generation == initial.generation + 2);
   fixture.script->release();
   BOOST_TEST(ready(probe).status_text == "v2-result");
   BOOST_TEST(fixture.owner->current().autonat_v2.empty());
   BOOST_TEST(fixture.script->exchanges.load() == 1U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_never_reinstalls_snapshot_after_source_mutation) {
   auto fixture = unpolled_manager_fixture{};
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.caller, fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   fixture.poll();
   BOOST_REQUIRE_EQUAL(fixture.script->exchanges.load(), 1U);
   const auto initial = fixture.owner->candidates();
   const auto replacement = p2p::parse_endpoint("/ip4/8.8.4.4/tcp/5001");
   auto mutated = false;
   fixture.script->after_snapshot = [&] {
      mutated = true;
      fixture.script->set_addresses({replacement});
   };
   fixture.script->release();
   fixture.poll();
   BOOST_REQUIRE(probe.wait_for(0s) == std::future_status::ready);
   BOOST_TEST(probe.get().status_text == "v2-result");
   BOOST_REQUIRE(mutated);
   const auto current = fixture.owner->candidates();
   BOOST_TEST(current.generation == initial.generation + 1);
   BOOST_REQUIRE_EQUAL(current.addresses.size(), 1U);
   BOOST_TEST(current.addresses.front().to_string() == replacement.to_string());
   BOOST_TEST(fixture.owner->current().autonat_v2.empty());
}

BOOST_AUTO_TEST_CASE(reachability_manager_identical_source_set_preserves_pending_result) {
   auto fixture = manager_fixture{};
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   fixture.count(1);
   const auto initial = fixture.owner->candidates();
   auto qualified = initial.addresses.front();
   qualified.peer = observer(2).peer;
   fixture.script->set_addresses({qualified, initial.addresses.front()});
   BOOST_TEST(fixture.owner->candidates().generation == initial.generation);
   fixture.script->release();
   BOOST_TEST(ready(probe).status_text == "v2-result");
   BOOST_TEST(fixture.owner->current().autonat_v2.size() == 1U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_stop_joins_terminal_cleanup_despite_canceled_waiters) {
   auto fixture = manager_fixture{};
   fixture.script->terminal_hold = true;
   fixture.owner->start(fixture.tracker);
   auto cancellation = asio::cancellation_signal{};
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1, false, true)),
       asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
   fixture.count(1);
   asio::post(fixture.runtime.context(), [&] { cancellation.emit(asio::cancellation_type::terminal); });
   BOOST_REQUIRE(probe.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(probe.get()), std::exception);
   BOOST_TEST(fixture.owner->stats().waiters == 0U);
   BOOST_TEST(fixture.owner->stats().pending_probes == 1U);
   fixture.tracker.request_stop();
   fixture.count(1, 0, 1);
   auto join_cancellation = asio::cancellation_signal{};
   auto joined = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_join(),
       asio::bind_cancellation_slot(join_cancellation.slot(), asio::use_future));
   fixture.barrier();
   asio::post(fixture.runtime.context(), [&] { join_cancellation.emit(asio::cancellation_type::terminal); });
   fixture.barrier();
   BOOST_CHECK(joined.wait_for(0s) == std::future_status::timeout);
   fixture.script->release();
   ready(joined);
   auto tracked = asio::co_spawn(fixture.runtime.context(), fixture.tracker.wait(), asio::use_future);
   ready(tracked);
   BOOST_TEST(fixture.owner->stats().pending_probes == 0U);
   BOOST_CHECK_THROW(fixture.owner->start(fixture.tracker), p2p::exceptions::closed);
}

BOOST_AUTO_TEST_CASE(reachability_manager_ping_only_observers_are_bounded_and_do_not_vote_on_nat) {
   auto policy = p2p::reachability_policy{};
   policy.max_parallel_pings = 2;
   auto fixture = manager_fixture{policy};
   for (std::uint8_t id = 1; id <= 5; ++id) {
      fixture.script->sources.push_back(observer(id, false, false, true));
   }
   fixture.owner->start(fixture.tracker);
   fixture.count(0, 2);
   BOOST_TEST(fixture.owner->stats().pending_pings == 2U);
   BOOST_TEST(fixture.script->exchanges.load() == 0U);
   fixture.script->release();
   fixture.count(0, 5);
   fixture.owner->request_stop();
   auto joined = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_join(), asio::use_future);
   ready(joined);
   BOOST_TEST(fixture.owner->stats().ping_failures == 5U);
   BOOST_TEST(fixture.owner->stats().pending_pings == 0U);
   BOOST_CHECK(fixture.owner->current().effective == p2p::reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(reachability_manager_all_protocol_errors_preserve_typed_failure) {
   auto fixture = manager_fixture{};
   fixture.script->fail_v1 = true;
   fixture.script->fail_v2 = true;
   fixture.script->release();
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1)), asio::use_future);
   BOOST_REQUIRE(probe.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(probe.get()), p2p::exceptions::unsupported_protocol);
   BOOST_TEST(fixture.script->exchanges.load() == 2U);
   BOOST_CHECK(fixture.owner->current().effective == p2p::reachability::state::unknown);
   auto unsupported = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(2, false, false, true)),
                                     asio::use_future);
   BOOST_REQUIRE(unsupported.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(unsupported.get()), p2p::exceptions::unsupported_protocol);
   BOOST_TEST(fixture.script->exchanges.load() == 2U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_autonomous_selection_uses_independent_observer_groups) {
   auto fixture = manager_fixture{};
   auto first = observer(1, false, true);
   auto alias = observer(2, false, true);
   alias.remote = first.remote;
   fixture.script->sources = {first, alias, observer(3, false, true)};
   fixture.owner->start(fixture.tracker);
   fixture.count(2);
   fixture.barrier();
   BOOST_TEST(fixture.script->exchanges.load() == 2U);
   BOOST_TEST(fixture.owner->stats().pending_probes == 2U);
   fixture.owner->request_stop();
   auto joined = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_join(), asio::use_future);
   ready(joined);
   BOOST_CHECK(fixture.owner->current().effective == p2p::reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(reachability_manager_lazy_join_owns_self_before_first_suspension) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto script = std::make_shared<probe_script>();
   auto owner = std::make_shared<manager>(runtime.context().get_executor(), p2p::reachability_policy{}, callbacks_for(script));
   auto weak = std::weak_ptr<manager>{owner};
   auto lazy = owner->async_join();
   owner.reset();
   BOOST_TEST(!weak.expired());
   auto joined = asio::co_spawn(runtime.context(), std::move(lazy), asio::use_future);
   ready(joined);
   auto barrier = std::make_shared<std::promise<void>>();
   auto finished = barrier->get_future();
   asio::post(runtime.context(), [barrier] { barrier->set_value(); });
   ready(finished);
   BOOST_TEST(weak.expired());
}

BOOST_AUTO_TEST_CASE(reachability_manager_explicit_clock_deadline_uses_parent_wakeup_and_joins_exchange) {
   auto policy = p2p::reachability_policy{};
   policy.timeout = 10s;
   auto fixture = manager_fixture{policy};
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   fixture.count(1);
   fixture.script->seconds = 11;
   fixture.owner->notify_addresses_changed();
   fixture.count(1, 0, 1);
   BOOST_REQUIRE(probe.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(probe.get()), p2p::exceptions::timeout);
   BOOST_TEST(fixture.owner->stats().pending_probes == 0U);
   BOOST_TEST(fixture.owner->stats().waiters == 0U);
   BOOST_CHECK(fixture.owner->current().effective == p2p::reachability::state::unknown);
}

BOOST_AUTO_TEST_CASE(reachability_default_budget_accepts_v1_negative_after_donor_fifteen_seconds) {
   BOOST_TEST(p2p::reachability_policy{}.timeout.count() == 20'000);
   auto fixture = manager_fixture{};
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(1, true, false)), asio::use_future);
   fixture.count(1);
   // Model the pinned Go v1 service's 15-second negative response using the
   // existing explicit clock. This is not live donor interoperability evidence.
   fixture.script->seconds = 15;
   fixture.script->release();
   BOOST_REQUIRE(probe.wait_for(4s) == std::future_status::ready);
   auto result = p2p::reachability::result{};
   BOOST_CHECK_NO_THROW(result = probe.get());
   BOOST_CHECK(result.value == p2p::reachability::state::private_network);
   BOOST_TEST(result.status_text == "v1-result");
   BOOST_TEST(fixture.owner->stats().probe_errors == 0U);
   BOOST_TEST(fixture.owner->stats().pending_probes == 0U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_rotates_v2_candidates_while_preserving_wire_index_zero) {
   auto fixture = manager_fixture{};
   const auto source = observer(1, false, true);
   fixture.script->sources = {source};
   fixture.script->set_addresses({p2p::parse_endpoint("/ip4/8.8.8.3/tcp/4001"),
                               p2p::parse_endpoint("/ip4/8.8.8.1/tcp/4001"),
                               p2p::parse_endpoint("/ip4/8.8.8.2/tcp/4001")});
   fixture.script->release();
   fixture.owner->start(fixture.tracker);
   for (auto index = 0; index < 4; ++index) {
      auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future);
      BOOST_TEST(ready(probe).status_text == "v2-result");
   }
   {
      const auto lock = std::scoped_lock{fixture.script->mutex};
      BOOST_REQUIRE_GE(fixture.script->requests.size(), 4U);
      for (auto index = std::size_t{}; index < fixture.script->requests.size(); ++index) {
         const auto& [v2, addresses] = fixture.script->requests[index];
         BOOST_TEST(v2);
         BOOST_REQUIRE_EQUAL(addresses.size(), 1U);
         BOOST_TEST(addresses.front().to_string() == "/ip4/8.8.8." + std::to_string(index % 3 + 1) + "/tcp/4001");
      }
   }
   BOOST_TEST(fixture.owner->current().autonat_v2.size() == 3U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_refreshes_candidates_when_nat_publication_is_unchanged) {
   auto fixture = manager_fixture{};
   fixture.owner->start(fixture.tracker);
   auto lock = std::unique_lock{fixture.script->mutex};
   BOOST_REQUIRE(fixture.script->reconciled.wait_for(lock, 4s, [&] { return fixture.script->ticks >= 1; }));
   const auto ticks = fixture.script->ticks;
   const auto refreshes = fixture.script->candidate_refreshes;
   const auto publications = fixture.script->publications.load();
   fixture.script->seconds = 600;
   fixture.owner->notify_addresses_changed();
   BOOST_REQUIRE(fixture.script->reconciled.wait_for(lock, 4s, [&] { return fixture.script->ticks > ticks; }));
   BOOST_TEST(fixture.script->candidate_refreshes > refreshes);
   BOOST_TEST(fixture.script->publications.load() == publications);
   BOOST_TEST(fixture.script->exchanges.load() == 0U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_canceling_one_waiter_preserves_shared_probe_and_other_result) {
   auto policy = p2p::reachability_policy{};
   policy.max_pending_probes = 1;
   auto fixture = manager_fixture{policy};
   fixture.owner->start(fixture.tracker);
   const auto source = observer(1, false, true);
   auto cancellation = asio::cancellation_signal{};
   auto canceled = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source),
       asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
   fixture.count(1);
   auto survivor = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future);
   fixture.barrier();
   BOOST_TEST(fixture.owner->stats().waiters == 2U);
   asio::post(fixture.runtime.context(), [&] { cancellation.emit(asio::cancellation_type::terminal); });
   BOOST_REQUIRE(canceled.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(canceled.get()), std::exception);
   BOOST_TEST(fixture.owner->stats().waiters == 1U);
   BOOST_TEST(fixture.owner->stats().pending_probes == 1U);
   BOOST_TEST(fixture.script->cancellations.load() == 0U);
   BOOST_CHECK(survivor.wait_for(0s) == std::future_status::timeout);
   auto overflow = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(observer(2)), asio::use_future);
   BOOST_REQUIRE(overflow.wait_for(4s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(overflow.get()), p2p::exceptions::backpressure_rejected);
   fixture.script->release();
   BOOST_TEST(ready(survivor).status_text == "v2-result");
   BOOST_TEST(fixture.script->exchanges.load() == 1U);
   BOOST_TEST(fixture.owner->stats().waiters == 0U);
   BOOST_TEST(fixture.owner->stats().pending_probes == 0U);
   BOOST_TEST(fixture.owner->stats().probe_errors == 0U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_failed_v2_candidate_does_not_starve_siblings) {
   auto fixture = manager_fixture{};
   const auto source = observer(1, false, true);
   fixture.script->sources = {source};
   fixture.script->set_addresses({p2p::parse_endpoint("/ip4/8.8.8.1/tcp/4001"),
                               p2p::parse_endpoint("/ip4/8.8.8.2/tcp/4001")});
   fixture.script->fail_v2 = true;
   fixture.script->release();
   fixture.owner->start(fixture.tracker);
   for (auto index = 0; index < 3; ++index) {
      auto probe = asio::co_spawn(fixture.runtime.context(), fixture.owner->async_probe(source), asio::use_future);
      BOOST_REQUIRE(probe.wait_for(4s) == std::future_status::ready);
      BOOST_CHECK_THROW(static_cast<void>(probe.get()), p2p::exceptions::unsupported_protocol);
   }
   {
      const auto lock = std::scoped_lock{fixture.script->mutex};
      BOOST_REQUIRE_GE(fixture.script->requests.size(), 3U);
      for (auto index = std::size_t{}; index < fixture.script->requests.size(); ++index) {
         const auto& [v2, addresses] = fixture.script->requests[index];
         BOOST_TEST(v2);
         BOOST_REQUIRE_EQUAL(addresses.size(), 1U);
         BOOST_TEST(addresses.front().to_string() == "/ip4/8.8.8." + std::to_string(index % 2 + 1) + "/tcp/4001");
      }
   }
   BOOST_TEST(fixture.owner->current().autonat_v2.empty());
   BOOST_TEST(fixture.owner->stats().pending_probes == 0U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_lifecycle_stop_before_first_poll_closes_admission) {
   auto fixture = unpolled_manager_fixture{};
   fixture.owner->start(fixture.tracker);
   fixture.tracker.request_stop();
   auto tracked = asio::co_spawn(fixture.caller,
       stats_after_tracking(fixture.tracker, fixture.owner), asio::use_future);
   unpolled_manager_fixture::poll_ready(fixture.caller);
   BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   fixture.poll();
   BOOST_REQUIRE(tracked.wait_for(0s) == std::future_status::ready);
   BOOST_TEST(tracked.get().pending_probes == 0U);

   auto probe = asio::co_spawn(fixture.caller, fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   fixture.poll();
   BOOST_REQUIRE(probe.wait_for(0s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(probe.get()), p2p::exceptions::closed);
   BOOST_CHECK_THROW(fixture.owner->start(fixture.tracker), p2p::exceptions::closed);
   BOOST_CHECK_THROW(static_cast<void>(fixture.owner->set_addresses({})), p2p::exceptions::closed);
   BOOST_TEST(fixture.script->exchanges.load() == 0U);
   BOOST_TEST(fixture.script->ticks == 0U);
}

BOOST_AUTO_TEST_CASE(reachability_manager_early_lifecycle_stop_drains_admitted_probe_before_tracking_release) {
   auto fixture = unpolled_manager_fixture{};
   // A broken early-stop wrapper would leave this exchange blocked after
   // retiring the parent's lifecycle ticket. Cleanup still releases it.
   fixture.script->terminal_hold = true;
   fixture.owner->start(fixture.tracker);
   auto probe = asio::co_spawn(fixture.caller, fixture.owner->async_probe(observer(1, false, true)), asio::use_future);
   unpolled_manager_fixture::poll_ready(fixture.caller);
   BOOST_REQUIRE_EQUAL(fixture.owner->stats().pending_probes, 1U);
   BOOST_TEST(fixture.owner->stats().waiters == 1U);
   BOOST_TEST(fixture.script->exchanges.load() == 0U);
   BOOST_TEST(fixture.script->ticks == 0U);

   fixture.tracker.request_stop();
   auto tracked = asio::co_spawn(fixture.caller,
       stats_after_tracking(fixture.tracker, fixture.owner), asio::use_future);
   unpolled_manager_fixture::poll_ready(fixture.caller);
   BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   fixture.poll();
   BOOST_REQUIRE(tracked.wait_for(0s) == std::future_status::ready);
   const auto retired = tracked.get();
   BOOST_TEST(retired.pending_probes == 0U);
   BOOST_TEST(retired.pending_pings == 0U);
   BOOST_REQUIRE(probe.wait_for(0s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(probe.get()), p2p::exceptions::canceled);
   BOOST_TEST(fixture.owner->stats().waiters == 0U);
   BOOST_TEST(fixture.script->exchanges.load() == 0U);
   BOOST_TEST(fixture.script->ticks == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
