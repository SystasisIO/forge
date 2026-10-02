module;

#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.relay;

#include "autorelay_manager_fixture.hxx"

namespace forge::tests::p2p {
namespace asio = boost::asio;
namespace net = forge::net::p2p;
using namespace std::chrono_literals;

autorelay_manager_fixture::autorelay_manager_fixture(net::relay::policy policy)
    : _runtime{forge::asio::runtime_options{.worker_threads = 4}},
      _tracker{_runtime.context().get_executor()},
      _owner{std::make_shared<manager>(_runtime.context().get_executor(), policy, manager::callbacks{
          .current = [this] { return current(); },
          .reserve = [this](manager::candidate source, std::uint64_t generation,
                            std::shared_ptr<net::cancellation_latch> cancellation) {
             return reserve(std::move(source), generation, std::move(cancellation));
          },
          .now = [] { return now(); },
      })} {}

autorelay_manager_fixture::~autorelay_manager_fixture() {
   release_all();
   auto stopped = stop();
   if (stopped.wait_for(4s) != std::future_status::ready) {
      BOOST_ERROR("AutoRelay stop callback exceeded the cleanup budget");
      _runtime.stop();
      return;
   }
   stopped.get();
   auto joined = join();
   if (joined.wait_for(4s) != std::future_status::ready) {
      BOOST_ERROR("AutoRelay manager did not drain within the cleanup budget");
      _runtime.stop();
      return;
   }
   try { joined.get(); }
   catch (...) {
      if (!_fail_snapshot) { BOOST_ERROR("unexpected AutoRelay parent failure during cleanup"); }
   }
}

autorelay_manager_fixture::manager::time_point autorelay_manager_fixture::now() {
   return manager::time_point{} + 10'000s;
}

autorelay_manager_fixture::manager::candidate autorelay_manager_fixture::candidate(std::uint8_t id) {
   return {.peer = net::make_peer_id({.type = net::public_key::type::ed25519,
                                     .data = std::vector<std::uint8_t>(32, id)}),
           .session_id = id};
}

autorelay_manager_fixture::manager::snapshot autorelay_manager_fixture::snapshot(
    std::initializer_list<std::uint8_t> ids, bool permitted, std::uint64_t generation) {
   auto value = manager::snapshot{.permitted = permitted, .generation = generation};
   for (const auto id : ids) { value.candidates.push_back(candidate(id)); }
   return value;
}

autorelay_manager_fixture::manager::reservation autorelay_manager_fixture::lease(
    const manager::candidate& source, bool due, bool automatic) {
   return {.info = {.relay_peer = source.peer, .id = source.session_id, .expires_at = 20'000s, .ttl = 1h},
           .expires_at = now() + 1h, .renew_at = due ? now() - 1s : now() + 30min,
           .automatic = automatic};
}

const std::shared_ptr<autorelay_manager_fixture::manager>& autorelay_manager_fixture::owner() const { return _owner; }

std::vector<autorelay_manager_fixture::request> autorelay_manager_fixture::requests() const {
   const auto lock = std::scoped_lock{_mutex};
   auto values = std::vector<request>{};
   for (const auto& value : _requests) { values.push_back(*value); }
   return values;
}

std::size_t autorelay_manager_fixture::peak_active() const {
   const auto lock = std::scoped_lock{_mutex};
   return _peak_active;
}

void autorelay_manager_fixture::set_snapshot(manager::snapshot value) {
   const auto lock = std::scoped_lock{_mutex};
   _snapshot = std::move(value);
}

void autorelay_manager_fixture::after_snapshot(std::function<void()> callback) {
   const auto lock = std::scoped_lock{_mutex};
   _after_snapshot = std::move(callback);
}

void autorelay_manager_fixture::on_cancel(std::function<void()> callback) {
   const auto lock = std::scoped_lock{_mutex};
   _on_cancel = std::move(callback);
}

void autorelay_manager_fixture::hold_canceled(bool value) {
   const auto lock = std::scoped_lock{_mutex};
   _hold_canceled = value;
}
void autorelay_manager_fixture::immediate_success(bool value) {
   const auto lock = std::scoped_lock{_mutex};
   _immediate_success = value;
}
void autorelay_manager_fixture::fail_snapshot(bool value) {
   const auto lock = std::scoped_lock{_mutex};
   _fail_snapshot = value;
}
void autorelay_manager_fixture::start() { _owner->start(_tracker); }

autorelay_manager_fixture::manager::snapshot autorelay_manager_fixture::current() {
   auto value = manager::snapshot{};
   auto callback = std::function<void()>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_fail_snapshot) { throw std::logic_error{"scripted snapshot failure"}; }
      value = _snapshot;
      callback = std::exchange(_after_snapshot, {});
   }
   if (callback) { callback(); }
   return value;
}

asio::awaitable<net::relay::reservation::info> autorelay_manager_fixture::reserve(
    manager::candidate source, std::uint64_t generation, std::shared_ptr<net::cancellation_latch> cancellation) {
   auto item = std::make_shared<request>(request{.source = source, .generation = generation,
                                               .cancellation = cancellation});
   auto immediate = false;
   {
      const auto lock = std::scoped_lock{_mutex};
      _requests.push_back(item);
      _peak_active = std::max(_peak_active, ++_active);
      immediate = _immediate_success;
      if (immediate) {
         _snapshot.reservations.push_back(lease(source));
         item->released = true;
      }
   }
   _progress.notify_all();
   auto subscription = net::cancellation_latch::subscribe(cancellation, [this, item] {
      auto callback = std::function<void()>{};
      {
         const auto lock = std::scoped_lock{_mutex};
         item->canceled = true;
         callback = std::exchange(_on_cancel, {});
      }
      _progress.notify_all();
      _changed.notify();
      if (callback) { callback(); }
   });
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   auto fail = false;
   auto canceled = false;
   auto timed_out = false;
   for (;;) {
      const auto epoch = _changed.epoch();
      {
         const auto lock = std::scoped_lock{_mutex};
         canceled = cancellation->stop_requested() && !_hold_canceled;
         timed_out = std::chrono::steady_clock::now() >= deadline;
         if (item->released || canceled || timed_out) {
            fail = item->fail;
            item->completed = true;
            --_active;
            break;
         }
      }
      static_cast<void>(co_await _changed.async_wait_until(epoch, deadline));
   }
   _progress.notify_all();
   if (fail || timed_out) { FORGE_THROW_EXCEPTION(net::exceptions::timeout, "scripted reservation failure"); }
   if (canceled) { FORGE_THROW_EXCEPTION(net::exceptions::canceled, "scripted reservation cancellation"); }
   co_return lease(source).info;
}

void autorelay_manager_fixture::release(std::size_t index, bool fail) {
   {
      const auto lock = std::scoped_lock{_mutex};
      BOOST_REQUIRE_LT(index, _requests.size());
      _requests[index]->fail = fail;
      _requests[index]->released = true;
   }
   _changed.notify();
}

void autorelay_manager_fixture::release_all() {
   {
      const auto lock = std::scoped_lock{_mutex};
      _hold_canceled = false;
      for (const auto& item : _requests) { item->released = true; }
   }
   _changed.notify();
}

void autorelay_manager_fixture::wait_started(std::size_t count) {
   auto lock = std::unique_lock{_mutex};
   BOOST_REQUIRE(_progress.wait_for(lock, 4s, [&] { return _requests.size() >= count; }));
}

void autorelay_manager_fixture::wait_canceled(std::size_t count) {
   auto lock = std::unique_lock{_mutex};
   BOOST_REQUIRE(_progress.wait_for(lock, 4s, [&] {
      return static_cast<std::size_t>(std::ranges::count_if(_requests, [](const auto& item) {
         return item->canceled;
      })) >= count;
   }));
}

void autorelay_manager_fixture::wait_state(std::function<bool(const net::diagnostics::autorelay_state&)> predicate) {
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   do {
      if (predicate(_owner->stats())) { return; }
      auto promise = std::make_shared<std::promise<void>>();
      auto barrier = promise->get_future();
      asio::post(_runtime.context(), [promise] { promise->set_value(); });
      BOOST_REQUIRE(barrier.wait_until(deadline) == std::future_status::ready);
      barrier.get();
   } while (std::chrono::steady_clock::now() < deadline);
   BOOST_REQUIRE(predicate(_owner->stats()));
}

std::future<autorelay_manager_fixture::result> autorelay_manager_fixture::refresh(std::chrono::milliseconds timeout) {
   return asio::co_spawn(_runtime.context(), _owner->async_refresh(timeout), asio::use_future);
}
std::future<void> autorelay_manager_fixture::join() { return asio::co_spawn(_runtime.context(), _owner->async_join(), asio::use_future); }
std::future<void> autorelay_manager_fixture::tracked() { return asio::co_spawn(_runtime.context(), _tracker.wait(), asio::use_future); }
std::future<net::diagnostics::autorelay_state> autorelay_manager_fixture::tracked_state() {
   return asio::co_spawn(_runtime.context(), observe_tracked_state(), asio::use_future);
}
asio::awaitable<net::diagnostics::autorelay_state> autorelay_manager_fixture::observe_tracked_state() {
   co_await _tracker.wait();
   // Capture at the lifecycle boundary, not later when the test consumes its future.
   co_return _owner->stats();
}
std::future<void> autorelay_manager_fixture::stop(bool through_lifecycle) {
   auto promise = std::make_shared<std::promise<void>>();
   auto value = promise->get_future();
   asio::post(_runtime.context(), [this, promise, through_lifecycle] {
      if (through_lifecycle) { _tracker.request_stop(); }
      else { _owner->request_stop(); }
      promise->set_value();
   });
   return value;
}

} // namespace forge::tests::p2p
