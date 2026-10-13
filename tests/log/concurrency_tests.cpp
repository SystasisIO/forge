#include <boost/test/unit_test.hpp>
#include <forge/log/macros.hpp>

#include <atomic>
#include <barrier>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import forge.log.logger;
import forge.log.logger_config;
import forge.log.record;

namespace {

class concurrent_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      std::lock_guard lock(_mutex);
      _records.push_back(record);
   }

   std::vector<forge::log_record> records() const {
      std::lock_guard lock(_mutex);
      return _records;
   }

 private:
   mutable std::mutex _mutex;
   std::vector<forge::log_record> _records;
};

class blocking_sink final : public forge::sink {
 public:
   blocking_sink(std::barrier<>& entered, std::barrier<>& resume) : _entered{entered}, _resume{resume} {}

   void log(const forge::log_record&) override {
      _entered.arrive_and_wait();
      _resume.arrive_and_wait();
   }

 private:
   std::barrier<>& _entered;
   std::barrier<>& _resume;
};

class destructor_logging_sink final : public forge::sink {
 public:
   destructor_logging_sink(forge::logger cleanup, std::atomic<int>& calls)
       : _cleanup{std::move(cleanup)}, _calls{calls} {}

   ~destructor_logging_sink() override {
      _cleanup.info("sink destroyed");
   }

   void log(const forge::log_record&) override {
      _calls.fetch_add(1, std::memory_order_relaxed);
   }

 private:
   forge::logger _cleanup;
   std::atomic<int>& _calls;
};

class mutating_sink final : public forge::sink {
 public:
   mutating_sink(forge::logger route, forge::logger parent)
       : _route{std::move(route)}, _parent{std::move(parent)} {}

   void log(const forge::log_record&) override {
      ++calls;
      _route.set_name("test.reentrant.updated");
      _route.set_enabled(true);
      _route.set_log_level(forge::log_level::info);
      _route.set_parent(_parent);
      valid = _route.get_name() == "test.reentrant.updated" && _route.is_enabled() &&
              _route.get_log_level().value == forge::log_level::info &&
              _route.get_parent().get_name() == _parent.get_name();
      _route.remove_sink(self.lock());
      _route.info("nested callback");
   }

   std::weak_ptr<forge::sink> self;
   int calls = 0;
   bool valid = false;

 private:
   forge::logger _route;
   forge::logger _parent;
};

forge::logging_config quiet_config(bool enabled = true) {
   auto config = forge::logging_config{};
   auto route = forge::logger_config{"default"};
   route.enabled = enabled;
   route.level = enabled ? forge::log_level::info : forge::log_level::off;
   config.loggers.push_back(std::move(route));
   return config;
}

} // namespace

BOOST_AUTO_TEST_SUITE(log_concurrency_test_suite)

BOOST_AUTO_TEST_CASE(shared_route_setters_and_filter_reads_race_with_logging) {
   constexpr int rounds = 64;
   constexpr int batch = 32;
   auto route = forge::logger{"test.concurrent.filter"};
   route.set_log_level(forge::log_level::info);
   auto captured = std::make_shared<concurrent_capture_sink>();
   route.add_sink(captured);
   auto phases = std::barrier<>{3};
   auto invalid_levels = std::atomic<int>{0};
   auto writer = std::jthread{[&, shared = route]() mutable {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            shared.set_enabled(item % 2 == 0);
            shared.set_log_level(item % 3 == 0 ? forge::log_level::off : forge::log_level::info);
         }
         phases.arrive_and_wait();
      }
   }};
   auto reader = std::jthread{[&, shared = route]() mutable {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            auto copy = forge::logger{shared};
            static_cast<void>(copy.is_enabled());
            static_cast<void>(copy.is_enabled(forge::log_level::info));
            const auto level = copy.get_log_level().value;
            if (level != forge::log_level::info && level != forge::log_level::off) {
               invalid_levels.fetch_add(1, std::memory_order_relaxed);
            }
            copy.log(forge::log_record{.message = "racing record"});
         }
         phases.arrive_and_wait();
      }
   }};
   for (int round = 0; round < rounds; ++round) {
      phases.arrive_and_wait();
      phases.arrive_and_wait();
      route.set_enabled(true);
      route.set_log_level(forge::log_level::info);
      route.info("enabled checkpoint");
      route.set_enabled(false);
      route.info("disabled checkpoint");
      route.set_enabled(true);
      route.set_log_level(forge::log_level::off);
      route.info("off checkpoint");
   }
   writer.join();
   reader.join();
   BOOST_TEST(invalid_levels.load() == 0);
   int checkpoints = 0;
   for (const auto& record : captured->records()) {
      BOOST_TEST(record.logger == "test.concurrent.filter");
      BOOST_TEST(record.level.value == forge::log_level::info);
      BOOST_TEST(record.message != "disabled checkpoint");
      BOOST_TEST(record.message != "off checkpoint");
      checkpoints += record.message == "enabled checkpoint";
   }
   BOOST_TEST(checkpoints == rounds);
}

BOOST_AUTO_TEST_CASE(name_and_parent_changes_preserve_origin_and_deduplicate_cycles) {
   constexpr int rounds = 64;
   constexpr int batch = 32;
   const auto name_a = std::string(128, 'a');
   const auto name_b = std::string(128, 'b');
   auto first = forge::logger{"test.concurrent.parent.first"};
   auto second = forge::logger{"test.concurrent.parent.second"};
   auto route = forge::logger{name_a, first};
   route.set_log_level(forge::log_level::info);
   first.set_enabled(false);
   second.set_log_level(forge::log_level::off);
   first.set_parent(route);
   auto common = std::make_shared<concurrent_capture_sink>();
   auto parent_only = std::make_shared<concurrent_capture_sink>();
   route.add_sink(common);
   route.add_sink(common);
   first.add_sink(common);
   second.add_sink(common);
   first.add_sink(parent_only);
   second.add_sink(parent_only);
   auto phases = std::barrier<>{2};
   auto invalid_reads = std::atomic<int>{0};
   auto writer = std::jthread{[&, shared = route]() mutable {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            shared.set_name(item % 2 == 0 ? name_a : name_b);
            shared.set_parent(item % 3 == 0 ? forge::logger{nullptr} : (item % 2 == 0 ? first : second));
         }
         phases.arrive_and_wait();
      }
   }};
   auto reader = std::jthread{[&, shared = route]() mutable {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            const auto name = shared.get_name();
            if (name != name_a && name != name_b) {
               invalid_reads.fetch_add(1, std::memory_order_relaxed);
            }
            const auto parent = shared.get_parent();
            if (parent != nullptr) {
               const auto parent_name = parent.get_name();
               if (parent_name != first.get_name() && parent_name != second.get_name()) {
                  invalid_reads.fetch_add(1, std::memory_order_relaxed);
               }
            }
            shared.log(forge::log_record{.message = "parent race"});
         }
         phases.arrive_and_wait();
      }
   }};
   writer.join();
   reader.join();
   first.set_parent(nullptr);
   route.set_parent(nullptr);
   BOOST_TEST(invalid_reads.load() == 0);
   const auto common_records = common->records();
   BOOST_TEST(common_records.size() == static_cast<std::size_t>(rounds * batch));
   BOOST_TEST(parent_only->records().size() <= common_records.size());
   for (const auto& record : common_records) {
      BOOST_CHECK(record.logger == name_a || record.logger == name_b);
   }
   route.set_parent(first);
   route.info("disabled parent still routes");
   BOOST_TEST(parent_only->records().back().message == "disabled parent still routes");
   route.set_parent(second);
   route.info("off parent still routes");
   BOOST_TEST(parent_only->records().back().message == "off parent still routes");
}

BOOST_AUTO_TEST_CASE(shared_handle_replacement_keeps_each_record_on_one_live_route) {
   constexpr int rounds = 64;
   constexpr int batch = 32;
   auto first = forge::logger{"test.handle.first"};
   auto second = forge::logger{"test.handle.second"};
   first.set_log_level(forge::log_level::info);
   second.set_log_level(forge::log_level::info);
   auto first_sink = std::make_shared<concurrent_capture_sink>();
   auto second_sink = std::make_shared<concurrent_capture_sink>();
   first.add_sink(first_sink);
   second.add_sink(second_sink);
   auto handle = forge::logger{first};
   auto phases = std::barrier<>{3};
   auto null_reads = std::atomic<int>{0};
   auto writer = std::jthread{[&] {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            handle = item % 2 == 0 ? first : second;
            auto incoming = forge::logger{item % 2 == 0 ? second : first};
            handle = std::move(incoming);
         }
         phases.arrive_and_wait();
      }
   }};
   const auto read = [&] {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            if (handle == nullptr) {
               null_reads.fetch_add(1, std::memory_order_relaxed);
            }
            auto copy = forge::resolve_log_logger(handle);
            auto moved = forge::logger{std::move(copy)};
            moved.log(forge::log_record{.message = "copied handle"});
            handle.info("direct handle");
         }
         phases.arrive_and_wait();
      }
   };
   auto reader_a = std::jthread{read};
   auto reader_b = std::jthread{read};
   writer.join();
   reader_a.join();
   reader_b.join();
   BOOST_TEST(null_reads.load() == 0);
   const auto first_records = first_sink->records();
   const auto second_records = second_sink->records();
   BOOST_TEST(first_records.size() + second_records.size() == static_cast<std::size_t>(4 * rounds * batch));
   for (const auto& record : first_records) {
      BOOST_TEST(record.logger == "test.handle.first");
   }
   for (const auto& record : second_records) {
      BOOST_TEST(record.logger == "test.handle.second");
   }
}

BOOST_AUTO_TEST_CASE(configure_logging_races_with_default_reference_and_named_resolution) {
   constexpr int rounds = 32;
   constexpr int batch = 16;
   auto original = forge::logger::get();
   BOOST_REQUIRE(forge::configure_logging(quiet_config()));
   auto& default_ref = forge::logger::default_logger();
   auto phases = std::barrier<>{3};
   auto failures = std::atomic<int>{0};
   auto writer = std::jthread{[&] {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            if (!forge::configure_logging(quiet_config(item % 2 == 0))) {
               failures.fetch_add(1, std::memory_order_relaxed);
            }
         }
         phases.arrive_and_wait();
      }
   }};
   auto direct_reader = std::jthread{[&] {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            auto copy = forge::logger{default_ref};
            if (copy == nullptr || copy.get_name() != "default") {
               failures.fetch_add(1, std::memory_order_relaxed);
            }
            static_cast<void>(default_ref.is_enabled());
            static_cast<void>(default_ref.get_log_level());
            static_cast<void>(default_ref.get_parent());
            default_ref.log(forge::log_record{.message = "default reference"});
            default_ref.info("default convenience");
         }
         phases.arrive_and_wait();
      }
   }};
   auto named_reader = std::jthread{[&] {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            auto named = forge::logger::get("test.concurrent.configured");
            if (named.get_parent() == nullptr || named.get_name() != "test.concurrent.configured") {
               failures.fetch_add(1, std::memory_order_relaxed);
            }
            ilog("default macro");
            ilog(default_ref, "explicit default macro");
            named.log(forge::log_record{.message = "named resolution"});
         }
         phases.arrive_and_wait();
      }
   }};
   writer.join();
   direct_reader.join();
   named_reader.join();
   BOOST_TEST(failures.load() == 0);
   BOOST_REQUIRE(forge::configure_logging(quiet_config()));
   auto captured = std::make_shared<concurrent_capture_sink>();
   forge::logger::get().add_sink(captured);
   default_ref.info("current default reference");
   ilog("current default macro");
   BOOST_TEST(captured->records().size() == 2U);
   forge::logger::update("default", original);
}

BOOST_AUTO_TEST_CASE(copy_self_assignment_cannot_restore_a_retired_default_after_configuration) {
   constexpr int rounds = 64;
   constexpr int batch = 1024;
   auto original = forge::logger::get();
   auto old_route = forge::logger{"test.self-assignment.retired"};
   old_route.set_enabled(false);
   old_route.set_log_level(forge::log_level::off);
   auto& default_ref = forge::logger::default_logger();
   auto captured = std::make_shared<concurrent_capture_sink>();
   auto phases = std::barrier<>{4};
   auto configuration_failures = std::atomic<int>{0};
   auto publisher = std::jthread{[&] {
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         if (!forge::configure_logging(quiet_config())) {
            configuration_failures.fetch_add(1, std::memory_order_relaxed);
         }
         phases.arrive_and_wait();
      }
   }};
   const auto self_assign = [&] {
      const auto& self = default_ref;
      for (int round = 0; round < rounds; ++round) {
         phases.arrive_and_wait();
         for (int item = 0; item < batch; ++item) {
            default_ref = self;
         }
         phases.arrive_and_wait();
      }
   };
   auto first = std::jthread{self_assign};
   auto second = std::jthread{self_assign};
   int stale_rounds = 0;
   for (int round = 0; round < rounds; ++round) {
      forge::logger::update("default", old_route);
      phases.arrive_and_wait();
      phases.arrive_and_wait();

      // Both writers are quiescent. Do not reconfigure here: that would hide
      // a self-assignment store that restored the retired implementation.
      auto configured = forge::logger::get();
      if (default_ref.get_name() != "default" || !default_ref.is_enabled() ||
          default_ref.get_log_level().value != forge::log_level::info ||
          configured.get_name() != "default") {
         ++stale_rounds;
      }
      configured.add_sink(captured);
      configured.info("registry checkpoint");
      default_ref.info("default reference checkpoint");
   }
   publisher.join();
   first.join();
   second.join();
   const auto final_name = default_ref.get_name();
   forge::logger::update("default", original);

   BOOST_TEST(configuration_failures.load() == 0);
   BOOST_TEST(stale_rounds == 0);
   BOOST_TEST(final_name == "default");
   const auto records = captured->records();
   BOOST_REQUIRE_EQUAL(records.size(), static_cast<std::size_t>(2 * rounds));
   for (int round = 0; round < rounds; ++round) {
      BOOST_TEST(records[2 * round].logger == "default");
      BOOST_TEST(records[2 * round].message == "registry checkpoint");
      BOOST_TEST(records[2 * round + 1].logger == "default");
      BOOST_TEST(records[2 * round + 1].message == "default reference checkpoint");
   }
}

BOOST_AUTO_TEST_CASE(ordinary_default_macros_use_registry_not_the_mutable_default_reference) {
   auto original = forge::logger::get();
   auto registered = forge::logger{"default"};
   auto detached = forge::logger{"test.detached.default"};
   registered.set_log_level(forge::log_level::info);
   detached.set_log_level(forge::log_level::info);
   auto registered_sink = std::make_shared<concurrent_capture_sink>();
   auto detached_sink = std::make_shared<concurrent_capture_sink>();
   registered.add_sink(registered_sink);
   detached.add_sink(detached_sink);
   forge::logger::update("default", registered);
   forge::logger::default_logger() = detached;
   ilog("ordinary default");
   ilog(forge::logger::default_logger(), "explicit default reference");
   BOOST_REQUIRE_EQUAL(registered_sink->records().size(), 1U);
   BOOST_REQUIRE_EQUAL(detached_sink->records().size(), 1U);
   BOOST_TEST(registered_sink->records().front().logger == "default");
   BOOST_TEST(detached_sink->records().front().logger == "test.detached.default");
   BOOST_REQUIRE(forge::configure_logging(quiet_config()));
   registered.info("retained old route");
   ilog("new configuration has no sinks");
   BOOST_TEST(registered_sink->records().size() == 2U);
   forge::logger::update("default", original);
}

BOOST_AUTO_TEST_CASE(sink_removal_keeps_inflight_snapshot_alive_and_destroys_outside_locks) {
   auto cleanup = forge::logger{"test.removal.cleanup"};
   cleanup.set_log_level(forge::log_level::info);
   auto cleanup_sink = std::make_shared<concurrent_capture_sink>();
   cleanup.add_sink(cleanup_sink);
   auto route = forge::logger{"test.removal.origin"};
   route.set_log_level(forge::log_level::info);
   auto entered = std::barrier<>{2};
   auto resume = std::barrier<>{2};
   auto blocker = std::make_shared<blocking_sink>(entered, resume);
   auto calls = std::atomic<int>{0};
   auto removed = std::make_shared<destructor_logging_sink>(cleanup, calls);
   auto lifetime = std::weak_ptr<forge::sink>{removed};
   auto survivor = std::make_shared<concurrent_capture_sink>();
   route.add_sink(blocker);
   route.add_sink(removed);
   route.add_sink(survivor);
   route.add_sink(removed);
   auto worker = std::jthread{[&] { route.info("inflight snapshot"); }};
   entered.arrive_and_wait();
   route.remove_sink(blocker);
   route.remove_sink(removed);
   removed.reset();
   const auto retained_while_inflight = !lifetime.expired();
   route.info("after removal");
   resume.arrive_and_wait();
   worker.join();
   BOOST_TEST(retained_while_inflight);
   BOOST_TEST(lifetime.expired());
   BOOST_TEST(calls.load() == 1);
   const auto records = survivor->records();
   BOOST_REQUIRE_EQUAL(records.size(), 2U);
   BOOST_TEST(records.front().message == "after removal");
   BOOST_TEST(records.back().message == "inflight snapshot");
   BOOST_REQUIRE_EQUAL(cleanup_sink->records().size(), 1U);
   BOOST_TEST(cleanup_sink->records().front().message == "sink destroyed");
}

BOOST_AUTO_TEST_CASE(sink_callbacks_can_mutate_route_remove_themselves_and_log_reentrantly) {
   auto route = forge::logger{"test.reentrant.origin"};
   auto parent = forge::logger{"test.reentrant.parent"};
   route.set_log_level(forge::log_level::info);
   auto captured = std::make_shared<concurrent_capture_sink>();
   route.add_sink(captured);
   parent.add_sink(captured);
   for (int item = 0; item < 32; ++item) {
      route.set_name("test.reentrant.origin");
      auto callback = std::make_shared<mutating_sink>(route, parent);
      callback->self = callback;
      route.add_sink(callback);
      route.info("outer callback");
      BOOST_TEST(callback->calls == 1);
      BOOST_TEST(callback->valid);
      route.remove_sink(callback);
   }
   const auto records = captured->records();
   BOOST_REQUIRE_EQUAL(records.size(), 64U);
   for (const auto& record : records) {
      if (record.message == "outer callback") {
         BOOST_TEST(record.logger == "test.reentrant.origin");
      } else {
         BOOST_TEST(record.message == "nested callback");
         BOOST_TEST(record.logger == "test.reentrant.updated");
      }
   }
}

BOOST_AUTO_TEST_CASE(last_sink_owners_can_log_during_parent_registry_and_default_retirement) {
   auto original = forge::logger::get();
   auto cleanup = forge::logger{"test.retirement.cleanup"};
   cleanup.set_log_level(forge::log_level::info);
   auto captured = std::make_shared<concurrent_capture_sink>();
   cleanup.add_sink(captured);
   auto calls = std::atomic<int>{0};
   const auto make_retiring = [&](const std::string& name) {
      auto route = forge::logger{name};
      route.add_sink(std::make_shared<destructor_logging_sink>(cleanup, calls));
      return route;
   };

   auto child = forge::logger{"test.retirement.child", make_retiring("test.retirement.parent")};
   child.set_parent(nullptr);
   BOOST_TEST(captured->records().size() == 1U);

   auto previous = make_retiring("test.retirement.named");
   forge::logger::update("test.retirement.named", previous);
   previous = nullptr;
   auto replacement = forge::logger{"test.retirement.named"};
   forge::logger::update("test.retirement.named", replacement);
   BOOST_TEST(captured->records().size() == 2U);

   previous = make_retiring("test.retirement.configured");
   forge::logger::update("test.retirement.configured", previous);
   previous = nullptr;
   BOOST_REQUIRE(forge::configure_logging(quiet_config()));
   BOOST_TEST(captured->records().size() == 3U);

   previous = make_retiring("default");
   forge::logger::update("default", previous);
   previous = nullptr;
   BOOST_REQUIRE(forge::configure_logging(quiet_config()));
   BOOST_TEST(captured->records().size() == 4U);

   forge::logger::default_logger() = make_retiring("test.retirement.detached-default");
   BOOST_REQUIRE(forge::configure_logging(quiet_config()));
   BOOST_TEST(captured->records().size() == 5U);
   BOOST_TEST(calls.load() == 0);
   forge::logger::update("default", original);
}

BOOST_AUTO_TEST_CASE(move_assignment_remains_a_swap_and_move_construction_empties_source) {
   auto first = forge::logger{"test.move.first"};
   auto second = forge::logger{"test.move.second"};
   first = std::move(second);
   BOOST_TEST(first.get_name() == "test.move.second");
   BOOST_TEST(second.get_name() == "test.move.first");
   auto moved = forge::logger{std::move(first)};
   BOOST_CHECK(first == nullptr);
   BOOST_TEST(moved.get_name() == "test.move.second");
   auto& self = moved;
   moved = self;
   moved = std::move(self);
   BOOST_TEST(moved.get_name() == "test.move.second");
}

BOOST_AUTO_TEST_SUITE_END()
