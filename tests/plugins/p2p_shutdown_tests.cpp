#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.descriptor;
import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.db.blob.store;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.net.p2p.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.identity;
import forge.plugins.db.store.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.plugin;

namespace {
namespace core = forge::db::core;
namespace db = forge::plugins::db::store;
namespace np = forge::plugins::net::p2p::node;
using namespace std::chrono_literals;

constexpr auto store_name = "shutdown-peer-state";

// The existing DB Store test-backend pattern, with real ObjectDB transactions.
// Only the flush boundary is faulted; plugin/native shutdown is never replaced.
struct key_less {
   bool operator()(const core::record_key& left, const core::record_key& right) const {
      return left.bytes() < right.bytes();
   }
};
using records = std::map<std::string, std::map<core::record_key, std::vector<std::byte>, key_less>>;
struct backend_state {
   std::mutex mutex;
   records committed;
};

class backend_session final : public core::session {
 public:
   backend_session(std::shared_ptr<backend_state> state, bool writes) : state_(std::move(state)), writes_(writes) {
      const auto lock = std::scoped_lock{state_->mutex};
      working_ = state_->committed;
   }
   core::capabilities capabilities() const noexcept override {
      return {.snapshot_reads = !writes_, .writes = writes_, .savepoints = writes_, .record_locks = writes_};
   }
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(core::family family, core::record_key key) override {
      const auto found = working_[family.name].find(key);
      if (found == working_[family.name].end()) { co_return std::nullopt; }
      co_return found->second;
   }
   boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   get_for_update(core::family family, core::record_key key) override {
      require_write();
      co_return co_await get(std::move(family), std::move(key));
   }
   boost::asio::awaitable<void> put(core::family family, core::record_key key, std::vector<std::byte> value) override {
      require_write();
      working_[family.name][std::move(key)] = std::move(value);
      co_return;
   }
   boost::asio::awaitable<void> erase(core::family family, core::record_key key) override {
      require_write();
      working_[family.name].erase(key);
      co_return;
   }
   boost::asio::awaitable<core::record_page> scan_page(core::family family, core::record_range range,
                                                     core::page_request request) override {
      auto out = core::record_page{};
      auto& rows = working_[family.name];
      auto current = rows.lower_bound(request.after ? request.after->boundary : range.begin);
      if (request.after && current != rows.end() && current->first == request.after->boundary) { ++current; }
      while (current != rows.end() && (!range.has_end || current->first.bytes() < range.end.bytes())) {
         out.entries.push_back({.key = current->first, .value = current->second});
         ++current;
         if (out.entries.size() == request.limit) { break; }
      }
      if (!out.entries.empty() && current != rows.end() &&
          (!range.has_end || current->first.bytes() < range.end.bytes())) {
         out.next = core::cursor{.boundary = out.entries.back().key};
      }
      co_return out;
   }
   boost::asio::awaitable<void> commit() override {
      require_write();
      const auto lock = std::scoped_lock{state_->mutex};
      state_->committed = std::move(working_);
      co_return;
   }
   boost::asio::awaitable<void> rollback() override { co_return; }
   boost::asio::awaitable<void> create_savepoint() override {
      require_write(); savepoints_.push_back(working_); co_return;
   }
   boost::asio::awaitable<void> rollback_to_savepoint() override {
      require_savepoint(); working_ = std::move(savepoints_.back()); savepoints_.pop_back(); co_return;
   }
   boost::asio::awaitable<void> release_savepoint() override {
      require_savepoint(); savepoints_.pop_back(); co_return;
   }
 private:
   void require_write() const {
      if (!writes_) { throw std::logic_error{"shutdown backend snapshot is read-only"}; }
   }
   void require_savepoint() const {
      require_write();
      if (savepoints_.empty()) { throw std::logic_error{"shutdown backend has no savepoint"}; }
   }
   std::shared_ptr<backend_state> state_;
   bool writes_;
   records working_;
   std::vector<records> savepoints_;
};

class backend_driver final : public core::driver {
 public:
   boost::asio::awaitable<void> async_flush(bool) override { co_return; }
 private:
   boost::asio::awaitable<std::unique_ptr<core::session>> open_transaction() override {
      co_return std::make_unique<backend_session>(state_, true);
   }
   boost::asio::awaitable<std::unique_ptr<core::session>> open_snapshot() override {
      co_return std::make_unique<backend_session>(state_, false);
   }
   std::shared_ptr<backend_state> state_ = std::make_shared<backend_state>();
};

class owned_store_state final : public db::store_handle_state {
 public:
   explicit owned_store_state(std::shared_ptr<forge::db::object::store> objects) : objects_(std::move(objects)) {}
   std::string name() const override { return store_name; }
   std::shared_ptr<forge::db::object::store> require_objects() const override { return objects_; }
   std::shared_ptr<forge::db::blob::store> require_blobs() const override {
      return {};
   }
   boost::asio::awaitable<db::transaction> begin_transaction() const override {
      co_return db::transaction{co_await objects_->begin_transaction(), store_name};
   }
 private:
   std::shared_ptr<core::driver> require_driver() const override { return objects_->driver(); }
   std::shared_ptr<forge::db::object::store> objects_;
};

class controlled_store final : public db::api {
 public:
   std::shared_ptr<forge::db::object::store> objects;
   std::weak_ptr<owned_store_state> owner;
   forge::asio::notification released;
   bool armed = false;
   bool entered = false;
   bool release_requested = false;
   unsigned cleanup_flushes = 0;
   unsigned failed_flushes = 0;

   boost::asio::awaitable<void> add_store(std::string, std::shared_ptr<core::driver>, db::store_options) override {
      throw std::logic_error{"shutdown fixture store is already installed"}; co_return;
   }
   boost::asio::awaitable<db::store_handle> store(std::string name) override {
      if (name != store_name) { throw std::logic_error{"unexpected shutdown fixture store"}; }
      auto state = std::make_shared<owned_store_state>(objects);
      owner = state;
      co_return db::store_handle{std::move(state)};
   }
   boost::asio::awaitable<void> flush(std::string name, bool sync) override {
      if (name != store_name || !sync) { throw std::logic_error{"cleanup must flush the named store durably"}; }
      if (armed || entered) { ++cleanup_flushes; }
      if (std::exchange(armed, false)) {
         entered = true;
         for (;;) {
            const auto epoch = released.epoch();
            if (release_requested) { break; }
            co_await released.async_wait(epoch);
         }
         ++failed_flushes;
         throw std::runtime_error{"one-shot shutdown durable flush failure"};
      }
      co_await objects->driver()->async_flush(sync);
   }
   boost::asio::awaitable<void> flush_all(bool sync) override { co_await flush(store_name, sync); }
   boost::asio::awaitable<db::status> status() override { co_return db::status{}; }
   void release() { release_requested = true; released.notify(); }
};

struct shutdown_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};
   np::plugin node;
   std::shared_ptr<backend_driver> backend = std::make_shared<backend_driver>();
   std::shared_ptr<controlled_store> stores = std::make_shared<controlled_store>();
   std::shared_ptr<np::diagnostics_api> diagnostics;

   shutdown_fixture() {
      // Drive the real runtime context to quiescence ourselves. No sleeps or
      // scheduler-timing assumptions are needed to prove both callers entered.
      runtime.stop();
      runtime.context().restart();
      stores->objects = run([](std::shared_ptr<core::driver> driver)
          -> boost::asio::awaitable<std::shared_ptr<forge::db::object::store>> {
         co_return std::make_shared<forge::db::object::store>(
             co_await forge::db::object::store::open(std::move(driver)));
      }(backend));
      apis.install<db::api>(db::api::describe(), stores);
      auto config = forge::config::core::document{};
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.peer-id", forge::net::p2p::make_peer_id(
          {.type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 91)}).value);
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.peer-store.store", store_name);
      run(node.configure({config, "plugins.net.p2p.node"}));
      auto installer = forge::api::core::installer{apis};
      run(node.provide(installer));
      diagnostics = apis.get<np::diagnostics_api>(np::diagnostics_api::ref()).shared();
      run(node.initialize(context));
      run(node.after_initialize());
      run(node.startup());
   }
   ~shutdown_fixture() {
      stores->release();
      auto cleanup = start_shutdown();
      if (joined(cleanup)) { BOOST_ERROR("shutdown fixture retained a cleanup failure"); }
      diagnostics.reset();
      apis.clear();
      run(scheduler.shutdown());
      run(backend->async_close());
   }
   void settle() {
      if (runtime.context().stopped()) { runtime.context().restart(); }
      runtime.context().poll();
   }
   template<class T> T joined(std::future<T>& result) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (result.wait_for(0s) != std::future_status::ready) {
         if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "shutdown fixture failed its owned cleanup budget\n";
            std::terminate();
         }
         if (runtime.context().stopped()) { runtime.context().restart(); }
         runtime.context().run_one_until(deadline);
      }
      return result.get();
   }
   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(result);
   }
   std::future<std::exception_ptr> start_shutdown() {
      auto result = std::make_shared<std::promise<std::exception_ptr>>();
      auto future = result->get_future();
      boost::asio::co_spawn(runtime.context(), node.shutdown(),
          [result](std::exception_ptr error) { result->set_value(error); });
      return future;
   }
};
} // namespace

BOOST_AUTO_TEST_CASE(p2p_shutdown_failed_durable_cleanup_is_shared_then_external_retry_releases_owners) {
   auto f = shutdown_fixture{};
   BOOST_REQUIRE(!f.stores->owner.expired());
   BOOST_CHECK_NO_THROW((void)f.diagnostics->snapshot());
   f.stores->armed = true;

   auto first = f.start_shutdown();
   f.settle();
   BOOST_REQUIRE(f.stores->entered);
   BOOST_TEST(f.stores->cleanup_flushes == 1U);
   BOOST_CHECK(first.wait_for(0s) == std::future_status::timeout);

   auto second = f.start_shutdown();
   f.settle();
   BOOST_CHECK(second.wait_for(0s) == std::future_status::timeout);
   BOOST_TEST(f.stores->cleanup_flushes == 1U);
   BOOST_TEST(!f.stores->owner.expired());

   f.stores->release();
   auto first_error = f.joined(first);
   auto second_error = f.joined(second);
   BOOST_REQUIRE(first_error);
   BOOST_REQUIRE(second_error);
   BOOST_CHECK(first_error == second_error);
   BOOST_CHECK_THROW(std::rethrow_exception(first_error), forge::net::p2p::exceptions::durability_uncertain);
   BOOST_TEST(f.stores->failed_flushes == 1U);
   BOOST_TEST(f.stores->cleanup_flushes == 1U);
   BOOST_TEST(!f.stores->owner.expired());
   // Diagnostics retains the stopped native owner until persistence really closes.
   BOOST_CHECK_NO_THROW((void)f.diagnostics->snapshot());

   auto retry = f.start_shutdown();
   BOOST_CHECK(!f.joined(retry));
   BOOST_TEST(f.stores->cleanup_flushes == 2U);
   BOOST_TEST(f.stores->failed_flushes == 1U);
   BOOST_TEST(f.stores->owner.expired());
   BOOST_CHECK_THROW((void)f.diagnostics->snapshot(), np::exceptions::plugin_not_initialized);
   BOOST_CHECK(first_error == second_error);

   auto terminal = f.start_shutdown();
   BOOST_CHECK(!f.joined(terminal));
   BOOST_TEST(f.stores->cleanup_flushes == 2U);
}
