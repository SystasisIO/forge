#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.handle;
import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.plugin;
import forge.plugins.net.p2p.pubsub.types;
import forge.raw.raw;

namespace {

namespace p2p = forge::net::p2p;
namespace core = forge::net::p2p::pubsub;
namespace facade = forge::plugins::net::p2p::pubsub;
namespace node_plugin = forge::plugins::net::p2p::node;
using namespace std::chrono_literals;

template <typename T> T joined(std::future<T>& result) {
   if (result.wait_for(5s) != std::future_status::ready) {
      std::cerr << "PubSub lifecycle fixture failed to join an owned operation\n";
      std::terminate();
   }
   return result.get();
}

template <typename T> void drain_pending(std::future<T>& result) noexcept {
   if (!result.valid()) { return; }
   // Failure cleanup still joins; the test's normal path checks the actual result/error.
   try { (void)joined(result); }
   catch (...) {}
}

boost::asio::awaitable<void> fence() { co_return; }

facade::handler accepting() {
   return [](facade::message) -> boost::asio::awaitable<core::validation_result> {
      co_return core::validation_result::accept;
   };
}

// These barriers exercise the public source contract, not a simulated native wire verdict.
struct barrier {
   forge::asio::notification changed;
   std::mutex mutex;
   std::condition_variable entered;
   unsigned calls = 0;
   bool released = false;
   bool fail = false;

   boost::asio::awaitable<void> wait() {
      {
         const auto lock = std::scoped_lock{mutex};
         ++calls;
      }
      entered.notify_all();
      for (;;) {
         const auto epoch = changed.epoch();
         {
            const auto lock = std::scoped_lock{mutex};
            if (released) {
               if (std::exchange(fail, false)) { throw std::runtime_error{"controlled source failure"}; }
               co_return;
            }
         }
         (void)co_await changed.async_wait(epoch);
      }
   }

   void await_call(unsigned count = 1) {
      auto lock = std::unique_lock{mutex};
      if (!entered.wait_for(lock, 5s, [&] { return calls >= count; })) {
         throw std::runtime_error{"source barrier was not entered"};
      }
   }

   void release(bool failure = false) {
      {
         const auto lock = std::scoped_lock{mutex};
         released = true;
         fail = failure;
      }
      changed.notify();
   }
};

class controlled_source final : public node_plugin::pubsub_source {
 public:
   mutable std::mutex mutex;
   std::map<std::string, std::shared_ptr<core::handler>> handlers;
   std::map<std::string, std::shared_ptr<barrier>> joins;
   std::map<std::string, std::shared_ptr<barrier>> leaves;
   std::map<std::string, unsigned> join_calls;
   std::map<std::string, unsigned> leave_calls;
   std::shared_ptr<barrier> publishing;
   std::function<void()> snapshot_hook;
   std::vector<std::string> order;
   std::atomic_uint active{0};

   void enable(core::options) override {}
   p2p::peer_id local_peer() const override { return {.value = "fixture-pubsub-owner"}; }

   boost::asio::awaitable<core::message> async_publish_message(
       core::topic topic, std::vector<std::uint8_t> bytes, core::publish_options) override {
      ++active;
      auto completed = boost::scope::scope_exit{[&] { --active; }};
      auto hold = std::shared_ptr<barrier>{};
      {
         const auto lock = std::scoped_lock{mutex};
         hold = publishing;
      }
      if (hold) { co_await hold->wait(); }
      co_return core::message{.data = std::move(bytes), .subject = std::move(topic)};
   }

   boost::asio::awaitable<core::subscription> async_join_topic(core::topic topic, core::handler callback) override {
      ++active;
      auto completed = boost::scope::scope_exit{[&] { --active; }};
      auto hold = std::shared_ptr<barrier>{};
      auto owned = std::make_shared<core::handler>(std::move(callback));
      {
         const auto lock = std::scoped_lock{mutex};
         ++join_calls[topic.value];
         order.push_back("join:" + topic.value);
         handlers[topic.value] = std::move(owned);
         if (const auto found = joins.find(topic.value); found != joins.end()) { hold = found->second; }
      }
      // Deliberately mutate before the await: cancellation must compensate, not assume absence.
      if (hold) { co_await hold->wait(); }
      co_return core::subscription{.subject = std::move(topic)};
   }

   boost::asio::awaitable<void> async_leave_topic(core::topic topic) override {
      ++active;
      auto completed = boost::scope::scope_exit{[&] { --active; }};
      auto hold = std::shared_ptr<barrier>{};
      {
         const auto lock = std::scoped_lock{mutex};
         ++leave_calls[topic.value];
         order.push_back("leave:" + topic.value);
         if (const auto found = leaves.find(topic.value); found != leaves.end()) { hold = found->second; }
      }
      if (hold) { co_await hold->wait(); }
      auto retired = std::shared_ptr<core::handler>{};
      {
         const auto lock = std::scoped_lock{mutex};
         if (const auto found = handlers.find(topic.value); found != handlers.end()) {
            retired = std::move(found->second);
            handlers.erase(found);
         }
      }
   }

   core::snapshot snapshot() const override {
      auto result = core::snapshot{};
      auto hook = std::function<void()>{};
      {
         const auto lock = std::scoped_lock{mutex};
         result.topics = handlers.size();
         hook = snapshot_hook;
      }
      if (hook) { hook(); }
      return result;
   }

   boost::asio::awaitable<core::validation_result> deliver(core::topic topic, std::vector<std::uint8_t> bytes = {1}) {
      auto callback = std::shared_ptr<core::handler>{};
      {
         const auto lock = std::scoped_lock{mutex};
         callback = handlers.at(topic.value);
      }
      co_return co_await (*callback)(core::event{
          .source = local_peer(), .value = {.data = std::move(bytes), .subject = std::move(topic)}});
   }

   unsigned count(bool join, const std::string& topic) const {
      const auto lock = std::scoped_lock{mutex};
      const auto& values = join ? join_calls : leave_calls;
      const auto found = values.find(topic);
      return found == values.end() ? 0U : found->second;
   }
};

struct fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 3}};
   forge::asio::task::scheduler scheduler{runtime};
   boost::asio::strand<boost::asio::io_context::executor_type> executor{boost::asio::make_strand(runtime.context())};
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};
   std::shared_ptr<controlled_source> source = std::make_shared<controlled_source>();
   std::unique_ptr<facade::plugin> owner = std::make_unique<facade::plugin>();
   std::shared_ptr<facade::api> api;
   std::vector<std::shared_ptr<barrier>> barriers;
   bool expected_shutdown_error = false;

   explicit fixture(unsigned max_topics = 16, unsigned max_handlers = 8) {
      auto config = forge::config::core::document{};
      config.set("plugins.net.p2p.pubsub.max-topics", max_topics);
      config.set("plugins.net.p2p.pubsub.max-handlers-per-topic", max_handlers);
      run(owner->configure({config, "plugins.net.p2p.pubsub"}));
      apis.install<node_plugin::pubsub_source>(source);
      auto provider = forge::api::core::installer{apis};
      run(owner->provide(provider));
      run(owner->initialize(context));
      run(owner->startup());
      api = apis.get<facade::api>({.id = {"forge.plugins.net.p2p.pubsub"}, .major = 1}).shared();
   }

   ~fixture() {
      for (const auto& hold : barriers) { hold->release(); }
      if (owner) {
         owner->request_stop();
         try {
            run(owner->shutdown());
            if (expected_shutdown_error) { BOOST_ERROR("expected sticky shutdown failure was lost"); }
         } catch (const std::exception& error) {
            if (!expected_shutdown_error) { BOOST_ERROR("PubSub lifecycle cleanup: " << error.what()); }
         }
      }
      BOOST_TEST(source->active.load() == 0U);
      api.reset();
      apis.clear();
      owner.reset();
      run(scheduler.shutdown());
   }

   template <typename T> std::future<T> start(boost::asio::awaitable<T> operation) {
      return boost::asio::co_spawn(executor, std::move(operation), boost::asio::use_future);
   }
   template <typename T> T run(boost::asio::awaitable<T> operation) {
      auto result = start(std::move(operation));
      return joined(result);
   }
   void settle() { run(fence()); }

   std::shared_ptr<barrier> hold(bool join, const std::string& topic) {
      auto result = std::make_shared<barrier>();
      barriers.push_back(result);
      const auto lock = std::scoped_lock{source->mutex};
      (join ? source->joins : source->leaves)[topic] = result;
      return result;
   }

   std::shared_ptr<barrier> callback_hold() {
      auto result = std::make_shared<barrier>();
      barriers.push_back(result);
      return result;
   }
};

struct destruction_barrier {
   std::mutex mutex;
   std::condition_variable changed;
   bool entered = false;
   bool released = false;

   void wait() noexcept {
      auto lock = std::unique_lock{mutex};
      entered = true;
      changed.notify_all();
      if (!changed.wait_for(lock, 5s, [&] { return released; })) { std::terminate(); }
   }

   void await_entry() {
      auto lock = std::unique_lock{mutex};
      if (!changed.wait_for(lock, 5s, [&] { return entered; })) {
         throw std::runtime_error{"capture destructor was not entered"};
      }
   }

   void release() noexcept {
      const auto lock = std::scoped_lock{mutex};
      released = true;
      changed.notify_all();
   }
};

struct retiring_capture {
   std::shared_ptr<destruction_barrier> barrier;
   explicit retiring_capture(std::shared_ptr<destruction_barrier> value) : barrier{std::move(value)} {}
   ~retiring_capture() { barrier->wait(); }
};

// Only the return from the real source's join is held, after its native mutation.
class held_native_source final : public node_plugin::pubsub_source {
 public:
   std::shared_ptr<node_plugin::pubsub_source> native;
   std::shared_ptr<barrier> joined_topic;
   std::shared_ptr<barrier> leaving_topic;
   explicit held_native_source(std::shared_ptr<node_plugin::pubsub_source> value) : native{std::move(value)} {}
   void enable(core::options value) override { native->enable(std::move(value)); }
   p2p::peer_id local_peer() const override { return native->local_peer(); }
   core::snapshot snapshot() const override { return native->snapshot(); }
   boost::asio::awaitable<core::message> async_publish_message(
       core::topic subject, std::vector<std::uint8_t> bytes, core::publish_options options) override {
      co_return co_await native->async_publish_message(std::move(subject), std::move(bytes), options);
   }
   boost::asio::awaitable<core::subscription> async_join_topic(core::topic subject, core::handler callback) override {
      auto value = co_await native->async_join_topic(std::move(subject), std::move(callback));
      if (joined_topic) { co_await joined_topic->wait(); }
      co_return value;
   }
   boost::asio::awaitable<void> async_leave_topic(core::topic subject) override {
      if (leaving_topic) { co_await leaving_topic->wait(); }
      co_await native->async_leave_topic(std::move(subject));
   }
};

struct native_source_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 3}};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry native_apis;
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};
   node_plugin::plugin node;
   facade::plugin pubsub;
   std::shared_ptr<held_native_source> source;
   std::shared_ptr<facade::api> api;

   native_source_fixture() {
      auto config = forge::config::core::document{};
      const auto peer = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
                                           .data = std::vector<std::uint8_t>(32, 74)});
      // Real local node and official source, no sockets or authentication claim.
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.peer-id", peer.value);
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      run(node.configure({config, "plugins.net.p2p.node"}));
      run(pubsub.configure({config, "plugins.net.p2p.pubsub"}));
      auto native_provider = forge::api::core::installer{native_apis};
      auto provider = forge::api::core::installer{apis};
      run(node.provide(native_provider));
      source = std::make_shared<held_native_source>(native_apis.get<node_plugin::pubsub_source>(
          {.id = {"forge.plugins.net.p2p.node.pubsub_source"}, .major = 1}).shared());
      apis.install<node_plugin::pubsub_source>(source);
      run(pubsub.provide(provider));
      run(node.initialize(context));
      run(pubsub.initialize(context));
      run(node.after_initialize());
      run(node.startup());
      run(pubsub.startup());
      api = apis.get<facade::api>({.id = {"forge.plugins.net.p2p.pubsub"}, .major = 1}).shared();
   }

   ~native_source_fixture() {
      if (source->joined_topic) { source->joined_topic->release(); }
      if (source->leaving_topic) { source->leaving_topic->release(); }
      node.request_stop();
      pubsub.request_stop();
      run(pubsub.shutdown());
      run(node.shutdown());
      run(scheduler.shutdown());
   }

   template <typename T> std::future<T> start(boost::asio::awaitable<T> operation) {
      return boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   }
   template <typename T> T run(boost::asio::awaitable<T> operation) {
      auto result = start(std::move(operation));
      return joined(result);
   }
};

} // namespace

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_parallel_waiters_share_join_and_other_topics_progress) {
   auto f = fixture{};
   const auto hold = f.hold(true, "one");
   auto first = f.start(f.api->subscribe({"one"}, accepting()));
   hold->await_call();
   auto second = f.start(f.api->subscribe({"one"}, accepting()));
   f.settle();
   BOOST_CHECK(first.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(second.wait_for(0ms) != std::future_status::ready);
   const auto other = f.run(f.api->subscribe({"two"}, accepting()));
   BOOST_TEST(other.id != 0U);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   hold->release();
   const auto a = joined(first);
   const auto b = joined(second);
   BOOST_TEST(a.id != b.id);
   const auto late = f.run(f.api->subscribe({"one"}, accepting()));
   BOOST_TEST(late.id != b.id);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   BOOST_TEST(f.api->subscriptions().size() == 4U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_pending_reservations_enforce_both_limits) {
   auto f = fixture{1, 2};
   const auto hold = f.hold(true, "one");
   auto first = f.start(f.api->subscribe({"one"}, accepting()));
   hold->await_call();
   auto second = f.start(f.api->subscribe({"one"}, accepting()));
   f.settle();
   BOOST_CHECK_THROW(f.run(f.api->subscribe({"one"}, accepting())), facade::exceptions::handler_limit);
   BOOST_CHECK_THROW(f.run(f.api->subscribe({"two"}, accepting())), facade::exceptions::handler_limit);
   hold->release();
   (void)joined(first);
   (void)joined(second);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   BOOST_TEST(f.source->count(true, "two") == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_failed_join_releases_waiters_and_compensates_mutation) {
   auto f = fixture{};
   const auto hold = f.hold(true, "one");
   auto first = f.start(f.api->subscribe({"one"}, accepting()));
   hold->await_call();
   auto second = f.start(f.api->subscribe({"one"}, accepting()));
   f.settle();
   hold->release(true);
   BOOST_CHECK_THROW((void)joined(first), std::runtime_error);
   BOOST_CHECK_THROW((void)joined(second), std::runtime_error);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   BOOST_TEST(f.source->count(false, "one") == 1U);
   BOOST_TEST(f.api->subscriptions().empty());
   BOOST_TEST(f.source->snapshot().topics == 0U);
   const auto retry = f.run(f.api->subscribe({"one"}, accepting()));
   BOOST_TEST(retry.id != 0U);
   BOOST_TEST(f.source->count(true, "one") == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_stop_during_join_waits_for_compensation_and_rejects_waiters) {
   auto f = fixture{};
   const auto join_hold = f.hold(true, "one");
   const auto leave_hold = f.hold(false, "one");
   auto first = f.start(f.api->subscribe({"one"}, accepting()));
   join_hold->await_call();
   auto second = f.start(f.api->subscribe({"one"}, accepting()));
   f.settle();
   f.owner->request_stop();
   auto stop_a = f.start(f.owner->shutdown());
   auto stop_b = f.start(f.owner->shutdown());
   BOOST_CHECK_THROW(f.run(f.api->publish({"other"}, {1})), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)joined(second), std::exception);
   join_hold->release();
   leave_hold->await_call();
   BOOST_CHECK(stop_a.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(stop_b.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(first.wait_for(0ms) != std::future_status::ready);
   leave_hold->release();
   BOOST_CHECK_THROW((void)joined(first), p2p::exceptions::canceled);
   joined(stop_a);
   joined(stop_b);
   BOOST_TEST(f.source->count(false, "one") == 1U);
   BOOST_TEST(f.source->active.load() == 0U);
   BOOST_TEST(f.source->snapshot().topics == 0U);
   f.run(f.owner->shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_last_leave_serializes_new_join_and_rejects_duplicate_leave) {
   auto f = fixture{};
   const auto token = f.run(f.api->subscribe({"one"}, accepting()));
   const auto hold = f.hold(false, "one");
   auto leaving = f.start(f.api->unsubscribe(token));
   hold->await_call();
   auto replacement = f.start(f.api->subscribe({"one"}, accepting()));
   f.settle();
   BOOST_CHECK_THROW(f.run(f.api->unsubscribe(token)), facade::exceptions::subscription_not_found);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   BOOST_CHECK(replacement.wait_for(0ms) != std::future_status::ready);
   hold->release();
   joined(leaving);
   const auto next = joined(replacement);
   BOOST_TEST(next.id != token.id);
   BOOST_TEST(f.source->count(true, "one") == 2U);
   BOOST_TEST(f.source->snapshot().topics == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_failed_leave_retains_token_and_blocks_rejoin_until_retry) {
   auto f = fixture{};
   const auto token = f.run(f.api->subscribe({"one"}, accepting()));
   const auto hold = f.hold(false, "one");
   hold->release(true);
   BOOST_CHECK_THROW(f.run(f.api->unsubscribe(token)), std::runtime_error);
   const auto subscriptions = f.api->subscriptions();
   BOOST_REQUIRE_EQUAL(subscriptions.size(), 1U);
   BOOST_TEST(subscriptions.front().id == token.id);
   BOOST_CHECK_THROW(f.run(f.api->subscribe({"one"}, accepting())), std::runtime_error);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   f.run(f.api->unsubscribe(token));
   BOOST_TEST(f.source->count(false, "one") == 2U);
   BOOST_TEST(f.api->subscriptions().empty());
   (void)f.run(f.api->subscribe({"one"}, accepting()));
   BOOST_TEST(f.source->count(true, "one") == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_canceled_mutating_join_compensates_before_return) {
   auto f = fixture{};
   const auto join_hold = f.hold(true, "one");
   const auto leave_hold = f.hold(false, "one");
   auto cancellation = boost::asio::cancellation_signal{};
   auto result = std::future<facade::subscription>{};
   auto cleanup = boost::scope::scope_exit{[&]() noexcept {
      join_hold->release();
      leave_hold->release();
      drain_pending(result);
   }};
   result = boost::asio::co_spawn(f.executor, f.api->subscribe({"one"}, accepting()),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   join_hold->await_call();
   auto cancel = [&]() -> boost::asio::awaitable<void> {
      cancellation.emit(boost::asio::cancellation_type::all);
      co_return;
   };
   f.run(cancel());
   leave_hold->await_call();
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   leave_hold->release();
   BOOST_CHECK_THROW((void)joined(result), std::exception);
   BOOST_TEST(f.source->snapshot().topics == 0U);
   BOOST_TEST(f.api->subscriptions().empty());
   BOOST_TEST(f.source->count(false, "one") == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_typed_arguments_are_owned_at_call_time) {
   auto f = fixture{};
   auto pending = std::optional<boost::asio::awaitable<facade::message>>{};
   const auto expected = std::uint32_t{481};
   {
      auto value = expected;
      pending.emplace(f.api->publish({"typed"}, value));
      value = 999;
   }
   const auto result = f.run(std::move(*pending));
   BOOST_TEST(forge::raw::unpack<std::uint32_t>(result.data) == expected);
   auto called = std::make_shared<std::atomic_uint>(0);
   auto subscribing = f.api->subscribe<std::uint32_t>({"typed"},
       facade::typed_handler<std::uint32_t>{[called](facade::typed_message<std::uint32_t> value)
           -> boost::asio::awaitable<core::validation_result> {
          *called = value.value;
          co_return core::validation_result::accept;
       }});
   const auto token = f.run(std::move(subscribing));
   const auto packed = forge::raw::pack(expected);
   BOOST_CHECK(f.run(f.source->deliver({"typed"}, {packed.begin(), packed.end()})) == core::validation_result::accept);
   BOOST_TEST(called->load() == expected);
   f.run(f.api->unsubscribe(token));
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_canceled_waiter_releases_only_its_reservation) {
   auto f = fixture{1, 2};
   const auto hold = f.hold(true, "one");
   auto cancellation = boost::asio::cancellation_signal{};
   auto leader = std::future<facade::subscription>{};
   auto waiter = std::future<facade::subscription>{};
   auto replacement = std::future<facade::subscription>{};
   auto cleanup = boost::scope::scope_exit{[&]() noexcept {
      hold->release();
      drain_pending(leader);
      drain_pending(waiter);
      drain_pending(replacement);
   }};
   leader = f.start(f.api->subscribe({"one"}, accepting()));
   hold->await_call();
   waiter = boost::asio::co_spawn(f.executor, f.api->subscribe({"one"}, accepting()),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   f.settle();
   auto cancel = [&]() -> boost::asio::awaitable<void> {
      cancellation.emit(boost::asio::cancellation_type::all);
      co_return;
   };
   f.run(cancel());
   BOOST_CHECK_THROW((void)joined(waiter), std::exception);
   replacement = f.start(f.api->subscribe({"one"}, accepting()));
   f.settle();
   BOOST_CHECK(replacement.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(f.source->count(false, "one") == 0U);
   hold->release();
   (void)joined(leader);
   (void)joined(replacement);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   BOOST_TEST(f.api->subscriptions().size() == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_cancel_granted_join_keeps_pending_waiter_independent) {
   auto f = fixture{1, 2};
   const auto join_hold = f.hold(true, "one");
   const auto leave_hold = f.hold(false, "one");
   auto leader_cancel = boost::asio::cancellation_signal{};
   auto waiter_cancel = boost::asio::cancellation_signal{};
   auto leader = std::future<facade::subscription>{};
   auto waiter = std::future<facade::subscription>{};
   auto cleanup = boost::scope::scope_exit{[&]() noexcept {
      join_hold->release();
      leave_hold->release();
      drain_pending(leader);
      drain_pending(waiter);
   }};
   leader = boost::asio::co_spawn(f.executor, f.api->subscribe({"one"}, accepting()),
       boost::asio::bind_cancellation_slot(leader_cancel.slot(), boost::asio::use_future));
   join_hold->await_call(); // The first ticket is already held and the source has mutated.
   waiter = boost::asio::co_spawn(f.executor, f.api->subscribe({"one"}, accepting()),
       boost::asio::bind_cancellation_slot(waiter_cancel.slot(), boost::asio::use_future));
   f.settle();
   auto cancel_leader = [&]() -> boost::asio::awaitable<void> {
      leader_cancel.emit(boost::asio::cancellation_type::all);
      co_return;
   };
   f.run(cancel_leader());
   leave_hold->await_call();
   f.settle();
   BOOST_CHECK(leader.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(waiter.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(f.source->count(true, "one") == 1U);
   BOOST_TEST(f.source->count(false, "one") == 1U);
   auto cancel_waiter = [&]() -> boost::asio::awaitable<void> {
      waiter_cancel.emit(boost::asio::cancellation_type::all);
      co_return;
   };
   f.run(cancel_waiter());
   BOOST_CHECK_THROW((void)joined(waiter), std::exception);
   BOOST_CHECK(leader.wait_for(0ms) != std::future_status::ready);
   leave_hold->release();
   BOOST_CHECK_THROW((void)joined(leader), std::exception);
   BOOST_TEST(f.source->snapshot().topics == 0U);
   BOOST_TEST(f.api->subscriptions().empty());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_source_snapshot_can_reenter_facade_outside_mutex) {
   auto f = fixture{};
   (void)f.run(f.api->subscribe({"one"}, accepting()));
   const auto weak = std::weak_ptr<facade::api>{f.api};
   auto observed = std::atomic_size_t{0};
   {
      const auto lock = std::scoped_lock{f.source->mutex};
      f.source->snapshot_hook = [weak, &observed] {
         const auto api = weak.lock();
         observed = api->subscriptions().size();
      };
   }
   auto cleanup = boost::scope::scope_exit{[&] {
      const auto lock = std::scoped_lock{f.source->mutex};
      f.source->snapshot_hook = {};
   }};
   auto inspect = [api = f.api]() -> boost::asio::awaitable<facade::snapshot> {
      co_return api->snapshot();
   };
   const auto snapshot = f.run(inspect());
   BOOST_TEST(snapshot.core.topics == 1U);
   BOOST_TEST(observed.load() == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_saved_awaitables_outlive_facade_and_plugin) {
   auto publish = std::optional<boost::asio::awaitable<facade::message>>{};
   auto typed_publish = std::optional<boost::asio::awaitable<facade::message>>{};
   auto subscribe = std::optional<boost::asio::awaitable<facade::subscription>>{};
   auto typed_subscribe = std::optional<boost::asio::awaitable<facade::subscription>>{};
   auto unsubscribe = std::optional<boost::asio::awaitable<void>>{};
   {
      auto f = fixture{};
      const auto token = f.run(f.api->subscribe({"one"}, accepting()));
      publish.emplace(f.api->publish({"one"}, {1}));
      typed_publish.emplace(f.api->publish({"one"}, std::uint32_t{73}));
      subscribe.emplace(f.api->subscribe({"two"}, accepting()));
      typed_subscribe.emplace(f.api->subscribe<std::uint32_t>({"three"},
          facade::typed_handler<std::uint32_t>{[](facade::typed_message<std::uint32_t>)
              -> boost::asio::awaitable<core::validation_result> {
             co_return core::validation_result::accept;
          }}));
      unsubscribe.emplace(f.api->unsubscribe(token));
   }
   auto runtime = forge::asio::runtime{};
   const auto check = [&]<typename T>(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      BOOST_CHECK_THROW((void)joined(result), p2p::exceptions::canceled);
   };
   check(std::move(*publish));
   check(std::move(*typed_publish));
   check(std::move(*subscribe));
   check(std::move(*typed_subscribe));
   check(std::move(*unsubscribe));
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_handler_can_unsubscribe_itself_without_waiting_for_itself) {
   auto f = fixture{};
   auto token = std::make_shared<facade::subscription>();
   const auto api = f.api;
   *token = f.run(api->subscribe({"one"}, [api, token](facade::message)
       -> boost::asio::awaitable<core::validation_result> {
      co_await api->unsubscribe(*token);
      co_return core::validation_result::accept;
   }));
   BOOST_CHECK(f.run(f.source->deliver({"one"})) == core::validation_result::accept);
   BOOST_TEST(f.api->subscriptions().empty());
   BOOST_TEST(f.api->snapshot().active_handlers == 0U);
   BOOST_TEST(f.source->count(false, "one") == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_unsubscribe_preserves_handler_deadline_cancellation) {
   auto f = fixture{};
   const auto waiting = f.callback_hold();
   auto token = std::make_shared<facade::subscription>();
   auto canceled = std::make_shared<std::atomic_bool>(false);
   const auto api = f.api;
   *token = f.run(api->subscribe({"one"}, [api, token, waiting, canceled](facade::message)
       -> boost::asio::awaitable<core::validation_result> {
      co_await api->unsubscribe(*token);
      try { co_await waiting->wait(); }
      catch (...) { canceled->store(true); throw; }
      co_return core::validation_result::accept;
   }, {.handler_deadline = 100ms}));
   auto delivery = f.start(f.source->deliver({"one"}));
   auto cleanup = boost::scope::scope_exit{[&]() noexcept { waiting->release(); drain_pending(delivery); }};
   waiting->await_call();
   BOOST_CHECK(joined(delivery) == core::validation_result::retry);
   BOOST_TEST(canceled->load());
   BOOST_TEST(f.source->count(false, "one") == 1U);
   BOOST_TEST(f.api->snapshot().active_handlers == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_shutdown_waits_for_last_capture_destruction) {
   auto f = fixture{};
   const auto barrier = std::make_shared<destruction_barrier>();
   auto capture = std::make_shared<retiring_capture>(barrier);
   auto release_capture = boost::scope::scope_exit{[barrier]() noexcept { barrier->release(); }};
   const auto token = f.run(f.api->subscribe({"one"}, [capture](facade::message)
       -> boost::asio::awaitable<core::validation_result> { co_return core::validation_result::accept; }));
   capture.reset();
   auto leaving = std::future<void>{};
   auto stopping_context = boost::asio::io_context{};
   auto stopping = std::future<void>{};
   auto cleanup = boost::scope::scope_exit{[&]() noexcept {
      barrier->release();
      drain_pending(leaving);
      stopping_context.restart();
      stopping_context.run_for(5s);
      drain_pending(stopping);
   }};
   leaving = f.start(f.api->unsubscribe(token));
   barrier->await_entry();
   f.owner->request_stop();
   stopping = boost::asio::co_spawn(stopping_context, f.owner->shutdown(), boost::asio::use_future);
   // Drain every ready continuation on an independent executor while the destructor is still blocked.
   stopping_context.poll();
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(leaving.wait_for(0ms) != std::future_status::ready);
   barrier->release();
   joined(leaving);
   stopping_context.restart();
   stopping_context.run_for(5s);
   joined(stopping);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_official_source_local_leave_after_all_stop_requests) {
   auto f = native_source_fixture{};
   (void)f.run(f.api->subscribe({"one"}, accepting()));
   BOOST_TEST(f.source->snapshot().topics == 1U);
   f.node.request_stop();
   f.pubsub.request_stop();
   f.run(f.pubsub.shutdown());
   BOOST_TEST(f.source->snapshot().topics == 0U);
   f.run(f.source->async_leave_topic({"one"}));
   f.run(f.source->async_leave_topic({"one"}));
   BOOST_TEST(f.source->snapshot().topics == 0U);
   f.run(f.pubsub.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_official_source_compensates_mutating_join_after_node_stop) {
   auto f = native_source_fixture{};
   const auto hold = std::make_shared<barrier>();
   f.source->joined_topic = hold;
   auto joining = std::future<facade::subscription>{};
   auto stopping = std::future<void>{};
   auto cleanup = boost::scope::scope_exit{[&]() noexcept {
      hold->release(); drain_pending(joining); drain_pending(stopping);
   }};
   joining = f.start(f.api->subscribe({"one"}, accepting()));
   hold->await_call();
   BOOST_TEST(f.source->snapshot().topics == 1U);
   f.node.request_stop();
   f.pubsub.request_stop();
   stopping = f.start(f.pubsub.shutdown());
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   hold->release(true);
   BOOST_CHECK_THROW((void)joined(joining), std::runtime_error);
   joined(stopping);
   BOOST_TEST(f.source->snapshot().topics == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_official_source_stop_racing_admitted_leave) {
   auto f = native_source_fixture{};
   const auto token = f.run(f.api->subscribe({"one"}, accepting()));
   const auto hold = std::make_shared<barrier>();
   f.source->leaving_topic = hold;
   auto leaving = std::future<void>{};
   auto cleanup = boost::scope::scope_exit{[&]() noexcept { hold->release(); drain_pending(leaving); }};
   leaving = f.start(f.api->unsubscribe(token));
   hold->await_call();
   f.node.request_stop();
   hold->release();
   joined(leaving);
   BOOST_TEST(f.source->snapshot().topics == 0U);
   BOOST_TEST(f.api->subscriptions().empty());
   f.pubsub.request_stop();
   f.run(f.pubsub.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_shutdown_joins_running_handler_and_publish) {
   auto f = fixture{};
   const auto callback = f.callback_hold();
   (void)f.run(f.api->subscribe({"one"}, [callback](facade::message)
       -> boost::asio::awaitable<core::validation_result> {
      co_await callback->wait();
      co_return core::validation_result::accept;
   }));
   const auto publisher = f.callback_hold();
   {
      const auto lock = std::scoped_lock{f.source->mutex};
      f.source->publishing = publisher;
   }
   auto delivery = f.start(f.source->deliver({"one"}));
   callback->await_call();
   auto publication = f.start(f.api->publish({"one"}, {1}));
   publisher->await_call();
   f.owner->request_stop();
   auto stopping = f.start(f.owner->shutdown());
   f.settle();
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(f.source->count(false, "one") == 0U);
   publisher->release();
   (void)joined(publication);
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   callback->release();
   BOOST_CHECK(joined(delivery) == core::validation_result::accept);
   joined(stopping);
   BOOST_TEST(f.source->active.load() == 0U);
   BOOST_TEST(f.source->count(false, "one") == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_lifecycle_concurrent_shutdown_replays_actual_leave_failure) {
   auto f = fixture{};
   (void)f.run(f.api->subscribe({"one"}, accepting()));
   const auto hold = f.hold(false, "one");
   auto first = f.start(f.owner->shutdown());
   hold->await_call();
   auto second = f.start(f.owner->shutdown());
   f.settle();
   f.expected_shutdown_error = true;
   hold->release(true);
   BOOST_CHECK_THROW(joined(first), std::runtime_error);
   BOOST_CHECK_THROW(joined(second), std::runtime_error);
   BOOST_CHECK_THROW(f.run(f.owner->shutdown()), std::runtime_error);
   BOOST_TEST(f.source->count(false, "one") == 1U);
   BOOST_TEST(f.source->active.load() == 0U);
}
