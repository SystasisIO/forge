#include <boost/test/unit_test.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/scope/scope_exit.hpp>
#include <array>
#include <barrier>
#include <chrono>
#include <csignal>
#include <exception>
#include <future>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <sys/wait.h>

import forge.asio.child_signal_registration;
import forge.asio.runtime;

#include "../../libraries/asio/details/child_signal_registration_test_access.hxx"

namespace {
namespace asio = boost::asio;
using guard = forge::asio::child_signal_registration;
using access = forge::asio::detail::child_signal_registration_test_access;
using namespace std::chrono_literals;

struct sigaction snapshot() {
   struct sigaction value{};
   if (::sigaction(SIGCHLD, nullptr, &value) != 0)
      throw std::runtime_error{"test child signal query failed"};
   return value;
}
void install(const struct sigaction& value) {
   if (::sigaction(SIGCHLD, &value, nullptr) != 0)
      throw std::runtime_error{"test child signal installation failed"};
}
bool same(const struct sigaction& a, const struct sigaction& b) {
   if (a.sa_flags != b.sa_flags)
      return false;
   if ((a.sa_flags & SA_SIGINFO) != 0 ? a.sa_sigaction != b.sa_sigaction : a.sa_handler != b.sa_handler)
      return false;
   for (auto signal = 1; signal < NSIG; ++signal)
      if (sigismember(&a.sa_mask, signal) != sigismember(&b.sa_mask, signal))
         return false;
   return true;
}
void foreign_handler(int) {}
void foreign_siginfo(int, siginfo_t*, void*) {}

pid_t spawn(const char* path, const char* argument = nullptr) {
   auto arguments = std::array<char*, 3>{const_cast<char*>(path), const_cast<char*>(argument), nullptr};
   auto environment = std::array<char*, 1>{nullptr};
   auto pid = pid_t{};
   if (::posix_spawn(&pid, path, nullptr, nullptr, arguments.data(), environment.data()) != 0)
      throw std::runtime_error{"test child spawn failed"};
   return pid;
}

asio::awaitable<int> reap(pid_t pid) {
   auto child = boost::process::v2::process_handle{co_await asio::this_coro::executor, pid};
   auto status = boost::process::v2::native_exit_code_type{W_STOPCODE(SIGSTOP)};
   co_await child.async_wait(status, asio::use_awaitable);
   co_return status;
}

asio::awaitable<int> stop_and_reap(asio::strand<asio::io_context::executor_type> strand, pid_t pid,
                                   std::stop_token stop, std::promise<void>& armed) {
   auto authority = std::make_shared<pid_t>(pid);
   auto child = boost::process::v2::process_handle{strand, pid};
   const auto registration = std::stop_callback{stop, [strand, authority] {
                                                   asio::post(strand, [authority] {
                                                      // Sole child waiter and stop signal share one strand. A queued
                                                      // stop can never signal an OS-reused PID after wait/reap clears
                                                      // authority.
                                                      if (*authority > 0)
                                                         ::kill(*authority, SIGTERM);
                                                   });
                                                }};
   armed.set_value();
   auto result = boost::process::v2::native_exit_code_type{W_STOPCODE(SIGSTOP)};
   co_await child.async_wait(result, asio::use_awaitable);
   *authority = -1;
   co_return result;
}
} // namespace

BOOST_AUTO_TEST_SUITE(child_signal_registration)

BOOST_AUTO_TEST_CASE(empty_executor_is_rejected_before_native_registration) {
   const auto original = snapshot();
   auto owner = guard{};
   BOOST_CHECK_THROW(owner.acquire({}), forge::asio::exceptions::invalid_options);
   BOOST_CHECK(!owner.registered());
   BOOST_CHECK(same(snapshot(), original));
   owner.release();
}

BOOST_AUTO_TEST_CASE(concurrent_owners_share_checked_handler_across_runtimes) {
   const auto original = snapshot();
   auto first_runtime = forge::asio::runtime{};
   auto second_runtime = forge::asio::runtime{};
   auto first = guard{};
   auto second = guard{};
   const auto cleanup = boost::scope::scope_exit{[&] {
      second.release();
      first.release();
   }};
   auto ready = std::barrier{2};
   auto first_error = std::exception_ptr{};
   auto second_error = std::exception_ptr{};
   auto a = std::jthread{[&] {
      ready.arrive_and_wait();
      try {
         first.acquire(first_runtime.context().get_executor());
      } catch (...) {
         first_error = std::current_exception();
      }
   }};
   auto b = std::jthread{[&] {
      ready.arrive_and_wait();
      try {
         second.acquire(second_runtime.context().get_executor());
      } catch (...) {
         second_error = std::current_exception();
      }
   }};
   a.join();
   b.join();
   BOOST_REQUIRE(!first_error);
   BOOST_REQUIRE(!second_error);
   BOOST_CHECK(first.registered());
   BOOST_CHECK(second.registered());
   const auto owned = snapshot();
   BOOST_CHECK(owned.sa_handler != SIG_DFL && owned.sa_handler != SIG_IGN);
   BOOST_CHECK_THROW(first.acquire(first_runtime.context().get_executor()), forge::asio::exceptions::invalid_state);
   auto a_wait = asio::co_spawn(first_runtime.context(), reap(spawn("/usr/bin/true")), asio::use_future);
   auto b_wait = asio::co_spawn(second_runtime.context(), reap(spawn("/usr/bin/true")), asio::use_future);
   BOOST_REQUIRE(a_wait.wait_for(3s) == std::future_status::ready);
   BOOST_REQUIRE(b_wait.wait_for(3s) == std::future_status::ready);
   BOOST_TEST(a_wait.get() == 0);
   BOOST_TEST(b_wait.get() == 0);
   first.release();
   first.release();
   BOOST_CHECK(!first.registered());
   BOOST_CHECK(same(snapshot(), owned));
   auto still_live = asio::co_spawn(second_runtime.context(), reap(spawn("/usr/bin/true")), asio::use_future);
   BOOST_REQUIRE(still_live.wait_for(3s) == std::future_status::ready);
   BOOST_TEST(still_live.get() == 0);
   second.release();
   BOOST_CHECK(same(snapshot(), original));
}

BOOST_AUTO_TEST_CASE(foreign_dispositions_are_rejected_without_mutation) {
   const auto original = snapshot();
   const auto restore = boost::scope::scope_exit{[&] { install(original); }};
   auto runtime = forge::asio::runtime{};
   auto owner = guard{};
   for (auto mode = 0; mode != 6; ++mode) {
      auto foreign = original;
      switch (mode) {
      case 0:
         foreign.sa_handler = foreign_handler;
         break;
      case 1:
         foreign.sa_handler = SIG_IGN;
         break;
      case 2:
         foreign.sa_flags = SA_NOCLDWAIT;
         break;
      case 3:
         foreign.sa_flags = SA_SIGINFO;
         foreign.sa_sigaction = foreign_siginfo;
         break;
      case 4:
         foreign.sa_flags = SA_NOCLDSTOP;
         break;
      case 5:
         sigaddset(&foreign.sa_mask, SIGUSR1);
         break;
      }
      install(foreign);
      const auto expected = snapshot();
      BOOST_CHECK_THROW(owner.acquire(runtime.context().get_executor()), forge::asio::exceptions::rejected);
      BOOST_CHECK(!owner.registered());
      BOOST_CHECK(same(snapshot(), expected));
      owner.release();
   }
}

BOOST_AUTO_TEST_CASE(failed_initialization_and_foreign_change_retain_same_registration) {
   const auto original = snapshot();
   auto runtime = forge::asio::runtime{};
   auto owner = guard{};
   const auto release = boost::scope::scope_exit{[&] { owner.release(); }};
   BOOST_CHECK_THROW(access::acquire(owner, runtime.context().get_executor(), [] { throw std::bad_alloc{}; }),
                     std::bad_alloc);
   BOOST_CHECK(owner.registered());
   const auto owned = snapshot();
   auto foreign = original;
   foreign.sa_handler = foreign_handler;
   install(foreign);
   auto restore = boost::scope::scope_exit{[&] { install(owned); }};
   const auto expected = snapshot();
   BOOST_CHECK_THROW(owner.release(), forge::asio::exceptions::invalid_state);
   BOOST_CHECK(owner.registered());
   BOOST_CHECK(same(snapshot(), expected));
   install(owned);
   restore.set_active(false);
   owner.release();
   BOOST_CHECK(!owner.registered());
   BOOST_CHECK(same(snapshot(), original));
}

BOOST_AUTO_TEST_CASE(pending_child_stop_joins_reaper_before_registration_release) {
   const auto original = snapshot();
   auto runtime = forge::asio::runtime{};
   auto owner = guard{};
   owner.acquire(runtime.context().get_executor());
   const auto cleanup = boost::scope::scope_exit{[&] { owner.release(); }};
   auto strand = asio::make_strand(runtime.context());
   auto stop = std::stop_source{};
   auto armed = std::promise<void>{};
   auto ready = armed.get_future();
   auto work = asio::co_spawn(strand, stop_and_reap(strand, spawn("/bin/sleep", "30"), stop.get_token(), armed),
                              asio::use_future);
   BOOST_REQUIRE(ready.wait_for(3s) == std::future_status::ready);
   BOOST_CHECK(owner.registered());
   stop.request_stop();
   BOOST_REQUIRE(work.wait_for(3s) == std::future_status::ready);
   const auto status = work.get();
   BOOST_CHECK(WIFSIGNALED(status));
   BOOST_TEST(WTERMSIG(status) == SIGTERM);
   owner.release();
   runtime.stop();
   BOOST_CHECK(!owner.registered());
   BOOST_CHECK(same(snapshot(), original));
}

BOOST_AUTO_TEST_SUITE_END()
