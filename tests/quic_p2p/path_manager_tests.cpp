module;

#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/scope/scope_exit.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.asio.runtime;
import forge.multiformats.multiaddr;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.protocol;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.tcp.connection;
import forge.net.yamux.session;
import forge.net.transport.session;
import forge.net.transport.stream;

#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/direct_transport.hxx"
#include "../../libraries/net/p2p/details/coordinated_dial.hxx"
#include "../../libraries/net/p2p/details/lifecycle_wakeup.hxx"
#include "../../libraries/net/p2p/details/lifecycle_tracker.hxx"
#include "../../libraries/net/p2p/details/operation_deadline.hxx"
#include "../../libraries/net/p2p/details/owner_cancellation.hxx"
#include "../../libraries/net/p2p/details/observed_address_manager.hxx"
#include "../../libraries/net/p2p/details/path_manager.hxx"
#include "../../libraries/net/p2p/details/path_manager_dial_batch.hxx"
#include "../../libraries/net/p2p/details/stream_upgrade.hxx"

namespace {
namespace p2p = forge::net::p2p;
using manager = p2p::detail::path_manager;
using namespace std::chrono_literals;

p2p::peer_id path_peer(std::uint8_t id) {
   return p2p::make_peer_id({.type = p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, id)});
}

auto owner() { return manager{std::make_shared<p2p::detail::lifecycle_wakeup>()}; }

auto coordinated_owner(boost::asio::any_io_executor executor) {
   return std::make_shared<p2p::detail::coordinated_dial>(std::move(executor),
       p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4001"),
       p2p::node::coordinated_connect_options{.expected_peer = path_peer(1),
           .local_source = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4002"), .timeout = 10s},
       1, p2p::upgrade_role::initiator);
}

struct pending_open {
   bool entered = false;
   bool terminal = false;
   std::size_t session_cancels = 0;
};

class pending_path_session final : public forge::net::transport::detail::session_concept {
 public:
   pending_path_session(boost::asio::io_context& io, std::shared_ptr<pending_open> state)
       : timer_(io), state_(std::move(state)) {}
   bool valid() const noexcept override { return true; }
   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      state_->entered = true;
      timer_.expires_after(1h);
      auto terminal = boost::scope::scope_exit{[this] { state_->terminal = true; }};
      co_await timer_.async_wait(boost::asio::use_awaitable);
      co_return forge::net::transport::stream{};
   }
   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      co_return forge::net::transport::stream{};
   }
   boost::asio::awaitable<void> async_close() override { co_return; }
   void cancel() override { ++state_->session_cancels; }

 private:
   boost::asio::steady_timer timer_;
   std::shared_ptr<pending_open> state_;
};

struct terminal_barrier {
   forge::asio::notification changed;
   bool entered = false;
   bool released = false;
   bool terminal = false;
   bool fail_close = false;
   std::size_t closes = 0;
   std::size_t cancels = 0;
};

class terminal_path_stream final : public forge::net::transport::detail::stream_concept {
 public:
   explicit terminal_path_stream(std::shared_ptr<terminal_barrier> barrier) : barrier_(std::move(barrier)) {}
   bool valid() const noexcept override { return true; }
   std::int64_t id() const noexcept override { return 1; }
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t>) override { co_return; }
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override { co_return std::vector<std::uint8_t>{}; }
   boost::asio::awaitable<void> async_close() override {
      ++barrier_->closes;
      barrier_->entered = true;
      while (!barrier_->released) {
         const auto epoch = barrier_->changed.epoch();
         if (!barrier_->released) { co_await barrier_->changed.async_wait(epoch); }
      }
      barrier_->terminal = true;
      if (barrier_->fail_close) { throw std::runtime_error{"terminal close error"}; }
   }
   void cancel() override { ++barrier_->cancels; }

 private:
   std::shared_ptr<terminal_barrier> barrier_;
};

class terminal_path_session final : public forge::net::transport::detail::session_concept {
 public:
   terminal_path_session(std::shared_ptr<terminal_barrier> barrier, std::shared_ptr<void> native_lifetime)
       : barrier_(std::move(barrier)), native_lifetime_(std::move(native_lifetime)) {}
   bool valid() const noexcept override { return !barrier_->terminal; }
   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      co_return forge::net::transport::stream{};
   }
   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      co_return forge::net::transport::stream{};
   }
   boost::asio::awaitable<void> async_close() override {
      ++barrier_->closes;
      barrier_->entered = true;
      while (!barrier_->released) {
         const auto epoch = barrier_->changed.epoch();
         if (!barrier_->released) { co_await barrier_->changed.async_wait(epoch); }
      }
      native_lifetime_.reset();
      barrier_->terminal = true;
      if (barrier_->fail_close) { throw std::runtime_error{"native terminal close error"}; }
   }
   void cancel() override { ++barrier_->cancels; }

 private:
   std::shared_ptr<terminal_barrier> barrier_;
   std::shared_ptr<void> native_lifetime_;
};

struct waiter_allocation {
   std::size_t attempts = 0;
   bool denied = true;
};

boost::asio::awaitable<void> close_collision_exchange(std::shared_ptr<manager> value,
                                                      std::shared_ptr<manager::operation> operation,
                                                      std::shared_ptr<manager::exchange> ticket,
                                                      std::shared_ptr<p2p::detail::resource_stream> resource) {
   auto terminal = boost::scope::scope_exit{[value, operation, ticket] { value->end_exchange(operation, ticket); }};
   co_await manager::async_close_exchange(resource);
}

template <typename T>
struct waiter_allocator {
   using value_type = T;
   waiter_allocation* state;

   explicit waiter_allocator(waiter_allocation& value) : state(&value) {}
   template <typename U>
   waiter_allocator(const waiter_allocator<U>& other) : state(other.state) {}

   T* allocate(std::size_t count) {
      ++state->attempts;
      if (state->denied) { throw std::bad_alloc{}; }
      return std::allocator<T>{}.allocate(count);
   }
   void deallocate(T* value, std::size_t count) { std::allocator<T>{}.deallocate(value, count); }
   template <typename U>
   bool operator==(const waiter_allocator<U>& other) const { return state == other.state; }
};

} // namespace

BOOST_AUTO_TEST_SUITE(path_manager_tests)

BOOST_AUTO_TEST_CASE(automatic_manual_and_handler_claims_coalesce_without_extending_deadline) {
   auto value = owner();
   const auto now = manager::time_point{} + 1h;
   const auto first = value.begin(path_peer(1), 17, manager::role::initiator, now + 10s, now);
   const auto manual = value.begin(path_peer(1), 18, manager::role::responder, now + 30s, now);
   BOOST_REQUIRE(first.owner);
   BOOST_TEST(first.leader);
   BOOST_TEST(!manual.leader);
   BOOST_CHECK(manual.owner == first.owner);
   BOOST_CHECK(first.owner->deadline == now + 10s);
   BOOST_TEST(first.owner->session_id == 17U);
   BOOST_TEST(value.active() == 1U);
   BOOST_TEST(value.retained() == 1U);
}

BOOST_AUTO_TEST_CASE(exchange_admission_is_exclusive_and_exhausts_exactly_three_attempts) {
   auto value = owner();
   const auto now = manager::time_point{} + 1h;
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 10s, now).owner;
   for (auto attempt = std::size_t{}; attempt < manager::max_attempts; ++attempt) {
      BOOST_TEST(value.begin_exchange(item, now));
      BOOST_TEST(!value.begin_exchange(item, now));
      BOOST_TEST(value.inspect(item).attempts == attempt + 1);
      value.end_exchange(item, now);
   }
   BOOST_TEST(!value.begin_exchange(item, now));
   value.finish(item, p2p::hole_punch::status::failed, now);
   BOOST_TEST(!value.begin(path_peer(1), 2, manager::role::initiator, now + 10s, now).owner);
}

BOOST_AUTO_TEST_CASE(cross_circuit_handover_waits_terminal_guard_and_keeps_canonical_winner) {
   auto io = boost::asio::io_context{};
   const auto first = path_peer(1);
   const auto second = path_peer(2);
   const auto smaller = first < second ? first : second;
   const auto larger = first < second ? second : first;
   const auto winner = std::make_shared<manager>(std::make_shared<p2p::detail::lifecycle_wakeup>());
   const auto loser = std::make_shared<manager>(std::make_shared<p2p::detail::lifecycle_wakeup>());
   const auto deadline = std::chrono::steady_clock::now() + 10s;
   const auto winning_owner = winner->begin(larger, 11, manager::role::initiator, deadline).owner;
   const auto losing_owner = loser->begin(smaller, 22, manager::role::initiator, deadline).owner;
   const auto winning = winner->start_exchange(winning_owner);
   const auto losing = loser->start_exchange(losing_owner);
   BOOST_REQUIRE(winning);
   BOOST_REQUIRE(losing);
   const auto rejected = std::make_shared<manager::exchange>(
       manager::exchange{12, manager::role::responder, std::make_shared<p2p::cancellation_latch>()});
   const auto incoming = std::make_shared<manager::exchange>(
       manager::exchange{21, manager::role::responder, std::make_shared<p2p::cancellation_latch>()});
   const auto barrier = std::make_shared<terminal_barrier>();
   auto resources = p2p::resource_manager{};
   auto reservation = resources.reserve_stream(smaller, p2p::resource_manager::session_direction::outbound);
   BOOST_REQUIRE(reservation);
   const auto resource = std::make_shared<p2p::detail::resource_stream>(std::move(*reservation));
   resource->attach(
       forge::net::transport::detail::stream_access::make(std::make_shared<terminal_path_stream>(barrier)));
   auto closed = std::future<void>{};
   auto cleanup = boost::scope::scope_exit{[&] {
      barrier->released = true;
      barrier->changed.notify();
      winner->request_stop();
      loser->request_stop();
      io.restart();
      io.poll();
      if (!closed.valid()) {
         closed = boost::asio::co_spawn(io, close_collision_exchange(loser, losing_owner, losing, resource),
                                        boost::asio::use_future);
      }
      winner->end_exchange(winning_owner, winning);
      loser->end_exchange(losing_owner, incoming);
      winner->finish(winning_owner, p2p::hole_punch::status::failed);
      loser->finish(losing_owner, p2p::hole_punch::status::failed);
      io.restart();
      io.run();
   }};
   auto refused = boost::asio::co_spawn(io, winner->async_accept_exchange(winning_owner, rejected, smaller),
                                        boost::asio::use_future);
   auto adopted =
       boost::asio::co_spawn(io, loser->async_accept_exchange(losing_owner, incoming, larger), boost::asio::use_future);
   auto waiter = boost::asio::co_spawn(io, loser->async_wait(losing_owner, deadline), boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(refused.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!refused.get());
   BOOST_TEST(!winning->cancellation->stop_requested());
   BOOST_TEST(losing->cancellation->stop_requested());
   BOOST_TEST(!losing_owner->cancellation->stop_requested());
   BOOST_TEST(!incoming->cancellation->stop_requested());
   BOOST_TEST(winner->active() == 1U);
   BOOST_TEST(loser->active() == 1U);
   BOOST_CHECK(adopted.wait_for(0ms) != std::future_status::ready);
   const auto manual = loser->begin(smaller, 99, manager::role::responder, deadline + 1h);
   BOOST_CHECK(manual.owner == losing_owner);
   BOOST_TEST(!manual.leader);
   BOOST_CHECK(losing_owner->deadline == deadline);
   closed = boost::asio::co_spawn(io, close_collision_exchange(loser, losing_owner, losing, resource),
                                  boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_TEST(barrier->entered);
   BOOST_TEST(!barrier->terminal);
   BOOST_TEST(resources.current().system.outbound_streams == 1U);
   BOOST_CHECK(adopted.wait_for(0ms) != std::future_status::ready);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.poll();
   BOOST_REQUIRE(closed.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(closed.get());
   BOOST_REQUIRE(adopted.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(adopted.get());
   BOOST_TEST(barrier->terminal);
   BOOST_TEST(resources.current().system.outbound_streams == 0U);
   BOOST_TEST(loser->inspect(losing_owner).attempts == 2U);
   BOOST_TEST(loser->inspect(losing_owner).handed_over);
   BOOST_TEST(losing_owner->session_id == 22U);
   BOOST_CHECK(losing_owner->side == manager::role::initiator);
   BOOST_TEST(incoming->session_id == 21U);
   BOOST_CHECK(incoming->side == manager::role::responder);
   loser->end_exchange(losing_owner, losing); // A stale guard cannot clear the adopted ticket.
   BOOST_TEST(loser->inspect(losing_owner).exchanging);
   BOOST_TEST(!loser->seal(losing_owner, true));
   BOOST_CHECK(waiter.wait_for(0ms) != std::future_status::ready);
   winner->end_exchange(winning_owner, winning);
   loser->end_exchange(losing_owner, incoming);
   winner->finish(winning_owner, p2p::hole_punch::status::succeeded);
   loser->finish(losing_owner, p2p::hole_punch::status::succeeded);
   io.restart();
   io.poll();
   BOOST_REQUIRE(waiter.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK(waiter.get() == p2p::hole_punch::status::succeeded);
   BOOST_TEST(winner->active() == 0U);
   BOOST_TEST(loser->active() == 0U);
   BOOST_TEST(winner->retained() == 0U);
   BOOST_TEST(loser->retained() == 0U);
}

BOOST_AUTO_TEST_CASE(cross_circuit_handover_shares_three_attempts_and_never_extends_deadline) {
   auto io = boost::asio::io_context{};
   auto value = owner();
   const auto now = std::chrono::steady_clock::now();
   const auto deadline = now + 10s;
   const auto local = path_peer(2);
   const auto expired_local = path_peer(4);
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, deadline).owner;
   for (auto index = 0U; index != 2U; ++index) {
      const auto ticket = value.start_exchange(item);
      BOOST_REQUIRE(ticket);
      value.end_exchange(item, ticket);
   }
   const auto incoming = std::make_shared<manager::exchange>(
       manager::exchange{2, manager::role::responder, std::make_shared<p2p::cancellation_latch>()});
   auto adopted =
       boost::asio::co_spawn(io, value.async_accept_exchange(item, incoming, local), boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(adopted.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(adopted.get());
   BOOST_TEST(value.inspect(item).attempts == manager::max_attempts);
   BOOST_CHECK(value.begin(path_peer(1), 3, manager::role::responder, deadline + 1h).owner == item);
   BOOST_CHECK(item->deadline == deadline);
   value.end_exchange(item, incoming);
   auto exhausted =
       boost::asio::co_spawn(io, value.async_accept_exchange(item, incoming, local), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_REQUIRE(exhausted.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!exhausted.get());
   BOOST_TEST(value.inspect(item).attempts == manager::max_attempts);
   value.finish(item, p2p::hole_punch::status::failed);

   // Admission can be modeled at a past clock point; incoming validation must
   // still use the unchanged real expired owner deadline, not a fresh budget.
   const auto expired_deadline = now - 1ms;
   const auto expired =
       value.begin(path_peer(3), 4, manager::role::initiator, expired_deadline, expired_deadline - 1ms).owner;
   BOOST_REQUIRE(expired);
   auto rejected = boost::asio::co_spawn(io, value.async_accept_exchange(expired, incoming, expired_local),
                                         boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_REQUIRE(rejected.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!rejected.get());
   BOOST_TEST(value.inspect(expired).attempts == 0U);
   value.finish(expired, p2p::hole_punch::status::failed);
   BOOST_TEST(value.active() == 0U);
}

BOOST_AUTO_TEST_CASE(cross_circuit_expired_handover_keeps_both_terminal_guards_charged) {
   auto io = boost::asio::io_context{};
   const auto first = path_peer(1);
   const auto second = path_peer(2);
   const auto remote = first < second ? first : second;
   const auto local = first < second ? second : first;
   const auto value = std::make_shared<manager>(std::make_shared<p2p::detail::lifecycle_wakeup>());
   const auto deadline = std::chrono::steady_clock::now() + 100ms;
   const auto item = value->begin(remote, 1, manager::role::initiator, deadline).owner;
   BOOST_REQUIRE(item);
   const auto original = value->start_exchange(item);
   BOOST_REQUIRE(original);
   const auto incoming = std::make_shared<manager::exchange>(
       manager::exchange{2, manager::role::responder, std::make_shared<p2p::cancellation_latch>()});
   const auto old_barrier = std::make_shared<terminal_barrier>();
   const auto new_barrier = std::make_shared<terminal_barrier>();
   auto resources = p2p::resource_manager{};
   const auto make_resource = [&](const auto& barrier) {
      auto reserved = resources.reserve_stream(remote, p2p::resource_manager::session_direction::outbound);
      if (!reserved) {
         throw std::runtime_error{"collision component stream admission failed"};
      }
      auto resource = std::make_shared<p2p::detail::resource_stream>(std::move(*reserved));
      resource->attach(
          forge::net::transport::detail::stream_access::make(std::make_shared<terminal_path_stream>(barrier)));
      return resource;
   };
   const auto old_resource = make_resource(old_barrier);
   const auto new_resource = make_resource(new_barrier);
   auto closed_old = std::future<void>{};
   auto closed_new = std::future<void>{};
   auto cleanup = boost::scope::scope_exit{[&] {
      old_barrier->released = true;
      new_barrier->released = true;
      old_barrier->changed.notify();
      new_barrier->changed.notify();
      value->request_stop();
      if (!closed_old.valid()) {
         closed_old = boost::asio::co_spawn(io, close_collision_exchange(value, item, original, old_resource),
                                            boost::asio::use_future);
      }
      if (!closed_new.valid()) {
         closed_new = boost::asio::co_spawn(io, close_collision_exchange(value, item, incoming, new_resource),
                                            boost::asio::use_future);
      }
      value->finish(item, p2p::hole_punch::status::failed);
      io.restart();
      io.run();
   }};
   auto accepted =
       boost::asio::co_spawn(io, value->async_accept_exchange(item, incoming, local), boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(original->cancellation->stop_requested());
   BOOST_CHECK(accepted.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(value->begin(remote, 3, manager::role::responder, deadline + 1h).owner == item);
   BOOST_CHECK(item->deadline == deadline);
   io.restart();
   io.run(); // Only the manager's original deadline can wake this pending admission.
   BOOST_REQUIRE(accepted.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK(!accepted.get());
   BOOST_TEST(value->inspect(item).attempts == 1U);
   BOOST_CHECK(!item->cancellation->stop_requested());
   BOOST_CHECK(!incoming->cancellation->stop_requested());
   value->finish(item, p2p::hole_punch::status::failed);
   closed_old = boost::asio::co_spawn(io, close_collision_exchange(value, item, original, old_resource),
                                      boost::asio::use_future);
   closed_new = boost::asio::co_spawn(io, close_collision_exchange(value, item, incoming, new_resource),
                                      boost::asio::use_future);
   auto joined = boost::asio::co_spawn(io, value->async_join(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_TEST(value->active() == 1U);
   BOOST_TEST(resources.current().system.outbound_streams == 2U);
   BOOST_CHECK(joined.wait_for(0ms) != std::future_status::ready);
   new_barrier->released = true;
   new_barrier->changed.notify();
   io.restart();
   io.poll();
   BOOST_REQUIRE(closed_new.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(closed_new.get());
   BOOST_TEST(new_barrier->terminal);
   BOOST_TEST(value->active() == 1U);
   BOOST_CHECK(joined.wait_for(0ms) != std::future_status::ready);
   old_barrier->released = true;
   old_barrier->changed.notify();
   io.restart();
   io.run();
   BOOST_REQUIRE(closed_old.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(closed_old.get());
   BOOST_REQUIRE(joined.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(old_barrier->terminal);
   BOOST_TEST(resources.current().system.outbound_streams == 0U);
   BOOST_TEST(value->active() == 0U);
}

BOOST_AUTO_TEST_CASE(native_wave_budget_preserves_slow_inbound_window_and_never_extends_owner_deadline) {
   const auto now = manager::time_point{} + 1h;
   const auto native_budget = p2p::hole_punch::options{}.timeout;
   BOOST_CHECK(manager::dial_deadline(now + 30s, now) == now + native_budget);
   const auto owner_deadline = now + 9s;
   const auto first_wave = manager::dial_deadline(owner_deadline, now);
   BOOST_CHECK(first_wave == owner_deadline);
   BOOST_CHECK(first_wave > now + 2s);
   // Fresh exchanges after early failure get only the original budget left.
   BOOST_CHECK(manager::dial_deadline(owner_deadline, now + 3s) == owner_deadline);
   BOOST_CHECK(manager::dial_deadline(owner_deadline, now + 8s) == owner_deadline);
   BOOST_CHECK(manager::dial_deadline(owner_deadline, owner_deadline) == owner_deadline);
}

BOOST_AUTO_TEST_CASE(parallel_operation_pressure_does_not_allocate_waiting_peer_state) {
   auto value = owner();
   const auto now = manager::time_point{} + 1h;
   for (auto id = std::uint8_t{1}; id <= manager::max_parallel_operations; ++id) {
      BOOST_REQUIRE(value.begin(path_peer(id), id, manager::role::initiator, now + 10s, now).leader);
   }
   for (auto id = std::uint8_t{20}; id < 100; ++id) {
      BOOST_TEST(!value.begin(path_peer(id), id, manager::role::initiator, now + 10s, now).owner);
   }
   BOOST_TEST(value.active() == manager::max_parallel_operations);
   BOOST_TEST(value.retained() == manager::max_parallel_operations);
}

BOOST_AUTO_TEST_CASE(full_retry_history_rejects_new_peers_without_evicting_unexpired_bans) {
   auto value = owner();
   const auto now = manager::time_point{} + 1h;
   for (auto id = std::size_t{}; id < manager::max_retained_peers; ++id) {
      const auto peer = path_peer(static_cast<std::uint8_t>(id));
      const auto item = value.begin(peer, id + 1, manager::role::initiator, now + 10s, now);
      BOOST_REQUIRE(item.leader);
      value.finish(item.owner, p2p::hole_punch::status::failed, now);
   }
   BOOST_TEST(value.active() == 0U);
   BOOST_TEST(value.retained() == manager::max_retained_peers);
   BOOST_TEST(!value.begin(path_peer(200), 200, manager::role::initiator, now + 10s, now).owner);
   BOOST_TEST(!value.begin(path_peer(0), 201, manager::role::initiator, now + 10s, now).owner);
   const auto later = now + manager::failure_backoff;
   BOOST_TEST(value.begin(path_peer(200), 200, manager::role::initiator, later + 10s, later).leader);
   BOOST_TEST(value.retained() == 1U);
}

BOOST_AUTO_TEST_CASE(deadline_exhaustion_and_cancellation_cannot_start_a_new_exchange) {
   auto value = owner();
   const auto now = manager::time_point{} + 1h;
   BOOST_TEST(!value.begin(path_peer(1), 1, manager::role::initiator, now, now).owner);
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 10s, now).owner;
   BOOST_TEST(!value.begin_exchange(item, now + 10s));
   item->cancellation->request_stop();
   BOOST_TEST(!value.begin_exchange(item, now));
   BOOST_TEST(value.inspect(item).attempts == 0U);
}

BOOST_AUTO_TEST_CASE(stop_callbacks_reenter_without_locks_and_join_waits_for_terminal_handler) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = manager::time_point{} + 1h;
   const auto item = value.begin(path_peer(1), 1, manager::role::responder, now + 10s, now).owner;
   BOOST_REQUIRE(value.begin_exchange(item, now));
   auto canceled = false;
   item->cancellation->arm([&] {
      canceled = true;
      BOOST_TEST(value.active() == 1U);
      BOOST_TEST(!value.begin(path_peer(2), 2, manager::role::initiator, now + 10s, now).owner);
   });
   value.request_stop();
   BOOST_TEST(canceled);
   auto joined = boost::asio::co_spawn(io, value.async_join(), boost::asio::use_future);
   io.poll();
   value.finish(item, p2p::hole_punch::status::failed, now);
   value.finish(item, p2p::hole_punch::status::succeeded, now);
   BOOST_TEST(value.active() == 1U);
   BOOST_CHECK(joined.wait_for(0ms) != std::future_status::ready);
   value.end_exchange(item, now);
   io.restart();
   io.run();
   BOOST_CHECK(joined.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(value.active() == 0U);
   BOOST_TEST(value.retained() == 0U);
   BOOST_CHECK(value.inspect(item).result == p2p::hole_punch::status::failed);
}

BOOST_AUTO_TEST_CASE(completed_results_are_sticky_for_manual_waiters) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 10s, now).owner;
   value.finish(item, p2p::hole_punch::status::succeeded, now);
   auto result = boost::asio::co_spawn(io, value.async_wait(item, now + 10s), boost::asio::use_future);
   io.run();
   BOOST_CHECK(result.get() == p2p::hole_punch::status::succeeded);
   BOOST_TEST(value.retained() == 0U);
}

BOOST_AUTO_TEST_CASE(ordinary_waiter_cancellation_does_not_stop_owner_or_other_waiters) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::responder, now + 1h, now).owner;
   auto cancellation = boost::asio::cancellation_signal{};
   auto canceled = boost::asio::co_spawn(io, value.async_wait(item, now + 1h),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   auto unaffected = boost::asio::co_spawn(io, value.async_wait(item, now + 1h), boost::asio::use_future);
   io.poll();
   cancellation.emit(boost::asio::cancellation_type::terminal);
   io.restart();
   io.poll();
   BOOST_REQUIRE(canceled.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(canceled.get()), boost::system::system_error);
   BOOST_TEST(!item->cancellation->stop_requested());
   BOOST_TEST(value.active() == 1U);
   BOOST_CHECK(unaffected.wait_for(0ms) != std::future_status::ready);
   value.finish(item, p2p::hole_punch::status::succeeded);
   io.restart();
   io.poll();
   BOOST_REQUIRE(unaffected.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK(unaffected.get() == p2p::hole_punch::status::succeeded);
}

BOOST_AUTO_TEST_CASE(explicit_cancel_before_or_during_raw_open_joins_without_canceling_shared_session) {
   for (const auto before_open : {true, false}) {
      auto value = owner();
      auto io = boost::asio::io_context{};
      const auto now = std::chrono::steady_clock::now();
      const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
      BOOST_REQUIRE(value.begin_exchange(item, now));
      const auto state = std::make_shared<pending_open>();
      auto session = forge::net::transport::detail::session_access::make(
          std::make_shared<pending_path_session>(io, state));
      auto canceled = std::future<bool>{};
      if (before_open) {
         canceled = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
         io.poll();
         BOOST_TEST(item->cancellation->stop_requested());
         BOOST_CHECK(canceled.wait_for(0ms) != std::future_status::ready);
         io.restart();
      }
      auto worker_terminal = false;
      boost::asio::co_spawn(io, value.async_run_exchange(item,
          [&session](boost::asio::cancellation_slot, std::shared_ptr<p2p::detail::worker_stop_bridge>)
              -> boost::asio::awaitable<void> { static_cast<void>(co_await session.async_open_stream()); }),
          [&](std::exception_ptr) {
             worker_terminal = true;
             value.end_exchange(item);
             value.finish(item, p2p::hole_punch::status::failed);
          });
      io.poll();
      if (!before_open) {
         BOOST_REQUIRE(state->entered);
         canceled = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
         io.restart();
         io.poll();
      }
      BOOST_REQUIRE(canceled.wait_for(0ms) == std::future_status::ready);
      BOOST_TEST(canceled.get());
      BOOST_TEST(worker_terminal);
      BOOST_TEST(state->entered == !before_open);
      BOOST_TEST(state->terminal == !before_open);
      BOOST_TEST(state->session_cancels == 0U);
      BOOST_TEST(value.active() == 0U);
      auto repeated = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
      io.restart();
      io.poll();
      BOOST_REQUIRE(repeated.wait_for(0ms) == std::future_status::ready);
      BOOST_TEST(!repeated.get());
   }
}

BOOST_AUTO_TEST_CASE(explicit_cancel_before_dial_publication_is_sticky) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   auto canceled = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
   io.poll();
   BOOST_TEST(item->cancellation->stop_requested());
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   auto launched = false;
   boost::asio::co_spawn(io, value.async_wait_dials(batch, item, now + 1h, [] { return false; },
       [&] { launched = true; }), [&](std::exception_ptr error, bool direct) {
          BOOST_CHECK(!error);
          BOOST_TEST(!direct);
          value.finish(item, p2p::hole_punch::status::failed);
       });
   io.restart();
   io.poll();
   BOOST_REQUIRE(canceled.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(canceled.get());
   BOOST_TEST(!launched);
}

BOOST_AUTO_TEST_CASE(explicit_cancel_has_bounded_joiners_and_cannot_abandon_native_worker_drain) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::responder, now + 1h, now).owner;
   auto first_waiter = boost::asio::co_spawn(io, value.async_wait(item, now + 1h), boost::asio::use_future);
   auto second_waiter = boost::asio::co_spawn(io, value.async_wait(item, now + 1h), boost::asio::use_future);
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   const auto barrier = std::make_shared<terminal_barrier>();
   auto stops = std::size_t{};
   auto stopped = p2p::cancellation_latch::subscribe(item->cancellation, [&] {
      ++stops;
      // Re-entry proves owner callbacks are not called under the manager lock.
      BOOST_TEST(value.active() == 1U);
   });
   auto worker_terminal = false;
   const auto worker = [barrier]() -> boost::asio::awaitable<void> {
      barrier->entered = true;
      while (!barrier->released) {
         const auto epoch = barrier->changed.epoch();
         if (!barrier->released) { co_await barrier->changed.async_wait(epoch); }
      }
      barrier->terminal = true;
   };
   const auto launch = [&] {
      batch->add_worker(true);
      boost::asio::co_spawn(io, worker(), [&, batch](std::exception_ptr) {
         worker_terminal = true;
         batch->complete_worker();
      });
   };
   boost::asio::co_spawn(io, value.async_wait_dials(batch, item, now + 1h, [] { return false; }, launch),
       [&](std::exception_ptr error, bool direct) {
          BOOST_CHECK(!error);
          BOOST_TEST(!direct);
          value.finish(item, p2p::hole_punch::status::failed);
       });
   io.poll();
   BOOST_REQUIRE(barrier->entered);
   auto caller_stop = boost::asio::cancellation_signal{};
   auto cancellations = std::vector<std::future<bool>>{};
   cancellations.push_back(boost::asio::co_spawn(io, value.async_cancel(item->peer),
       boost::asio::bind_cancellation_slot(caller_stop.slot(), boost::asio::use_future)));
   for (auto index = std::size_t{1}; index < manager::max_cancel_waiters; ++index) {
      cancellations.push_back(boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future));
   }
   io.restart();
   io.poll();
   caller_stop.emit(boost::asio::cancellation_type::terminal);
   auto excess = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_REQUIRE(excess.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(excess.get()), p2p::exceptions::backpressure_rejected);
   BOOST_TEST(stops == 1U);
   BOOST_TEST(batch->cancellation->stop_requested());
   for (auto& result : cancellations) { BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready); }
   BOOST_TEST(!worker_terminal);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.poll();
   for (auto& result : cancellations) {
      BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
      BOOST_TEST(result.get());
   }
   BOOST_TEST(worker_terminal);
   BOOST_TEST(barrier->terminal);
   BOOST_TEST(value.active() == 0U);
   BOOST_REQUIRE(first_waiter.wait_for(0ms) == std::future_status::ready);
   BOOST_REQUIRE(second_waiter.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK(first_waiter.get() == p2p::hole_punch::status::failed);
   BOOST_CHECK(second_waiter.get() == p2p::hole_punch::status::failed);
}

BOOST_AUTO_TEST_CASE(explicit_cancel_waits_for_handler_terminal_close_even_after_owner_result) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   auto resources = p2p::resource_manager{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::responder, now + 1h, now).owner;
   BOOST_REQUIRE(value.begin_exchange(item, now));
   auto reservation = resources.reserve_stream(item->peer, p2p::resource_manager::session_direction::inbound);
   BOOST_REQUIRE(reservation);
   const auto resource = std::make_shared<p2p::detail::resource_stream>(std::move(*reservation));
   const auto dispatcher = resource;
   const auto barrier = std::make_shared<terminal_barrier>();
   resource->attach(forge::net::transport::detail::stream_access::make(
       std::make_shared<terminal_path_stream>(barrier)));
   auto stopped = p2p::cancellation_latch::subscribe(item->cancellation,
       [resource] { resource->request_cancel(); });
   auto canceled = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
   io.poll();
   BOOST_TEST(item->cancellation->stop_requested());
   BOOST_TEST(barrier->cancels == 1U);
   value.finish(item, p2p::hole_punch::status::failed);
   auto completed = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
   const auto close = [&]() -> boost::asio::awaitable<void> {
      auto exchange = boost::scope::scope_exit{[&] { value.end_exchange(item); }};
      co_await manager::async_close_exchange(resource, {});
   };
   auto closed = boost::asio::co_spawn(io, close(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_REQUIRE(barrier->entered);
   BOOST_REQUIRE(completed.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!completed.get());
   BOOST_CHECK(canceled.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(value.active() == 1U);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.poll();
   BOOST_REQUIRE(canceled.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(canceled.get());
   BOOST_REQUIRE(closed.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(closed.get());
   BOOST_TEST(barrier->terminal);
   BOOST_TEST(!dispatcher->valid());
   BOOST_TEST(resources.current().system.inbound_streams == 0U);
}

BOOST_AUTO_TEST_CASE(explicit_cancel_races_natural_completion_at_manager_linearization) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   for (auto round = std::size_t{}; round < 64; ++round) {
      auto value = owner();
      const auto now = std::chrono::steady_clock::now();
      const auto item = value.begin(path_peer(1), 1, manager::role::responder, now + 1h, now).owner;
      const auto finished = std::make_shared<std::promise<void>>();
      auto terminal = finished->get_future();
      if (round == 0) { value.finish(item, p2p::hole_punch::status::succeeded); }
      auto result = boost::asio::co_spawn(runtime.context(), value.async_cancel(item->peer), boost::asio::use_future);
      boost::asio::post(runtime.context(), [&, item, finished] {
         value.finish(item, p2p::hole_punch::status::succeeded);
         finished->set_value();
      });
      BOOST_REQUIRE(result.wait_for(5s) == std::future_status::ready);
      const auto accepted = result.get();
      BOOST_REQUIRE(terminal.wait_for(5s) == std::future_status::ready);
      BOOST_TEST(item->cancellation->stop_requested() == accepted);
      BOOST_CHECK(value.inspect(item).result == p2p::hole_punch::status::succeeded);
      BOOST_TEST(value.active() == 0U);
      BOOST_TEST(value.retained() == 0U);
      if (round == 0) { BOOST_TEST(!accepted); }
   }
}

BOOST_AUTO_TEST_CASE(stop_before_delay_wait_installation_is_sticky) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   item->cancellation->request_stop();
   auto result = boost::asio::co_spawn(io, value.async_delay(item, now + 30min), boost::asio::use_future);
   io.run();
   BOOST_CHECK(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!result.get());
}

BOOST_AUTO_TEST_CASE(stop_during_delay_is_immediate_and_stray_notifications_do_not_shorten_it) {
   const auto wakeup = std::make_shared<p2p::detail::lifecycle_wakeup>();
   auto value = manager{wakeup};
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   auto result = boost::asio::co_spawn(io, value.async_delay(item, now + 30min), boost::asio::use_future);
   io.poll();
   wakeup->notify();
   io.restart();
   io.poll();
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   item->cancellation->request_stop();
   io.restart();
   io.run();
   BOOST_CHECK(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!result.get());
}

BOOST_AUTO_TEST_CASE(exchange_stop_cancels_pending_raw_open_without_canceling_shared_session) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto state = std::make_shared<pending_open>();
   auto session = forge::net::transport::detail::session_access::make(
       std::make_shared<pending_path_session>(io, state));
   auto result = boost::asio::co_spawn(io, value.async_run_exchange(item,
       [&session](boost::asio::cancellation_slot, std::shared_ptr<p2p::detail::worker_stop_bridge>)
           -> boost::asio::awaitable<void> { static_cast<void>(co_await session.async_open_stream()); }),
       boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(state->entered);
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   item->cancellation->request_stop();
   io.restart();
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(result.get());
   BOOST_TEST(state->terminal);
   BOOST_TEST(state->session_cancels == 0U);
}

BOOST_AUTO_TEST_CASE(exchange_stop_before_installation_never_opens_the_shared_session) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto state = std::make_shared<pending_open>();
   auto session = forge::net::transport::detail::session_access::make(
       std::make_shared<pending_path_session>(io, state));
   item->cancellation->request_stop();
   auto result = boost::asio::co_spawn(io, value.async_run_exchange(item,
       [&session](boost::asio::cancellation_slot, std::shared_ptr<p2p::detail::worker_stop_bridge>)
           -> boost::asio::awaitable<void> { static_cast<void>(co_await session.async_open_stream()); }),
       boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(result.get());
   BOOST_TEST(!state->entered);
   BOOST_TEST(state->session_cancels == 0U);
}

BOOST_AUTO_TEST_CASE(exchange_overall_deadline_cancels_pending_raw_open) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1s, now).owner;
   const auto state = std::make_shared<pending_open>();
   auto session = forge::net::transport::detail::session_access::make(
       std::make_shared<pending_path_session>(io, state));
   auto result = boost::asio::co_spawn(io, value.async_run_exchange(item,
       [&session](boost::asio::cancellation_slot, std::shared_ptr<p2p::detail::worker_stop_bridge>)
           -> boost::asio::awaitable<void> { static_cast<void>(co_await session.async_open_stream()); }),
       boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(state->entered);
   auto deadline = p2p::operation_deadline{io, std::chrono::ceil<std::chrono::milliseconds>(
       item->deadline - std::chrono::steady_clock::now())};
   deadline.arm([item] { item->cancellation->request_stop(); });
   io.restart();
   io.run_for(2s);
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(result.get());
   BOOST_TEST(deadline.timed_out());
   BOOST_TEST(state->terminal);
   BOOST_TEST(state->session_cancels == 0U);
}

BOOST_AUTO_TEST_CASE(explicit_terminal_close_with_dispatcher_owner_precedes_exchange_release) {
   for (const auto failed : {false, true}) {
      auto value = owner();
      auto io = boost::asio::io_context{};
      auto resources = p2p::resource_manager{};
      const auto now = std::chrono::steady_clock::now();
      const auto item = value.begin(path_peer(1), 1, manager::role::responder, now + 1h, now).owner;
      BOOST_REQUIRE(value.begin_exchange(item, now));
      auto reservation = resources.reserve_stream(path_peer(1), p2p::resource_manager::session_direction::inbound);
      BOOST_REQUIRE(reservation);
      auto resource = std::make_shared<p2p::detail::resource_stream>(std::move(*reservation));
      const auto dispatcher_owner = resource;
      const auto barrier = std::make_shared<terminal_barrier>();
      barrier->fail_close = failed;
      resource->attach(forge::net::transport::detail::stream_access::make(
          std::make_shared<terminal_path_stream>(barrier)));
      const auto close = [&]() -> boost::asio::awaitable<void> {
         auto exchange = boost::scope::scope_exit{[&] { value.end_exchange(item); }};
         auto failure = std::exception_ptr{};
         if (failed) {
            failure = std::make_exception_ptr(std::runtime_error{"CONNECT error"});
         }
         co_await manager::async_close_exchange(resource, failure);
      };
      auto result = boost::asio::co_spawn(io, close(), boost::asio::use_future);
      io.poll();
      BOOST_REQUIRE(barrier->entered);
      BOOST_TEST(value.inspect(item).exchanging);
      BOOST_TEST(resources.current().system.inbound_streams == 1U);
      BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
      barrier->released = true;
      barrier->changed.notify();
      io.restart();
      io.poll();
      BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
      if (failed) {
         try { result.get(); BOOST_FAIL("expected the primary CONNECT error"); }
         catch (const std::runtime_error& error) { BOOST_TEST(std::string_view{error.what()} == "CONNECT error"); }
      } else { BOOST_CHECK_NO_THROW(result.get()); }
      BOOST_TEST(barrier->terminal);
      BOOST_TEST(barrier->closes == 1U);
      BOOST_TEST(!value.inspect(item).exchanging);
      BOOST_TEST(resources.current().system.inbound_streams == 0U);
      BOOST_CHECK(!dispatcher_owner->valid());
   }
}

BOOST_AUTO_TEST_CASE(real_waiter_allocation_failure_never_publishes_workers_or_retries) {
   auto io = boost::asio::io_context{};
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   auto allocation = waiter_allocation{};
   auto launched = false;
   auto completed = false;
   auto executor_progress = false;
   boost::asio::post(io, [&] { executor_progress = true; });
   BOOST_CHECK_THROW(batch->async_run([&] { launched = true; batch->add_worker(); },
       boost::asio::bind_allocator(waiter_allocator<std::byte>{allocation},
           [&](boost::system::error_code) { completed = true; })), std::bad_alloc);
   BOOST_TEST(allocation.denied);
   BOOST_TEST(allocation.attempts == 1U);
   BOOST_TEST(!launched);
   BOOST_TEST(!completed);
   BOOST_TEST(batch->active() == 0U);
   io.poll();
   BOOST_TEST(executor_progress);
}

BOOST_AUTO_TEST_CASE(preparation_allocation_failure_after_receipt_keeps_admission_until_native_close) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   auto resources = p2p::resource_manager{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   const auto barrier = std::make_shared<terminal_barrier>();
   barrier->fail_close = true;
   auto admission = resources.reserve_session(p2p::resource_manager::session_direction::outbound);
   BOOST_REQUIRE(admission);
   auto descriptor = admission->reserve_file_descriptors(1);
   BOOST_REQUIRE(descriptor);
   auto connection = p2p::direct::connection{};
   connection.admission.emplace(std::move(*admission));
   connection.native_lifetime = std::make_shared<p2p::resource_manager::file_descriptor_reservation>(
       std::move(*descriptor));
   const auto native = std::weak_ptr<void>{connection.native_lifetime};
   connection.session = forge::net::transport::detail::session_access::make(
       std::make_shared<terminal_path_session>(barrier, connection.native_lifetime));
   auto allocation = waiter_allocation{};
   const auto worker = [&]() -> boost::asio::awaitable<void> {
      auto failure = std::exception_ptr{};
      try {
         // A real allocator fault in the preparation harness, not a synthetic
         // bad_alloc result or global process allocator/test escape.
         auto roots = std::vector<p2p::endpoint, waiter_allocator<p2p::endpoint>>{
             waiter_allocator<p2p::endpoint>{allocation}};
         roots.push_back(p2p::parse_endpoint("/ip4/8.8.8.8/tcp/4001"));
      } catch (...) { failure = std::current_exception(); }
      co_await p2p::direct::async_discard_unpublished(connection);
      if (failure) { std::rethrow_exception(failure); }
   };
   const auto launch = [&] {
      batch->add_worker();
      boost::asio::co_spawn(io, worker(), [batch](std::exception_ptr failure) {
         if (failure) { batch->fail(failure); }
         batch->complete_worker();
      });
   };
   auto result = boost::asio::co_spawn(io,
       value.async_wait_dials(batch, item, now + 1h, [] { return false; }, launch), boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(barrier->entered);
   BOOST_TEST(allocation.attempts == 1U);
   BOOST_TEST(batch->active() == 1U);
   BOOST_REQUIRE(connection.admission);
   BOOST_TEST(connection.admission->active());
   BOOST_TEST(!native.expired());
   BOOST_TEST(resources.current().system.file_descriptors == 1U);
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(result.get()), std::bad_alloc);
   BOOST_TEST(barrier->terminal);
   BOOST_TEST(barrier->closes == 1U);
   BOOST_TEST(batch->active() == 0U);
   BOOST_TEST(!connection.admission);
   BOOST_TEST(native.expired());
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(prearmed_terminal_wait_needs_no_new_waiter_allocation_after_launch) {
   auto io = boost::asio::io_context{};
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   auto allocation = waiter_allocation{.denied = false};
   auto completions = std::size_t{};
   auto terminal = false;
   batch->async_run([&] {
      batch->add_worker();
      allocation.denied = true;
      boost::asio::post(io, [&, batch] { terminal = true; batch->complete_worker(); });
   }, boost::asio::bind_allocator(waiter_allocator<std::byte>{allocation},
       [&](boost::system::error_code error) {
          BOOST_CHECK(error == boost::asio::error::operation_aborted);
          BOOST_TEST(terminal);
          BOOST_TEST(batch->active() == 0U);
          ++completions;
       }));
   BOOST_TEST(completions == 0U);
   io.poll();
   BOOST_TEST(completions == 1U);
   BOOST_TEST(allocation.attempts == 1U);
}

BOOST_AUTO_TEST_CASE(multithreaded_terminal_wait_serializes_install_cancel_and_worker_completion) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   for (auto round = std::size_t{}; round < 64; ++round) {
      const auto batch = std::make_shared<manager::dial_batch>(runtime.context().get_executor());
      auto entered = std::latch{4};
      auto threads = std::array<std::thread::id, 4>{};
      auto terminal = std::atomic_size_t{};
      const auto completion = std::make_shared<std::promise<bool>>();
      auto result = completion->get_future();
      batch->async_run([&] {
         for (auto index = std::size_t{}; index < 4; ++index) { batch->add_worker(true); }
         for (auto index = std::size_t{}; index < 4; ++index) {
            boost::asio::post(runtime.context(), [&, batch, index] {
               threads[index] = std::this_thread::get_id();
               // Test-only barrier forces callbacks onto four actual runtime
               // threads; production workers never block an executor to join.
               entered.arrive_and_wait();
               if (index == 0) { batch->direct_arrived(); }
               if (index == 1) { batch->stop_waiting(); }
               terminal.fetch_add(1);
               batch->complete_worker();
            });
         }
      }, [&, completion, batch](boost::system::error_code error) {
         completion->set_value(error == boost::asio::error::operation_aborted &&
             terminal.load() == 4 && batch->active() == 0);
      });
      BOOST_REQUIRE(result.wait_for(5s) == std::future_status::ready);
      BOOST_TEST(result.get());
      for (auto left = std::size_t{}; left < 4; ++left) {
         for (auto right = left + 1; right < 4; ++right) { BOOST_CHECK(threads[left] != threads[right]); }
      }
   }
}

BOOST_AUTO_TEST_CASE(post_launch_probe_bad_alloc_joins_native_terminal_callback_before_throwing) {
   const auto wakeup = std::make_shared<p2p::detail::lifecycle_wakeup>();
   auto value = manager{wakeup};
   auto io = boost::asio::io_context{};
   auto lifecycle = p2p::detail::lifecycle_tracker{io.get_executor()};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   const auto barrier = std::make_shared<terminal_barrier>();
   batch->cancellation->arm([barrier] { barrier->changed.notify(); });
   const auto worker = [batch, barrier]() -> boost::asio::awaitable<void> {
      while (!batch->cancellation->stop_requested()) {
         const auto epoch = barrier->changed.epoch();
         if (!batch->cancellation->stop_requested()) { co_await barrier->changed.async_wait(epoch); }
      }
      barrier->entered = true;
      while (!barrier->released) {
         const auto epoch = barrier->changed.epoch();
         if (!barrier->released) { co_await barrier->changed.async_wait(epoch); }
      }
      barrier->terminal = true;
   };
   const auto launch = std::function<void()>{[&] {
      auto tracked = lifecycle.track();
      BOOST_REQUIRE(tracked.active());
      const auto worker_executor = tracked.executor();
      batch->add_worker();
      boost::asio::co_spawn(worker_executor, worker(),
          [batch, wakeup, tracked = std::move(tracked)](std::exception_ptr) mutable {
             tracked.release();
             wakeup->notify();
             batch->complete_worker();
          });
   }};
   // A distinct post-publication probe fault exercises cancellation + terminal
   // drain. The preceding allocator test fails the actual native wait itself.
   auto probes = std::size_t{};
   const auto probe = std::function<bool()>{[&]() -> bool {
      if (++probes > 1) { throw std::bad_alloc{}; }
      return false;
   }};
   auto result = boost::asio::co_spawn(io,
       value.async_wait_dials(batch, item, now + 1h, probe, launch), boost::asio::use_future);
   io.poll();
   BOOST_TEST(batch->cancellation->stop_requested());
   BOOST_REQUIRE(barrier->entered);
   BOOST_TEST(batch->active() == 1U);
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   lifecycle.request_stop();
   auto joined = boost::asio::co_spawn(io, lifecycle.wait(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(joined.wait_for(0ms) != std::future_status::ready);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(result.get()), std::bad_alloc);
   BOOST_REQUIRE(joined.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(batch->active() == 0U);
   BOOST_TEST(barrier->terminal);
}

BOOST_AUTO_TEST_CASE(late_authenticated_inbound_remains_eligible_after_outbound_tcp_failure) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto peer = path_peer(1);
   const auto item = value.begin(peer, 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   auto direct = false;
   const auto launch = std::function<void()>{[&] {
      batch->add_worker(true);
      boost::asio::post(io, [batch] { batch->complete_worker(); });
   }};
   auto result = boost::asio::co_spawn(io,
       value.async_wait_dials(batch, item, now + 1h, [&] { return direct; }, launch), boost::asio::use_future);
   io.poll();
   BOOST_TEST(batch->active() == 0U);
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   value.notify_direct(path_peer(2), p2p::path::kind::direct, p2p::peer_authentication::noise);
   value.notify_direct(peer, p2p::path::kind::relay, p2p::peer_authentication::noise);
   value.notify_direct(peer, p2p::path::kind::direct, p2p::peer_authentication::unverified);
   io.restart();
   io.poll();
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   direct = true;
   value.notify_direct(peer, p2p::path::kind::direct, p2p::peer_authentication::noise);
   io.restart();
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(result.get());
}

BOOST_AUTO_TEST_CASE(stop_during_remaining_inbound_budget_completes_without_deadline_wait) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   const auto launch = std::function<void()>{[&] { batch->add_worker(true); batch->complete_worker(); }};
   auto result = boost::asio::co_spawn(io,
       value.async_wait_dials(batch, item, now + 1h, [] { return false; }, launch), boost::asio::use_future);
   io.poll();
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   item->cancellation->request_stop();
   io.restart();
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!result.get());
}

BOOST_AUTO_TEST_CASE(sticky_stop_before_wait_registration_never_publishes_candidates) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   auto launched = false;
   item->cancellation->request_stop();
   auto result = boost::asio::co_spawn(io,
       value.async_wait_dials(batch, item, now + 1h, [] { return false; }, [&] { launched = true; }),
       boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!result.get());
   BOOST_TEST(!launched);
}

BOOST_AUTO_TEST_CASE(inbound_window_expiry_uses_preowned_deadline_without_reinstalling_a_waiter) {
   auto value = owner();
   auto io = boost::asio::io_context{};
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   auto deadline = p2p::operation_deadline{io, 20ms};
   deadline.arm([batch] { batch->cancellation->request_stop(); });
   const auto launch = std::function<void()>{[&] { batch->add_worker(true); batch->complete_worker(); }};
   auto result = boost::asio::co_spawn(io,
       value.async_wait_dials(batch, item, now + 1h, [] { return false; }, launch), boost::asio::use_future);
   io.poll();
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   io.restart();
   io.run_for(1s);
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!result.get());
   BOOST_TEST(deadline.timed_out());
   BOOST_TEST(batch->active() == 0U);
}

BOOST_AUTO_TEST_CASE(hole_punch_uses_single_authenticated_listener_observation_without_advertising_it) {
   using observed_manager = p2p::detail::observed_address_manager;
   auto observations = observed_manager{};
   const auto now = manager::time_point{} + 1h;
   const auto local = p2p::parse_endpoint("/ip4/10.0.0.2/tcp/4001");
   const auto remote = p2p::parse_endpoint("/ip4/11.0.0.1/tcp/5001");
   const auto reported = p2p::parse_endpoint("/ip4/8.8.8.8/tcp/8000");
   const auto listeners = std::array{p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4001")};
   BOOST_REQUIRE(observations.observe(1, path_peer(1), local, remote, reported, listeners, now));
   const auto candidates = observations.hole_punch_candidates(now, listeners, 8);
   BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
   BOOST_TEST(candidates.front().to_string() == reported.to_string());
   BOOST_TEST(!candidates.front().peer.has_value());
   BOOST_TEST(observations.confirmed(now).empty());
   const auto other_listener = std::array{p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4002")};
   BOOST_TEST(observations.hole_punch_candidates(now, other_listener, 8).empty());
   BOOST_TEST(observations.hole_punch_candidates(now, {}, 8).empty());
   BOOST_TEST(observations.hole_punch_candidates(now, listeners, 0).empty());
   BOOST_TEST(observations.hole_punch_candidates(now + 10min, listeners, 8).empty());
   observations.remove(1);
   BOOST_TEST(observations.hole_punch_candidates(now, listeners, 8).empty());
}

BOOST_AUTO_TEST_CASE(hole_punch_observed_candidates_preserve_authentication_transport_and_capacity_bounds) {
   using observed_manager = p2p::detail::observed_address_manager;
   auto observations = observed_manager{observed_manager::options{.max_observations = 8, .max_candidates = 3}};
   const auto now = manager::time_point{} + 1h;
   for (const auto quic : {false, true}) {
      const auto suffix = std::string{quic ? "/udp/4001/quic-v1" : "/tcp/4001"};
      const auto local = p2p::parse_endpoint("/ip4/10.0.0.2" + suffix);
      const auto remote = p2p::parse_endpoint("/ip4/11.0.0.1" + suffix);
      const auto reported = p2p::parse_endpoint("/ip4/8.8.8.8" + suffix);
      const auto listeners = std::array{p2p::parse_endpoint("/ip4/0.0.0.0" + suffix)};
      auto unauthenticated = remote;
      unauthenticated.peer = path_peer(2);
      BOOST_TEST(!observations.observe(10, p2p::peer_id{}, local, remote, reported, listeners, now));
      BOOST_TEST(!observations.observe(10, path_peer(1), local, unauthenticated, reported, listeners, now));
      BOOST_TEST(!observations.observe(10, path_peer(1), local, remote, reported, {}, now));
      const auto id = quic ? std::uint64_t{2} : std::uint64_t{1};
      BOOST_REQUIRE(observations.observe(id, path_peer(1), local, remote, reported, listeners, now));
      BOOST_REQUIRE(observations.observe(id + 2, path_peer(1), local, remote, reported, listeners, now));
      const auto candidates = observations.hole_punch_candidates(now, listeners, 8);
      BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
      BOOST_TEST(candidates.front().is_direct_quic() == quic);
      BOOST_TEST(candidates.front().to_string() == reported.to_string());
      const auto private_report = p2p::parse_endpoint("/ip4/192.168.1.2" + suffix);
      BOOST_REQUIRE(observations.observe(20, path_peer(3), local, remote, private_report, listeners, now));
      BOOST_TEST(observations.hole_punch_candidates(now, listeners, 8).size() == 1U);
      observations.remove(20);
   }
   const auto local = p2p::parse_endpoint("/ip4/10.0.0.2/tcp/4001");
   const auto remote = p2p::parse_endpoint("/ip4/11.0.0.1/tcp/5001");
   const auto listeners = std::array{p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4001"),
       p2p::parse_endpoint("/ip4/0.0.0.0/udp/4001/quic-v1")};
   const auto third = p2p::parse_endpoint("/ip4/8.8.4.4/tcp/4001");
   BOOST_REQUIRE(observations.observe(5, path_peer(3), local, remote, third, listeners, now));
   BOOST_TEST(observations.hole_punch_candidates(now, listeners, 1).size() == 1U);
   BOOST_TEST(observations.hole_punch_candidates(now, listeners, 100).size() == 3U);
   BOOST_TEST(!observations.observe(6, path_peer(4), local, remote,
       p2p::parse_endpoint("/ip4/1.1.1.1/tcp/4001"), listeners, now));
   BOOST_TEST(observations.hole_punch_candidates(now, listeners, 100).size() == 3U);
   BOOST_TEST(observations.confirmed(now).empty());
}

BOOST_AUTO_TEST_CASE(hole_punch_observed_quic_candidate_requires_exact_live_wildcard_listener_bind) {
   auto observations = p2p::detail::observed_address_manager{};
   const auto now = manager::time_point{} + 1h;
   const auto bind = p2p::parse_endpoint("/ip4/0.0.0.0/udp/4001/quic-v1");
   const auto listeners = std::array{bind};
   const auto remote = p2p::parse_endpoint("/ip4/11.0.0.1/udp/5001/quic-v1");
   const auto reported = p2p::parse_endpoint("/ip4/8.8.8.8/udp/8000/quic-v1");
   BOOST_REQUIRE(observations.observe(1, path_peer(1), bind, remote, reported, listeners, now));
   const auto candidates = observations.hole_punch_candidates(now, listeners, 4);
   BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
   BOOST_TEST(candidates.front().to_string() == reported.to_string());
   BOOST_TEST(observations.confirmed(now).empty());
   const auto ephemeral = p2p::parse_endpoint("/ip4/0.0.0.0/udp/54321/quic-v1");
   BOOST_TEST(!observations.observe(2, path_peer(1), ephemeral, remote, reported, listeners, now));
   const auto replacement = std::array{p2p::parse_endpoint("/ip4/0.0.0.0/udp/4002/quic-v1")};
   BOOST_TEST(observations.hole_punch_candidates(now, replacement, 4).empty());
   BOOST_TEST(observations.hole_punch_candidates(now + 10min, listeners, 4).empty());
}

BOOST_AUTO_TEST_CASE(authenticated_relay_direction_selects_initiator_and_rejects_direct_handlers) {
   using role = manager::role;
   for (const auto kind : {p2p::path::kind::direct, p2p::path::kind::hole_punch, p2p::path::kind::relay}) {
      for (const auto inbound : {false, true}) {
         BOOST_TEST(!manager::eligible(kind, p2p::peer_authentication::unverified, inbound, role::responder));
         BOOST_TEST(!manager::eligible(kind, p2p::peer_authentication::unverified, inbound, role::initiator));
      }
   }
   BOOST_TEST(manager::eligible(p2p::path::kind::relay, p2p::peer_authentication::noise, true, role::initiator));
   BOOST_TEST(!manager::eligible(p2p::path::kind::relay, p2p::peer_authentication::noise, true, role::responder));
   BOOST_TEST(manager::eligible(p2p::path::kind::relay, p2p::peer_authentication::libp2p_tls, false, role::responder));
   BOOST_TEST(!manager::eligible(p2p::path::kind::direct, p2p::peer_authentication::quic_tls, false, role::responder));
}

BOOST_AUTO_TEST_CASE(canonical_candidates_exclude_circuits_wrong_peers_zero_ports_and_duplicates) {
   const auto expected = path_peer(1);
   auto direct = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4001");
   auto wrong = direct;
   wrong.peer = path_peer(2);
   auto circuit = direct;
   circuit.peer = path_peer(2);
   circuit.relayed = p2p::endpoint::circuit{.target = expected};
   auto zero = direct;
   zero.transport.port = 0;
   auto unspecified = p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4001");
   auto quic = p2p::parse_endpoint("/ip4/127.0.0.1/udp/4002/quic-v1");
   const auto values = std::vector<p2p::endpoint>{circuit, wrong, zero, unspecified, direct, direct, quic};
   const auto result = manager::direct_endpoints(values, expected, 32);
   BOOST_REQUIRE_EQUAL(result.size(), 2U);
   BOOST_TEST(result.front().is_direct_tcp());
   BOOST_TEST(result.back().is_direct_quic());
   BOOST_CHECK(result.front().peer == expected);
   BOOST_TEST(manager::direct_endpoints(values, expected, 1).size() == 1U);
   BOOST_TEST(manager::direct_endpoints(values, expected, 0).empty());
}

BOOST_AUTO_TEST_CASE(dcutr_source_uses_complete_carrier_tuple_not_first_listener_port) {
   for (const auto protocol : {"tcp", "udp"}) {
      const auto suffix = std::string_view{protocol} == "udp" ? "/quic-v1" : "";
      const auto make = [&](std::string_view host, unsigned port) {
         return p2p::parse_endpoint("/ip4/" + std::string{host} + "/" + protocol + "/" +
             std::to_string(port) + suffix);
      };
      const auto first = make("0.0.0.0", 4001);
      const auto actual = make("0.0.0.0", 4002);
      const auto carrier = make("10.2.0.2", 4002);
      const auto remote = make("11.0.0.2", 5001);
      for (const auto& listeners : {std::vector{first, actual}, std::vector{actual, first}}) {
         const auto selected = manager::coordinated_source(listeners, remote, carrier);
         BOOST_TEST(selected.transport.host == carrier.transport.host);
         BOOST_TEST(selected.transport.port == carrier.transport.port);
         BOOST_CHECK(selected.transport.protocol == carrier.transport.protocol);
      }
      const auto ephemeral = make("10.2.0.2", 5002);
      const auto listened = std::vector{first, actual};
      BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(listened, remote, ephemeral)),
          p2p::exceptions::invalid_options);
      auto routed = first;
      routed.transport.host = carrier.transport.host;
      BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(listened, remote, ephemeral, routed)),
          p2p::exceptions::invalid_options);
      const auto unrelated = std::vector{first, make("10.2.0.3", 4002)};
      BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(unrelated, remote, carrier)),
          p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(dcutr_cross_transport_and_family_wildcards_use_actual_kernel_routed_source) {
   auto io = boost::asio::io_context{};
   auto resources = p2p::resource_manager{};
   const auto cases = std::array{
       std::pair{"/ip4/127.0.0.1/tcp/5001", "/ip4/10.2.0.2/udp/4002/quic-v1"},
       std::pair{"/ip4/127.0.0.1/udp/5001/quic-v1", "/ip4/10.2.0.2/tcp/4002"},
       std::pair{"/ip6/::1/tcp/5001", "/ip4/10.2.0.2/tcp/4002"},
       std::pair{"/ip6/::1/udp/5001/quic-v1", "/ip4/10.2.0.2/udp/4002/quic-v1"},
       std::pair{"/ip4/127.0.0.1/tcp/5001", "/ip6/::1/tcp/4002"},
   };
   for (const auto& [remote_text, carrier_text] : cases) {
      const auto remote = p2p::parse_endpoint(remote_text);
      const auto carrier = p2p::parse_endpoint(carrier_text);
      auto listener = remote;
      listener.transport.host = remote.transport.host_type == p2p::endpoint::host_kind::ip4 ? "0.0.0.0" : "::";
      listener.transport.port = 4001;
      auto other = listener;
      other.transport.port = 4003;
      const auto listeners = std::vector{listener, other};
      const auto routed = p2p::direct::select_dial_source(io, resources, listeners, remote);
      BOOST_REQUIRE(routed);
      const auto selected = manager::coordinated_source(listeners, remote, carrier, routed);
      BOOST_TEST(selected.transport.host == remote.transport.host);
      BOOST_TEST(selected.transport.port == listener.transport.port);
      BOOST_CHECK(selected.transport.protocol == remote.transport.protocol);
      BOOST_CHECK(selected.transport.host_type == remote.transport.host_type);
      BOOST_TEST(resources.current().system.file_descriptors == 0U);
      BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(listeners, remote, carrier)),
          p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(dcutr_routed_source_rejects_unknown_interface_and_ambiguous_listener_ownership) {
   auto io = boost::asio::io_context{};
   auto resources = p2p::resource_manager{};
   const auto remote = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/5001");
   const auto carrier = p2p::parse_endpoint("/ip4/10.2.0.2/udp/4002/quic-v1");
   const auto unrelated = std::vector{p2p::parse_endpoint("/ip4/10.2.0.3/tcp/4001")};
   const auto missing = p2p::direct::select_dial_source(io, resources, unrelated, remote);
   BOOST_TEST(!missing);
   BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(unrelated, remote, carrier, missing)),
       p2p::exceptions::invalid_options);
   const auto wildcard = p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4001");
   const auto concrete = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4001");
   const auto ambiguous = std::vector{wildcard, concrete};
   const auto routed = p2p::direct::select_dial_source(io, resources, ambiguous, remote);
   BOOST_REQUIRE(routed);
   BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(ambiguous, remote, carrier, routed)),
       p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(manager::coordinated_source(unrelated, remote, carrier, routed)),
       p2p::exceptions::invalid_options);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(explicit_path_cancel_joins_real_coordinated_owner_inbound_native_close) {
   auto io = boost::asio::io_context{};
   auto value = owner();
   const auto now = std::chrono::steady_clock::now();
   const auto item = value.begin(path_peer(1), 1, manager::role::initiator, now + 1h, now).owner;
   const auto batch = std::make_shared<manager::dial_batch>(io.get_executor());
   const auto operation = coordinated_owner(io.get_executor());
   auto resources = p2p::resource_manager{};
   auto permit = resources.reserve_dial(operation->options.expected_peer);
   BOOST_REQUIRE(permit);
   operation->permit = std::move(*permit);
   // Same parent bridge as node::impl::connect_coordinated. Its native readiness
   // includes the accepted worker, not merely the outgoing candidate coroutine.
   auto parent = p2p::cancellation_latch::subscribe(batch->cancellation,
       [operation] noexcept { operation->request_cancel(); });
   auto result = boost::asio::co_spawn(io, [&]() -> boost::asio::awaitable<bool> {
      const auto upgraded = co_await value.async_wait_dials(batch, item, item->deadline, [] { return false; }, [&] {
         batch->add_worker();
         operation->async_run([&] {
            BOOST_REQUIRE(operation->begin_inbound());
            operation->end_outbound(std::make_exception_ptr(std::runtime_error{"outgoing refused"}));
         }, [operation, batch](boost::system::error_code error) {
            BOOST_CHECK(error == boost::asio::error::operation_aborted);
            operation->finish();
            batch->complete_worker();
         });
      });
      value.finish(item, p2p::hole_punch::status::failed);
      co_return upgraded;
   }, boost::asio::use_future);
   io.poll();
   auto canceled = boost::asio::co_spawn(io, value.async_cancel(item->peer), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_TEST(operation->stopped());
   BOOST_TEST(!operation->begin_inbound());
   BOOST_CHECK(canceled.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(result.wait_for(0ms) != std::future_status::ready);
   const auto barrier = std::make_shared<terminal_barrier>();
   auto native = p2p::direct::connection{
       .session = forge::net::transport::detail::session_access::make(
           std::make_shared<terminal_path_session>(barrier, std::make_shared<int>(1)))};
   auto closed = boost::asio::co_spawn(io, [&]() -> boost::asio::awaitable<void> {
      co_await p2p::direct::async_discard_unpublished(native);
      operation->end_inbound();
   }, boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_TEST(barrier->entered);
   BOOST_TEST(resources.current().active_dials == 1U);
   BOOST_CHECK(canceled.wait_for(0ms) != std::future_status::ready);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.run();
   BOOST_CHECK_NO_THROW(closed.get());
   BOOST_REQUIRE(canceled.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(canceled.get());
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
   BOOST_TEST(!result.get());
   BOOST_TEST(barrier->terminal);
   BOOST_TEST(resources.current().active_dials == 0U);
   BOOST_TEST(value.active() == 0U);
}

BOOST_AUTO_TEST_CASE(coordinated_waiter_allocation_failure_precedes_native_children_and_can_retire_setup_once) {
   auto io = boost::asio::io_context{};
   const auto operation = coordinated_owner(io.get_executor());
   auto allocation = waiter_allocation{.denied = true};
   auto launched = false;
   BOOST_CHECK_THROW(operation->async_run([&] { launched = true; },
       boost::asio::bind_allocator(waiter_allocator<std::byte>{allocation}, [](boost::system::error_code) {})),
       std::bad_alloc);
   BOOST_TEST(!launched);
   BOOST_TEST(allocation.attempts == 1U);
   operation->request_cancel();
   operation->end_outbound({});
   operation->finish();
   auto joined = boost::asio::co_spawn(io, operation->async_join(), boost::asio::use_future);
   io.run();
   BOOST_CHECK_NO_THROW(joined.get());
}

BOOST_AUTO_TEST_CASE(coordinated_cancel_joins_eight_waiters_only_after_held_native_inbound_cleanup) {
   auto io = boost::asio::io_context{};
   const auto operation = coordinated_owner(io.get_executor());
   auto resources = p2p::resource_manager{};
   auto permit = resources.reserve_dial(operation->options.expected_peer);
   BOOST_REQUIRE(permit);
   operation->permit = std::move(*permit);
   auto ready = std::promise<void>{};
   auto terminal = ready.get_future();
   operation->async_run([&] {
      BOOST_REQUIRE(operation->begin_inbound());
      operation->end_outbound(std::make_exception_ptr(std::runtime_error{"outgoing refused"}));
   }, [&, operation](boost::system::error_code error) {
      BOOST_CHECK(error == boost::asio::error::operation_aborted);
      operation->finish();
      ready.set_value();
   });
   auto waiting = std::vector<std::future<void>>{};
   for (std::size_t index = 0; index != 8; ++index) {
      waiting.push_back(boost::asio::co_spawn(io, operation->async_join(), boost::asio::use_future));
   }
   auto excess = boost::asio::co_spawn(io, operation->async_join(), boost::asio::use_future);
   io.poll();
   BOOST_REQUIRE(excess.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_THROW(excess.get(), p2p::exceptions::backpressure_rejected);
   operation->request_cancel();
   operation->request_cancel();
   BOOST_TEST(!operation->begin_inbound());
   BOOST_TEST(resources.current().active_dials == 1U);
   BOOST_CHECK(terminal.wait_for(0ms) != std::future_status::ready);
   const auto barrier = std::make_shared<terminal_barrier>();
   auto native = p2p::direct::connection{
       .session = forge::net::transport::detail::session_access::make(
           std::make_shared<terminal_path_session>(barrier, std::make_shared<int>(1)))};
   auto closed = boost::asio::co_spawn(io, [&]() -> boost::asio::awaitable<void> {
      co_await p2p::direct::async_discard_unpublished(native);
      operation->end_inbound();
   }, boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_TEST(barrier->entered);
   for (auto& join : waiting) { BOOST_CHECK(join.wait_for(0ms) != std::future_status::ready); }
   BOOST_TEST(resources.current().active_dials == 1U);
   barrier->released = true;
   barrier->changed.notify();
   io.restart();
   io.run();
   BOOST_CHECK_NO_THROW(closed.get());
   BOOST_REQUIRE(terminal.wait_for(0ms) == std::future_status::ready);
   for (auto& join : waiting) { BOOST_CHECK_NO_THROW(join.get()); }
   BOOST_TEST(barrier->terminal);
   BOOST_TEST(resources.current().active_dials == 0U);
}

BOOST_AUTO_TEST_CASE(coordinated_prearmed_wait_serializes_multithreaded_cancel_and_terminal_native_workers) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   for (std::size_t round = 0; round != 32; ++round) {
      const auto operation = coordinated_owner(runtime.context().get_executor());
      auto entered = std::latch{4};
      auto done = std::atomic_size_t{};
      auto promise = std::promise<bool>{};
      auto result = promise.get_future();
      operation->async_run([&] {
         for (std::size_t index = 0; index != 3; ++index) { BOOST_REQUIRE(operation->begin_inbound()); }
         for (std::size_t index = 0; index != 4; ++index) {
            boost::asio::post(runtime.context(), [&, operation, index] {
               entered.arrive_and_wait();
               operation->request_cancel();
               done.fetch_add(1);
               if (index == 0) { operation->end_outbound({}); }
               else { operation->end_inbound(); }
            });
         }
      }, [&, operation](boost::system::error_code error) {
         operation->finish();
         promise.set_value(error == boost::asio::error::operation_aborted && done.load() == 4);
      });
      BOOST_REQUIRE(result.wait_for(2s) == std::future_status::ready);
      BOOST_TEST(result.get());
   }
}

BOOST_AUTO_TEST_SUITE_END()
