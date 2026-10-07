module;

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <stdexcept>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

module forge.net.p2p.node;
import forge.asio.runtime;

#include "gossipsub_test_shutdown.hxx"

namespace forge::tests::p2p {

gossipsub_test_shutdown::gossipsub_test_shutdown(forge::asio::runtime& runtime, forge::net::p2p::node& first,
                                                forge::net::p2p::node& second, std::function<void()> release_workers,
                                                std::function<void(std::chrono::steady_clock::time_point)> join_workers,
                                                std::function<void()> cancel_workers,
                                                std::chrono::milliseconds join_timeout)
    : _runtime(runtime), _first(first), _second(second), _release_workers(std::move(release_workers)),
      _join_workers(std::move(join_workers)), _cancel_workers(std::move(cancel_workers)), _join_timeout(join_timeout) {
   if (_join_timeout <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument{"GossipSub test shutdown requires a positive join budget"};
   }
}

bool gossipsub_test_shutdown::joined() const noexcept {
   return _workers_released && _workers_joined && _owners_joined[0] && _owners_joined[1];
}

[[noreturn]] void gossipsub_test_shutdown::fail_closed() noexcept {
   std::fputs("FATAL: GossipSub test cleanup exhausted retries with unjoined owners/workers\n", stderr);
   std::fflush(stderr);
   // Do not unwind local owners or abandon their executor while a coroutine
   // may still refer to them. This policy belongs only to the test process.
   std::_Exit(incomplete_join_exit_code);
}

void gossipsub_test_shutdown::join() {
   if (joined()) { return; }
   auto failure = std::exception_ptr{};
   const auto remember_failure = [&] { if (!failure) { failure = std::current_exception(); } };
   const auto join_workers = [&] {
      if (_workers_joined) { return; }
      try {
         if (_join_workers) { _join_workers(std::chrono::steady_clock::now() + _join_timeout); }
         _workers_joined = true;
      } catch (...) { remember_failure(); }
   };
   if (!_workers_released) {
      try {
         if (_release_workers) { _release_workers(); }
         _workers_released = true;
      } catch (...) { remember_failure(); }
   }
   // Cold connection singleflight workers must enter their terminal guard before
   // lifecycle stop can skip an admitted but not yet started task.
   if (_workers_released) { join_workers(); }
   if (!_workers_joined && _cancel_workers) {
      try { _cancel_workers(); } catch (...) { remember_failure(); }
      join_workers();
   }

   const auto owners = std::array{&_first, &_second};
   for (auto index = std::size_t{}; index < owners.size(); ++index) {
      if (_owners_joined[index] || _stops[index].valid()) { continue; }
      owners[index]->request_stop();
      try {
         _stops[index] = boost::asio::co_spawn(_runtime.context(), owners[index]->async_stop(),
                                               boost::asio::use_future);
      } catch (...) { remember_failure(); }
   }
   const auto deadline = std::chrono::steady_clock::now() + _join_timeout;
   for (auto index = std::size_t{}; index < owners.size(); ++index) {
      if (_owners_joined[index] || !_stops[index].valid()) { continue; }
      try {
         if (_stops[index].wait_until(deadline) != std::future_status::ready) {
            throw std::runtime_error{"GossipSub node teardown did not join within its test budget"};
         }
         _stops[index].get();
         _owners_joined[index] = true;
      } catch (...) { remember_failure(); }
   }
   // Even a failed first barrier must settle its futures after async_stop has
   // closed the node-owned connection gates. Do not leave raw node references live.
   join_workers();
   if (failure) { std::rethrow_exception(failure); }
}

gossipsub_test_shutdown::~gossipsub_test_shutdown() noexcept {
   auto failure = std::exception_ptr{};
   for (auto attempt = 0U; attempt < 2U; ++attempt) {
      try { join(); return; }
      catch (...) {
         failure = std::current_exception();
         if (joined()) { break; }
      }
   }
   if (!joined()) {
      try { if (failure) { std::rethrow_exception(failure); } }
      catch (const std::exception& error) {
         std::fprintf(stderr, "GossipSub cleanup exception: %.2048s\n", error.what());
      }
      catch (...) { std::fputs("GossipSub cleanup has a non-standard exception\n", stderr); }
      std::fprintf(stderr, "GossipSub cleanup state: released=%d workers=%d owners=%d,%d futures=%d,%d\n",
          _workers_released, _workers_joined, _owners_joined[0], _owners_joined[1],
          _stops[0].valid(), _stops[1].valid());
      fail_closed();
   }
   try { if (failure) { std::rethrow_exception(failure); } }
   catch (const std::exception& error) { BOOST_ERROR("GossipSub native teardown failed: " << error.what()); }
   catch (...) { BOOST_ERROR("GossipSub native teardown failed with a non-standard exception"); }
}

} // namespace forge::tests::p2p
