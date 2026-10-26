#include <cstddef>
#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/scope/scope_exit.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <stdexcept>
#include <utility>
#include <vector>

import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.log.logger;

namespace {

using forge::app::lifecycle_operation;
using forge::app::lifecycle_phase;
using forge::app::plugin_lifecycle;

constexpr auto test_timeout = std::chrono::seconds{2};

template <typename T>
concept can_stop = requires(T value) { value.request_stop(); };

template <typename T>
concept can_begin = requires(T value) { value.begin(lifecycle_operation::startup); };

static_assert(!can_stop<forge::app::lifecycle_observer>);
static_assert(!can_begin<forge::app::lifecycle_observer>);
static_assert(!std::is_copy_constructible_v<plugin_lifecycle::admission>);
static_assert(std::is_nothrow_move_constructible_v<plugin_lifecycle::admission>);
static_assert(std::is_nothrow_move_assignable_v<plugin_lifecycle::admission>);

boost::asio::awaitable<std::exception_ptr> capture(boost::asio::awaitable<void> value) {
   try {
      co_await std::move(value);
   } catch (...) {
      co_return std::current_exception();
   }
   co_return std::exception_ptr{};
}

boost::asio::awaitable<void> begin_and_finish(plugin_lifecycle lifecycle, lifecycle_operation value) {
   auto operation = co_await lifecycle.begin(value);
   if (const auto error = operation.complete()) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<void> executor_barrier() {
   co_return;
}

boost::asio::awaitable<void> hold_admission(plugin_lifecycle lifecycle, std::atomic_size_t& active,
                                           forge::asio::notification& changed, forge::asio::notification& entered,
                                           forge::asio::notification& release, std::stop_token stop) {
   const auto admitted = lifecycle.admit(lifecycle.snapshot().generation, lifecycle_phase::started,
                                         active, 1, changed);
   if (!admitted.active()) {
      throw forge::app::exceptions::invalid_state{"test request was not admitted"};
   }
   const auto observed = release.epoch();
   entered.notify();
   static_cast<void>(co_await release.async_wait(observed, stop));
}

boost::asio::awaitable<void> cancel_then_complete(plugin_lifecycle::operation operation,
                                                std::stop_source* stop,
                                                boost::asio::cancellation_signal* signal) {
   if (stop != nullptr) {
      static_cast<void>(stop->request_stop());
   } else {
      signal->emit(boost::asio::cancellation_type::terminal);
   }
   if (const auto error = operation.complete()) {
      std::rethrow_exception(error);
   }
   co_return;
}

boost::asio::awaitable<void> fail_then_retry(plugin_lifecycle lifecycle,
                                          plugin_lifecycle::operation first,
                                          std::exception_ptr first_error,
                                          std::exception_ptr retry_error) {
   first.complete(std::move(first_error));
   auto retry = co_await lifecycle.begin(lifecycle_operation::shutdown);
   retry.complete(std::move(retry_error));
}

boost::asio::awaitable<std::exception_ptr> finish_failed_cleanup(plugin_lifecycle lifecycle,
                                                               std::exception_ptr error) {
   auto operation = co_await lifecycle.begin(lifecycle_operation::shutdown);
   co_return operation.complete(std::move(error));
}

template <typename T>
T ready(std::future<T>& value) {
   BOOST_REQUIRE(value.wait_for(test_timeout) == std::future_status::ready);
   return value.get();
}

void ready(std::future<void>& value) {
   BOOST_REQUIRE(value.wait_for(test_timeout) == std::future_status::ready);
   value.get();
}

template <typename Error>
bool is_error(const std::exception_ptr& value) {
   if (!value) {
      return false;
   }
   try {
      std::rethrow_exception(value);
   } catch (const Error&) {
      return true;
   } catch (...) {
      return false;
   }
}

struct hook_gate {
   std::atomic_bool blocked = false;
   forge::asio::notification entered;
   forge::asio::notification release;

   boost::asio::awaitable<void> wait() {
      const auto observed = release.epoch();
      entered.notify();
      if (blocked.load()) {
         const auto current = co_await release.async_wait_until(
             observed, std::chrono::steady_clock::now() + test_timeout);
         if (current == observed) {
            throw forge::app::exceptions::invalid_state{"test hook deadline expired"};
         }
      }
   }
};

struct plugin_resources {
   explicit plugin_resources(plugin_lifecycle value) : lifecycle{std::move(value)} {}

   plugin_lifecycle lifecycle;
   hook_gate initialization;
   hook_gate startup;
   hook_gate shutdown;
   std::atomic_int initializes = 0;
   std::atomic_int preparations = 0;
   std::atomic_int startups = 0;
   std::atomic_int stops = 0;
   std::atomic_int shutdowns = 0;
   std::atomic_int failed_cleanups = 0;
   std::atomic_bool fail_startup = false;
   std::atomic_bool partial_resource = false;
   std::atomic_bool installed_resource = false;
   std::atomic_bool stop_observed_closed_admission = false;
   std::atomic_bool fail_identity = false;
};

class controlled_plugin final : public forge::app::plugin {
 public:
   controlled_plugin() : resources{std::make_shared<plugin_resources>(lifecycle_handle())} {}

   forge::app::plugin_id id() const override {
      if (resources->fail_identity.load()) {
         throw forge::app::exceptions::invalid_state{"identity logging failure"};
      }
      return {.value = "lifecycle-test"};
   }

   std::string version() const override {
      return "1";
   }

   std::shared_ptr<plugin_resources> resources;

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override {
      const auto owner = resources;
      ++owner->initializes;
      co_await owner->initialization.wait();
   }

   boost::asio::awaitable<void> on_startup() override {
      const auto owner = resources;
      const auto generation = owner->lifecycle.snapshot().generation;
      ++owner->startups;
      // Partial acquisition belongs to the plugin and is rolled back only by
      // its cleanup hook, even when startup subsequently fails or is stopped.
      static_cast<void>(owner->lifecycle.try_admit(generation, [&owner] {
         owner->partial_resource.store(true);
      }));
      co_await owner->startup.wait();
      if (owner->fail_startup.load()) {
         throw forge::app::exceptions::startup_failed{"late startup failure"};
      }
      static_cast<void>(owner->lifecycle.try_admit(generation, [&owner] {
         owner->installed_resource.store(true);
      }));
   }

   boost::asio::awaitable<void> on_after_initialize() override {
      ++resources->preparations;
      co_return;
   }

   void on_request_stop() noexcept override {
      ++resources->stops;
      // Re-entry proves the virtual hook is not called under the lifecycle lock.
      resources->stop_observed_closed_admission.store(!resources->lifecycle.snapshot().admission_open);
   }

   boost::asio::awaitable<void> on_shutdown() override {
      const auto owner = resources;
      const auto attempt = ++owner->shutdowns;
      co_await owner->shutdown.wait();
      if (attempt <= owner->failed_cleanups.load()) {
         throw forge::app::exceptions::shutdown_failed{"cleanup requires retry"};
      }
      owner->installed_resource.store(false);
      owner->partial_resource.store(false);
   }
};

struct plugin_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler{runtime};
   forge::app::event_journal events{};
   forge::app::plugin_context context{scheduler, events};
   controlled_plugin plugin;

   ~plugin_fixture() {
      // On an assertion failure, stop worker execution before destroying the
      // plugin, so no suspended hook can resume against a destroyed owner.
      runtime.stop();
   }

   std::future<std::exception_ptr> run(boost::asio::awaitable<void> value) {
      return boost::asio::co_spawn(runtime.context(), capture(std::move(value)), boost::asio::use_future);
   }

   void initialize() {
      auto result = run(plugin.initialize(context));
      BOOST_REQUIRE(!ready(result));
   }

   void wait_entered(hook_gate& gate, std::uint64_t observed) {
      auto waiting = boost::asio::co_spawn(
          runtime.context(), gate.entered.async_wait_until(observed, std::chrono::steady_clock::now() + test_timeout),
          boost::asio::use_future);
      BOOST_REQUIRE(ready(waiting) != observed);
   }
};

struct retired_exception_probe {
   retired_exception_probe(forge::app::lifecycle_observer value, std::atomic_int& count)
       : observer{std::move(value)}, destructions{&count} {}

   ~retired_exception_probe() {
      static_cast<void>(observer.snapshot());
      ++*destructions;
   }

   forge::app::lifecycle_observer observer;
   std::atomic_int* destructions;
};

class reentrant_cleanup_error final : public std::exception {
 public:
   explicit reentrant_cleanup_error(std::shared_ptr<retired_exception_probe> value)
       : probe{std::move(value)} {}
   std::shared_ptr<retired_exception_probe> probe;
};

class lifecycle_throwing_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override {
      ++calls;
      throw std::runtime_error{"sink-secret-must-not-be-logged"};
   }
   std::atomic_int calls = 0;
};

class lifecycle_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& value) override {
      const auto lock = std::scoped_lock{mutex};
      records.push_back(value);
   }
   std::mutex mutex;
   std::vector<forge::log_record> records;
};

struct lifecycle_sink_registration {
   lifecycle_sink_registration() {
      previous_level = route.get_log_level();
      route.set_log_level(forge::log_level::debug);
      route.add_sink(throwing);
      route.add_sink(captured);
   }
   ~lifecycle_sink_registration() {
      route.remove_sink(throwing);
      route.remove_sink(captured);
      route.set_log_level(previous_level);
   }

   forge::logger route = forge::logger::get("forge.app.plugin");
   forge::log_level previous_level;
   std::shared_ptr<lifecycle_throwing_sink> throwing = std::make_shared<lifecycle_throwing_sink>();
   std::shared_ptr<lifecycle_capture_sink> captured = std::make_shared<lifecycle_capture_sink>();
};

} // namespace

BOOST_AUTO_TEST_CASE(plugin_lifecycle_direct_calls_are_guarded_without_configure_or_provide) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::initialized);

   auto duplicate_init = fixture.run(fixture.plugin.initialize(fixture.context));
   BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(duplicate_init)));
   auto starting = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(starting));
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::started);
   auto repeated = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(repeated));
   BOOST_CHECK_EQUAL(fixture.plugin.resources->startups.load(), 1);
   BOOST_CHECK_EQUAL(fixture.plugin.resources->preparations.load(), 1);
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().completed(lifecycle_operation::after_initialize));

   fixture.plugin.request_stop();
   fixture.plugin.request_stop();
   BOOST_CHECK_EQUAL(fixture.plugin.resources->stops.load(), 1);
   BOOST_CHECK(fixture.plugin.resources->stop_observed_closed_admission.load());
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopping);
   auto shutdown = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(shutdown));
   auto repeated_shutdown = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(repeated_shutdown));
   BOOST_CHECK_EQUAL(fixture.plugin.resources->shutdowns.load(), 1);
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
   BOOST_CHECK(!fixture.plugin.resources->partial_resource.load());
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_setup_and_phase_qualified_admission_share_one_generation) {
   auto fixture = plugin_fixture{};
   auto lifecycle = fixture.plugin.resources->lifecycle;
   const auto created = lifecycle.snapshot();
   auto installed = false;
   BOOST_CHECK(lifecycle.try_admit(created.generation, [&installed] { installed = true; }));
   BOOST_CHECK(installed);
   BOOST_CHECK(!lifecycle.try_admit(created.generation, lifecycle_phase::started, [] {}));

   auto configured = fixture.run(begin_and_finish(lifecycle, lifecycle_operation::configure));
   BOOST_REQUIRE(!ready(configured));
   BOOST_CHECK(lifecycle.snapshot().completed(lifecycle_operation::configure));
   BOOST_CHECK(lifecycle.try_admit(created.generation, lifecycle_phase::configured, [] {}));
   fixture.initialize();
   auto starting = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(starting));
   BOOST_CHECK(lifecycle.try_admit(created.generation, lifecycle_phase::started, [] {}));

   fixture.plugin.request_stop();
   BOOST_CHECK_NE(lifecycle.snapshot().generation, created.generation);
   BOOST_CHECK(!lifecycle.try_admit(created.generation, [] {}));
   BOOST_CHECK(!lifecycle.try_admit(lifecycle.snapshot().generation, [] {}));
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_scoped_admission_without_phase_preserves_setup_and_word_limit) {
   auto active = std::atomic_size_t{0};
   auto changed = forge::asio::notification{};
   auto lifecycle = plugin_lifecycle{};
   const auto generation = lifecycle.snapshot().generation;
   BOOST_CHECK(lifecycle.snapshot().phase == lifecycle_phase::created);
   BOOST_CHECK(lifecycle.admit(generation, lifecycle_phase::started, active, 1, changed).status() ==
               forge::app::admission_status::closed);
   BOOST_CHECK(lifecycle.admit(generation + 1, active, 1, changed).status() ==
               forge::app::admission_status::closed);
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 0U);
   {
      auto admitted = lifecycle.admit(generation, active, 1, changed);
      BOOST_REQUIRE(admitted.active());
      BOOST_CHECK(lifecycle.admit(generation, active, 1, changed).status() ==
                  forge::app::admission_status::exhausted);
      auto moved = plugin_lifecycle::admission{std::move(admitted)};
      BOOST_CHECK(!admitted.active() && moved.active());
      BOOST_CHECK_EQUAL(active.load(), 1U);
      moved.release();
      moved.release();
   }
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 1U);
   BOOST_CHECK_THROW(([&] {
      const auto admitted = lifecycle.admit(generation, active, 1, changed);
      BOOST_REQUIRE(admitted.active());
      throw forge::app::exceptions::invalid_state{"test request failed"};
   }()), forge::app::exceptions::invalid_state);
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 2U);

   const auto configured_limit = std::numeric_limits<std::uint32_t>::max();
   active.store(static_cast<std::size_t>(configured_limit) - 1);
   {
      const auto admitted = lifecycle.admit(generation, active, configured_limit, changed);
      BOOST_CHECK(admitted.active());
      BOOST_CHECK_EQUAL(active.load(), static_cast<std::size_t>(configured_limit));
      BOOST_CHECK(lifecycle.admit(generation, active, configured_limit, changed).status() ==
                  forge::app::admission_status::exhausted);
   }
   BOOST_CHECK_EQUAL(active.load(), static_cast<std::size_t>(configured_limit) - 1);
   active.store(std::numeric_limits<std::size_t>::max() - 1);
   {
      const auto admitted = lifecycle.admit(generation, active, std::numeric_limits<std::size_t>::max(), changed);
      BOOST_CHECK(admitted.active());
      BOOST_CHECK_EQUAL(active.load(), std::numeric_limits<std::size_t>::max());
      BOOST_CHECK(lifecycle.admit(generation, active, std::numeric_limits<std::size_t>::max(), changed).status() ==
                  forge::app::admission_status::exhausted);
   }
   BOOST_CHECK_EQUAL(active.load(), std::numeric_limits<std::size_t>::max() - 1);
   active.store(0);
   const auto epoch = changed.epoch();
   BOOST_CHECK(lifecycle.request_stop());
   lifecycle.complete_stop();
   BOOST_CHECK(lifecycle.admit(generation, active, 1, changed).status() == forge::app::admission_status::closed);
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), epoch);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_scoped_admission_bounds_moves_and_unwind) {
   auto fixture = plugin_fixture{};
   auto lifecycle = fixture.plugin.resources->lifecycle;
   auto active = std::atomic_size_t{0};
   auto changed = forge::asio::notification{};
   const auto generation = lifecycle.snapshot().generation;
   BOOST_CHECK(lifecycle.admit(generation, lifecycle_phase::started, active, 1, changed).status() ==
               forge::app::admission_status::closed);
   fixture.initialize();
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(startup));
   BOOST_CHECK(lifecycle.admit(generation + 1, lifecycle_phase::started, active, 1, changed).status() ==
               forge::app::admission_status::closed);
   BOOST_CHECK(lifecycle.admit(generation, lifecycle_phase::started, active, 0, changed).status() ==
               forge::app::admission_status::exhausted);
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 0U);
   {
      auto first = lifecycle.admit(generation, lifecycle_phase::started, active, 2, changed);
      auto second = lifecycle.admit(generation, lifecycle_phase::started, active, 2, changed);
      BOOST_CHECK(first.status() == forge::app::admission_status::accepted && first.active());
      BOOST_CHECK(lifecycle.admit(generation, lifecycle_phase::started, active, 2, changed).status() ==
                  forge::app::admission_status::exhausted);
      BOOST_CHECK_EQUAL(active.load(), 2U);
      auto moved = plugin_lifecycle::admission{std::move(first)};
      BOOST_CHECK(!first.active() && moved.active());
      second = std::move(moved);
      BOOST_CHECK(!moved.active() && second.active());
      BOOST_CHECK_EQUAL(active.load(), 1U);
      BOOST_CHECK_EQUAL(changed.epoch(), 1U);
      second.release();
      second.release();
      BOOST_CHECK(!second.active());
      BOOST_CHECK(second.status() == forge::app::admission_status::accepted);
   }
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 2U);
   auto unwound = false;
   try {
      const auto admitted = lifecycle.admit(generation, lifecycle_phase::started, active, 1, changed);
      BOOST_REQUIRE(admitted.active());
      throw forge::app::exceptions::invalid_state{"test request failed"};
   } catch (const forge::app::exceptions::invalid_state&) {
      unwound = true;
   }
   BOOST_CHECK(unwound);
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 3U);

   active.store(std::numeric_limits<std::size_t>::max() - 1);
   {
      const auto last = lifecycle.admit(generation, lifecycle_phase::started, active,
                                         std::numeric_limits<std::size_t>::max(), changed);
      BOOST_CHECK(last.active());
      BOOST_CHECK_EQUAL(active.load(), std::numeric_limits<std::size_t>::max());
      BOOST_CHECK(lifecycle.admit(generation, lifecycle_phase::started, active,
                                   std::numeric_limits<std::size_t>::max(), changed).status() ==
                  forge::app::admission_status::exhausted);
      BOOST_CHECK_EQUAL(active.load(), std::numeric_limits<std::size_t>::max());
   }
   BOOST_CHECK_EQUAL(active.load(), std::numeric_limits<std::size_t>::max() - 1);
   active.store(0);
   auto shutdown = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(shutdown));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_scoped_admission_concurrent_bound_and_stop) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(startup));
   auto lifecycle = fixture.plugin.resources->lifecycle;
   const auto generation = lifecycle.snapshot().generation;
   auto active = std::atomic_size_t{0};
   auto changed = forge::asio::notification{};
   auto accepted = std::atomic<unsigned>{0};
   auto exhausted = std::atomic<unsigned>{0};
   auto rendezvous = std::barrier{9};
   auto threads = std::vector<std::jthread>{};
   threads.reserve(8);
   for (auto index = 0; index < 8; ++index) {
      threads.emplace_back([&] {
         rendezvous.arrive_and_wait();
         const auto admitted = lifecycle.admit(generation, lifecycle_phase::started, active, 3, changed);
         if (admitted.active()) ++accepted;
         else if (admitted.status() == forge::app::admission_status::exhausted) ++exhausted;
         rendezvous.arrive_and_wait();
         rendezvous.arrive_and_wait();
      });
   }
   rendezvous.arrive_and_wait();
   rendezvous.arrive_and_wait();
   BOOST_CHECK_EQUAL(active.load(), 3U);
   BOOST_CHECK_EQUAL(accepted.load(), 3U);
   BOOST_CHECK_EQUAL(exhausted.load(), 5U);
   fixture.plugin.request_stop();
   BOOST_CHECK(lifecycle.admit(generation, lifecycle_phase::started, active, 3, changed).status() ==
               forge::app::admission_status::closed);
   BOOST_CHECK(lifecycle.admit(lifecycle.snapshot().generation, lifecycle_phase::started, active, 3, changed).status() ==
               forge::app::admission_status::closed);
   rendezvous.arrive_and_wait();
   threads.clear();
   BOOST_CHECK_EQUAL(active.load(), 0U);
   BOOST_CHECK_EQUAL(changed.epoch(), 3U);
   auto shutdown = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(shutdown));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_scoped_admission_cancellation_releases_and_wakes_drain) {
   // The fixture stops its runtime before the borrowed request state is destroyed,
   // including a failed assertion while the request coroutine is still suspended.
   auto active = std::atomic_size_t{0};
   auto changed = forge::asio::notification{};
   auto entered = forge::asio::notification{};
   auto release = forge::asio::notification{};
   auto stop = std::stop_source{};
   auto fixture = plugin_fixture{};
   fixture.initialize();
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(startup));
   auto lifecycle = fixture.plugin.resources->lifecycle;
   const auto entered_epoch = entered.epoch();
   auto waiting = fixture.run(hold_admission(lifecycle, active, changed, entered, release, stop.get_token()));
   const auto stop_on_exit = boost::scope::scope_exit{[&] {
      stop.request_stop();
      release.notify();
      if (waiting.valid()) {
         if (waiting.wait_for(test_timeout) == std::future_status::ready) {
            static_cast<void>(waiting.get());
         } else {
            fixture.runtime.stop();
         }
      }
   }};
   auto entry = boost::asio::co_spawn(fixture.runtime.context(),
       entered.async_wait_until(entered_epoch, std::chrono::steady_clock::now() + test_timeout),
       boost::asio::use_future);
   BOOST_REQUIRE_NE(ready(entry), entered_epoch);
   BOOST_CHECK_EQUAL(active.load(), 1U);
   const auto before = changed.epoch();
   auto drain = boost::asio::co_spawn(fixture.runtime.context(),
       changed.async_wait_until(before, std::chrono::steady_clock::now() + test_timeout),
       boost::asio::use_future);
   fixture.plugin.request_stop();
   stop.request_stop();
   const auto error = ready(waiting);
   BOOST_CHECK(is_error<boost::system::system_error>(error));
   BOOST_CHECK_NE(ready(drain), before);
   BOOST_CHECK_EQUAL(active.load(), 0U);
   auto shutdown = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(shutdown));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_input_operations_reject_duplicates_and_owner_ensure_joins_atomically) {
   auto fixture = plugin_fixture{};
   auto lifecycle = fixture.plugin.resources->lifecycle;
   auto serial = boost::asio::make_strand(fixture.runtime.context());
   for (const auto value : {lifecycle_operation::configure, lifecycle_operation::provide,
                            lifecycle_operation::initialize}) {
      auto beginning = boost::asio::co_spawn(serial, lifecycle.begin(value), boost::asio::use_future);
      auto operation = ready(beginning);
      BOOST_REQUIRE(static_cast<bool>(operation));
      auto duplicate = fixture.run(begin_and_finish(lifecycle, value));
      BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(duplicate)));
      auto ensured = boost::asio::co_spawn(serial, lifecycle.begin_joined(value), boost::asio::use_future);
      auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
      ready(barrier);
      BOOST_CHECK(ensured.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
      BOOST_REQUIRE(!operation.complete());
      BOOST_CHECK(!static_cast<bool>(ready(ensured)));
      BOOST_CHECK(lifecycle.snapshot().completed(value));
      auto repeated = fixture.run(begin_and_finish(lifecycle, value));
      BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(repeated)));
      auto completed_ensure = boost::asio::co_spawn(serial, lifecycle.begin_joined(value), boost::asio::use_future);
      BOOST_CHECK(!static_cast<bool>(ready(completed_ensure)));
   }
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_stop_before_initialization_is_fail_closed) {
   auto fixture = plugin_fixture{};
   fixture.plugin.request_stop();
   auto initialize = fixture.run(fixture.plugin.initialize(fixture.context));
   BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(initialize)));
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(startup)));
   BOOST_CHECK_EQUAL(fixture.plugin.resources->initializes.load(), 0);
   BOOST_CHECK_EQUAL(fixture.plugin.resources->startups.load(), 0);
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopping);
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_late_success_cannot_publish_and_shutdown_drains_startup) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   const auto owner = fixture.plugin.resources;
   owner->startup.blocked.store(true);
   const auto entered = owner->startup.entered.epoch();
   auto startup = fixture.run(fixture.plugin.startup());
   fixture.wait_entered(owner->startup, entered);
   BOOST_CHECK(owner->partial_resource.load());
   fixture.plugin.request_stop();
   auto first_cleanup = fixture.run(fixture.plugin.shutdown());
   auto second_cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_CHECK(first_cleanup.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(owner->shutdowns.load(), 0);
   owner->startup.release.notify();

   const auto startup_error = ready(startup);
   BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(startup_error));
   BOOST_CHECK(is_error<forge::app::exceptions::operation_stopped>(startup_error));
   BOOST_REQUIRE(!ready(first_cleanup));
   BOOST_REQUIRE(!ready(second_cleanup));
   BOOST_CHECK_EQUAL(owner->shutdowns.load(), 1);
   BOOST_CHECK(!owner->installed_resource.load());
   const auto final = fixture.plugin.lifecycle().snapshot();
   BOOST_CHECK(final.phase == lifecycle_phase::stopped);
   BOOST_CHECK(!final.completed(lifecycle_operation::startup));
   BOOST_REQUIRE(final.failure);
   BOOST_CHECK(final.failure->operation == lifecycle_operation::startup);
   BOOST_REQUIRE(final.failure->exception);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_late_failure_is_published_before_startup_joiners_wake) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   const auto owner = fixture.plugin.resources;
   owner->startup.blocked.store(true);
   owner->fail_startup.store(true);
   const auto entered = owner->startup.entered.epoch();
   auto serial = boost::asio::make_strand(fixture.runtime.context());
   auto startup = boost::asio::co_spawn(serial, capture(fixture.plugin.startup()), boost::asio::use_future);
   fixture.wait_entered(owner->startup, entered);
   auto joined = boost::asio::co_spawn(serial, capture(fixture.plugin.startup()), boost::asio::use_future);
   auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
   ready(barrier);
   fixture.plugin.request_stop();
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   owner->startup.release.notify();

   const auto error = ready(startup);
   const auto joined_error = ready(joined);
   BOOST_REQUIRE(error);
   BOOST_CHECK(error == joined_error);
   BOOST_CHECK(is_error<forge::app::exceptions::startup_failed>(error));
   const auto snapshot = fixture.plugin.lifecycle().snapshot();
   BOOST_REQUIRE(snapshot.failure);
   BOOST_CHECK(snapshot.failure->exception == error);
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK(!owner->partial_resource.load());
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_shutdown_waits_for_initialization_and_synchronous_stop_hook) {
   auto fixture = plugin_fixture{};
   const auto owner = fixture.plugin.resources;
   owner->initialization.blocked.store(true);
   const auto entered = owner->initialization.entered.epoch();
   auto initialization = fixture.run(fixture.plugin.initialize(fixture.context));
   fixture.wait_entered(owner->initialization, entered);

   auto lifecycle = owner->lifecycle;
   BOOST_REQUIRE(lifecycle.request_stop());
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   owner->initialization.release.notify();
   BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(initialization)));
   BOOST_CHECK(cleanup.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(owner->shutdowns.load(), 0);
   lifecycle.complete_stop();
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_failed_cleanup_coalesces_then_later_retry_succeeds_once) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   const auto owner = fixture.plugin.resources;
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(startup));
   owner->failed_cleanups.store(1);
   owner->shutdown.blocked.store(true);
   const auto entered = owner->shutdown.entered.epoch();
   auto serial = boost::asio::make_strand(fixture.runtime.context());
   auto cleanup = boost::asio::co_spawn(serial, capture(fixture.plugin.shutdown()), boost::asio::use_future);
   fixture.wait_entered(owner->shutdown, entered);
   auto joined = boost::asio::co_spawn(serial, capture(fixture.plugin.shutdown()), boost::asio::use_future);
   auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
   ready(barrier);
   owner->shutdown.release.notify();

   const auto first_error = ready(cleanup);
   BOOST_REQUIRE(first_error);
   BOOST_CHECK(ready(joined) == first_error);
   BOOST_CHECK(is_error<forge::app::exceptions::shutdown_failed>(first_error));
   const auto failed = fixture.plugin.lifecycle().snapshot();
   BOOST_CHECK(failed.phase == lifecycle_phase::stopping);
   BOOST_CHECK(!failed.completed(lifecycle_operation::shutdown));
   BOOST_REQUIRE(failed.failure);
   BOOST_CHECK(failed.failure->exception == first_error);
   BOOST_CHECK(owner->partial_resource.load());

   owner->shutdown.blocked.store(false);
   auto retry = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(retry));
   auto repeated = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(repeated));
   BOOST_CHECK_EQUAL(owner->shutdowns.load(), 2);
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
   BOOST_CHECK(!owner->partial_resource.load());
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_immediate_cleanup_retry_cannot_replace_joined_attempt_error) {
   auto fixture = plugin_fixture{};
   auto lifecycle = fixture.plugin.resources->lifecycle;
   auto serial = boost::asio::make_strand(fixture.runtime.context());
   auto beginning = boost::asio::co_spawn(serial, lifecycle.begin(lifecycle_operation::shutdown),
                                          boost::asio::use_future);
   auto first = ready(beginning);
   const auto first_error = std::make_exception_ptr(forge::app::exceptions::shutdown_failed{"first attempt"});
   const auto next_error = std::make_exception_ptr(forge::app::exceptions::shutdown_failed{"next attempt"});
   auto joined = boost::asio::co_spawn(
       serial, capture(begin_and_finish(lifecycle, lifecycle_operation::shutdown)), boost::asio::use_future);
   auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
   ready(barrier);
   auto retrying = boost::asio::co_spawn(serial, fail_then_retry(lifecycle, std::move(first), first_error, next_error),
                                         boost::asio::use_future);
   ready(retrying);
   BOOST_CHECK(ready(joined) == first_error);
   const auto current = lifecycle.snapshot();
   BOOST_REQUIRE(current.failure);
   BOOST_CHECK(current.failure->exception == next_error);
   BOOST_CHECK(current.phase == lifecycle_phase::stopping);
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_retires_previous_cleanup_exception_outside_lock) {
   auto fixture = plugin_fixture{};
   auto lifecycle = fixture.plugin.resources->lifecycle;
   auto destructions = std::atomic_int{0};
   auto probe = std::make_shared<retired_exception_probe>(lifecycle.observer(), destructions);
   auto first_error = std::make_exception_ptr(reentrant_cleanup_error{probe});
   auto failed = boost::asio::co_spawn(fixture.runtime.context(), finish_failed_cleanup(lifecycle, first_error),
                                       boost::asio::use_future);
   auto first_result = ready(failed);
   BOOST_REQUIRE(first_result);
   first_result = {};
   first_error = {};
   probe.reset();
   BOOST_CHECK_EQUAL(destructions.load(), 0);

   const auto next_error = std::make_exception_ptr(forge::app::exceptions::shutdown_failed{"retry"});
   auto replacing = boost::asio::co_spawn(fixture.runtime.context(), finish_failed_cleanup(lifecycle, next_error),
                                          boost::asio::use_future);
   BOOST_CHECK(ready(replacing) == next_error);
   BOOST_CHECK_EQUAL(destructions.load(), 1);
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_canceled_shutdown_joiner_does_not_cancel_shared_cleanup) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   const auto owner = fixture.plugin.resources;
   owner->shutdown.blocked.store(true);
   const auto entered = owner->shutdown.entered.epoch();
   auto serial = boost::asio::make_strand(fixture.runtime.context());
   auto cleanup = boost::asio::co_spawn(serial, capture(fixture.plugin.shutdown()), boost::asio::use_future);
   fixture.wait_entered(owner->shutdown, entered);
   auto cancel = boost::asio::cancellation_signal{};
   auto joined = boost::asio::co_spawn(
       serial, capture(fixture.plugin.shutdown()),
       boost::asio::bind_cancellation_slot(cancel.slot(), boost::asio::use_future));
   auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
   ready(barrier);
   boost::asio::post(serial, [&cancel] { cancel.emit(boost::asio::cancellation_type::terminal); });
   const auto canceled = ready(joined);
   BOOST_REQUIRE(canceled);
   BOOST_CHECK(is_error<boost::system::system_error>(canceled));
   BOOST_CHECK(cleanup.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(owner->shutdowns.load(), 1);
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopping);
   owner->shutdown.release.notify();
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_cleanup_owner_is_not_canceled_while_draining_startup) {
   auto fixture = plugin_fixture{};
   fixture.initialize();
   const auto owner = fixture.plugin.resources;
   owner->startup.blocked.store(true);
   const auto entered = owner->startup.entered.epoch();
   auto startup = fixture.run(fixture.plugin.startup());
   fixture.wait_entered(owner->startup, entered);

   auto serial = boost::asio::make_strand(fixture.runtime.context());
   auto cancel = boost::asio::cancellation_signal{};
   auto cleanup = boost::asio::co_spawn(
       serial, capture(fixture.plugin.shutdown()),
       boost::asio::bind_cancellation_slot(cancel.slot(), boost::asio::use_future));
   auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
   ready(barrier);
   boost::asio::post(serial, [&cancel] { cancel.emit(boost::asio::cancellation_type::terminal); });
   BOOST_CHECK(cleanup.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   owner->startup.release.notify();
   BOOST_CHECK(is_error<forge::app::exceptions::invalid_state>(ready(startup)));
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK_EQUAL(owner->shutdowns.load(), 1);
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_observer_handles_concurrent_waits_cancel_deadline_and_terminal_epochs) {
   auto fixture = plugin_fixture{};
   const auto observer = fixture.plugin.lifecycle();
   const auto epoch = observer.snapshot().epoch;
   auto waits = std::vector<std::future<forge::app::lifecycle_snapshot>>{};
   for (auto index = 0; index < 16; ++index) {
      waits.push_back(boost::asio::co_spawn(
          fixture.runtime.context(), observer.async_wait_until(epoch, std::chrono::steady_clock::now() + test_timeout),
          boost::asio::use_future));
   }
   fixture.plugin.request_stop();
   for (auto& wait : waits) {
      const auto state = ready(wait);
      BOOST_CHECK_NE(state.epoch, epoch);
      BOOST_CHECK(state.phase == lifecycle_phase::stopping);
      BOOST_CHECK(!state.admission_open);
   }

   const auto stopped_epoch = observer.snapshot().epoch;
   auto cancel = std::stop_source{};
   auto canceled_wait = boost::asio::co_spawn(
       fixture.runtime.context(), observer.async_wait(stopped_epoch, cancel.get_token()), boost::asio::use_future);
   cancel.request_stop();
   BOOST_REQUIRE(canceled_wait.wait_for(test_timeout) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(canceled_wait.get()), boost::system::system_error, [](const auto& error) {
      return error.code() == boost::asio::error::operation_aborted;
   });

   auto deadline = boost::asio::co_spawn(
       fixture.runtime.context(), observer.async_wait_until(stopped_epoch, std::chrono::steady_clock::now()),
       boost::asio::use_future);
   BOOST_REQUIRE(deadline.wait_for(test_timeout) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(deadline.get()), boost::system::system_error, [](const auto& error) {
      return error.code() == boost::asio::error::timed_out;
   });

   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
   const auto final = observer.snapshot();
   auto terminal = boost::asio::co_spawn(fixture.runtime.context(), observer.async_wait(final.epoch),
                                         boost::asio::use_future);
   BOOST_CHECK(ready(terminal).phase == lifecycle_phase::stopped);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_observer_terminal_completion_wins_racing_cancellation) {
   auto fixture = plugin_fixture{};
   auto serial = boost::asio::make_strand(fixture.runtime.context());
   for (auto attempt = 0; attempt < 32; ++attempt) {
      auto lifecycle = plugin_lifecycle{};
      auto beginning = boost::asio::co_spawn(
          serial, lifecycle.begin(lifecycle_operation::shutdown), boost::asio::use_future);
      auto cleanup = ready(beginning);
      const auto observer = lifecycle.observer();
      const auto observed = observer.snapshot().epoch;
      auto stop = std::stop_source{};
      auto signal = boost::asio::cancellation_signal{};
      auto waiting = attempt % 2 == 0
          ? boost::asio::co_spawn(serial, observer.async_wait(observed, stop.get_token()), boost::asio::use_future)
          : boost::asio::co_spawn(serial, observer.async_wait(observed),
                                 boost::asio::bind_cancellation_slot(signal.slot(), boost::asio::use_future));
      auto barrier = boost::asio::co_spawn(serial, executor_barrier(), boost::asio::use_future);
      ready(barrier);
      // Serial delivery queues the canceled wait's continuation until after
      // terminal publication, testing cancellation followed by terminal success.
      auto completing = boost::asio::co_spawn(
          serial, cancel_then_complete(std::move(cleanup), attempt % 2 == 0 ? &stop : nullptr, &signal),
          boost::asio::use_future);
      ready(completing);
      BOOST_CHECK(ready(waiting).phase == lifecycle_phase::stopped);
   }
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_observer_outlives_owner_without_fabricating_cleanup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = std::make_unique<controlled_plugin>();
   const auto observer = plugin->lifecycle();
   const auto epoch = observer.snapshot().epoch;
   auto waiting = boost::asio::co_spawn(
       runtime.context(), observer.async_wait_until(epoch, std::chrono::steady_clock::now() + test_timeout),
       boost::asio::use_future);
   plugin.reset();
   const auto destroyed = ready(waiting);
   BOOST_CHECK(!destroyed.owner_alive);
   BOOST_CHECK(!destroyed.admission_open);
   BOOST_CHECK(destroyed.phase == lifecycle_phase::stopping);
   BOOST_CHECK(!destroyed.completed(lifecycle_operation::shutdown));
   auto late = boost::asio::co_spawn(runtime.context(), observer.async_wait(destroyed.epoch),
                                     boost::asio::use_future);
   BOOST_CHECK(!ready(late).owner_alive);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_logging_identity_failure_cannot_veto_transitions_or_cleanup) {
   auto fixture = plugin_fixture{};
   fixture.plugin.resources->fail_identity.store(true);
   fixture.initialize();
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_REQUIRE(!ready(startup));
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
   BOOST_CHECK_EQUAL(fixture.plugin.resources->shutdowns.load(), 1);
}

BOOST_AUTO_TEST_CASE(plugin_lifecycle_throwing_log_sink_cannot_veto_cleanup_or_leak_exception_text) {
   auto fixture = plugin_fixture{};
   auto sinks = lifecycle_sink_registration{};
   fixture.initialize();
   fixture.plugin.resources->fail_startup.store(true);
   auto startup = fixture.run(fixture.plugin.startup());
   BOOST_CHECK(is_error<forge::app::exceptions::startup_failed>(ready(startup)));
   auto cleanup = fixture.run(fixture.plugin.shutdown());
   BOOST_REQUIRE(!ready(cleanup));
   BOOST_CHECK(fixture.plugin.lifecycle().snapshot().phase == lifecycle_phase::stopped);
   BOOST_CHECK_GT(sinks.throwing->calls.load(), 0);
   const auto lock = std::scoped_lock{sinks.captured->mutex};
   BOOST_REQUIRE(!sinks.captured->records.empty());
   for (const auto& record : sinks.captured->records) {
      BOOST_CHECK(record.message.find("sink-secret") == std::string::npos);
      BOOST_CHECK(record.message.find("late startup failure") == std::string::npos);
   }
}
