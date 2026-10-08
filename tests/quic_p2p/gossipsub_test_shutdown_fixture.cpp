module;

#include <atomic>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>
#include "libp2p_identity_fixture.hxx"

#if defined(__unix__) || defined(__APPLE__)
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

module forge.net.p2p.node;
import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.tcp.connection;
import forge.net.tcp.listener;

#include "gossipsub_test_shutdown.hxx"
#include "gossipsub_test_shutdown_fixture.hxx"

namespace forge::tests::p2p {
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

namespace {
p2p::node::options shutdown_options(std::string name, std::shared_ptr<p2p::peer_store::persistence> store) {
   const auto identity = make_identity_fixture(name);
   auto options = p2p::node::options{.certificate_pem = identity.certificate_pem,
      .private_key_pem = identity.private_key_pem, .capabilities = {.bits = p2p::capabilities::pubsub}};
   options.peer_state.persistence = std::move(store);
   options.dht_profiles.clear();
   options.relay_policy.client_enabled = false;
   options.relay_policy.auto_discovery_enabled = false;
   options.path_policy.allow_relay = false;
   options.path_policy.allow_hole_punch = false;
   options.limits.topology.query_timeout = 2s;
   return options;
}
} // namespace

gossipsub_test_shutdown_fixture::close_store::close_store()
    : _delegate(p2p::peer_store::make_memory_persistence()) {}

boost::asio::awaitable<p2p::peer_store::hydration_page>
gossipsub_test_shutdown_fixture::close_store::async_hydrate(p2p::peer_store::hydration_request request) {
   co_return co_await _delegate->async_hydrate(std::move(request));
}

boost::asio::awaitable<p2p::peer_store::apply_result>
gossipsub_test_shutdown_fixture::close_store::async_apply(p2p::peer_store::mutation_batch batch) {
   co_return co_await _delegate->async_apply(std::move(batch));
}

boost::asio::awaitable<p2p::peer_store::prune_result>
gossipsub_test_shutdown_fixture::close_store::async_prune_expired(std::chrono::system_clock::time_point now,
                                                                 std::size_t limit) {
   co_return co_await _delegate->async_prune_expired(now, limit);
}

boost::asio::awaitable<void> gossipsub_test_shutdown_fixture::close_store::async_flush() {
   co_await _delegate->async_flush();
}

boost::asio::awaitable<void> gossipsub_test_shutdown_fixture::close_store::async_close() {
   close_calls.fetch_add(1);
   if (fail_next_close.exchange(false)) { throw std::runtime_error{"injected peer-store close failure"}; }
   co_await _delegate->async_close();
}

gossipsub_test_shutdown_fixture::gossipsub_test_shutdown_fixture()
    : runtime(forge::asio::runtime_options{.worker_threads = 4}), first_store(std::make_shared<close_store>()),
      second_store(std::make_shared<close_store>()), first(runtime, shutdown_options("shutdown-first", first_store)),
      second(runtime, shutdown_options("shutdown-second", second_store)),
      _listener(runtime.context().get_executor(), p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0").transport),
      _worker_executor(boost::asio::make_strand(runtime.context())), _start(std::make_shared<std::atomic_bool>()) {
   workers.reserve(4);
   _cancellations.reserve(4);
}

void gossipsub_test_shutdown_fixture::admit_cold_subscriptions() {
   first.peers().learn_endpoint(second.local_peer(), p2p::endpoint{.transport = _listener.local_endpoint()},
                                {.bits = p2p::capabilities::pubsub});
   _accepting = boost::asio::co_spawn(runtime.context(), _listener.async_accept_connection(), boost::asio::use_future);
   for (auto index = 0U; index < 4U; ++index) {
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
      _cancellations.push_back(cancellation);
      workers.push_back(boost::asio::co_spawn(_worker_executor,
          [this, index, start = _start]() -> boost::asio::awaitable<p2p::pubsub::subscription> {
             // The initiator owns a real pending TCP handshake; the other
             // subscriptions remain behind the test start gate until unwind.
             while (index != 0 && !start->load(std::memory_order_acquire)) {
                co_await boost::asio::post(boost::asio::use_awaitable);
             }
             co_return co_await first.async_subscribe({"forge.pubsub.shutdown." + std::to_string(index)},
                 [](p2p::pubsub::event) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
                    co_return p2p::pubsub::validation_result::accept;
                 });
          }, boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future)));
   }
   if (_accepting.wait_for(5s) != std::future_status::ready) {
      throw std::runtime_error{"cold subscription did not reach its native TCP listener"};
   }
   _accepted = _accepting.get();
   if (workers.front().wait_for(0s) == std::future_status::ready) {
      throw std::runtime_error{"cold subscription was not pending at the native handshake barrier"};
   }
}

void gossipsub_test_shutdown_fixture::release_workers() noexcept {
   _start->store(true, std::memory_order_release);
}

void gossipsub_test_shutdown_fixture::cancel_workers() {
   ++cancellations;
   for (const auto& signal : _cancellations) {
      boost::asio::post(_worker_executor, [signal] { signal->emit(boost::asio::cancellation_type::terminal); });
   }
}

void gossipsub_test_shutdown_fixture::join_workers(std::chrono::steady_clock::time_point deadline) {
   ++barriers;
   const auto phase = first.lifecycle_state().phase;
   barrier_before_stop = barrier_before_stop && phase != p2p::lifecycle_phase::stopping &&
                         phase != p2p::lifecycle_phase::stopped;
   for (auto& worker : workers) {
      if (worker.valid() && worker.wait_until(deadline) != std::future_status::ready) {
         throw std::runtime_error{"cold subscription worker did not join within its test budget"};
      }
   }
}

bool gossipsub_test_shutdown_fixture::workers_ready() const {
   for (const auto& worker : workers) {
      if (worker.valid() && worker.wait_for(0s) != std::future_status::ready) { return false; }
   }
   return true;
}

#if defined(__unix__) || defined(__APPLE__)
gossipsub_test_shutdown_fixture::child_result
gossipsub_test_shutdown_fixture::run_exhaustion_child(const std::string& executable, child_mode mode,
                                                     std::chrono::milliseconds wait_budget,
                                                     child_result* exception_cleanup) {
   if (wait_budget <= 0ms) { throw std::invalid_argument{"child wait budget must be positive"}; }
   auto context = boost::asio::io_context{};
   auto exited = boost::asio::signal_set{context, SIGCHLD};
   auto deadline = boost::asio::steady_timer{context, wait_budget};
   auto terminal_deadline = boost::asio::steady_timer{context, deadline.expiry() + 2s};
   auto result = child_result{};
   auto process = pid_t{-1};
   auto finished = false;
   auto wait_error = 0;
   const auto finish = [&] {
      finished = true;
      static_cast<void>(deadline.cancel());
      static_cast<void>(terminal_deadline.cancel());
      static_cast<void>(exited.cancel());
   };
   const auto reap = [&] {
      const auto observed = ::waitpid(process, &result.status, WNOHANG);
      if (observed == process) {
         process = -1;
         result.reaped = true;
         finish();
      } else if (observed < 0) {
         wait_error = errno;
         // ECHILD is not a terminal receipt, but this PID is no longer owned:
         // never send SIGKILL to a possibly reused PID after an external reap.
         if (wait_error == ECHILD) { process = -1; }
         throw std::system_error{wait_error, std::generic_category(), "reap shutdown exhaustion child"};
      }
   };
   auto watch = std::function<void(boost::system::error_code, int)>{};
   watch = [&](boost::system::error_code error, int) {
      if (finished) { return; }
      if (error) {
         wait_error = error.value();
         throw boost::system::system_error{error, "observe shutdown exhaustion child"};
      }
      reap();
      if (!finished) { exited.async_wait(watch); }
   };
   // Install the signal wait before spawn; Asio also queues signals between
   // waits, so an exit before context.run() cannot lose the child notification.
   exited.async_wait(watch);
   deadline.async_wait([&](boost::system::error_code error) {
      if (error || finished) { return; }
      result.timed_out = true;
      if (::kill(process, SIGKILL) != 0 && errno != ESRCH) {
         wait_error = errno;
         throw std::system_error{wait_error, std::generic_category(), "kill shutdown exhaustion child"};
      }
      reap();
   });
   // Prearm this wait before exec as well: SIGKILL is only a request, not a
   // terminal receipt. Missing SIGCHLD/reap cannot leave context.run() unbounded.
   terminal_deadline.async_wait([&](boost::system::error_code error) {
      if (error || finished) { return; }
      reap();
      if (process > 0) { gossipsub_test_shutdown::fail_closed(); }
   });
   auto arguments = std::array<std::string, 4>{executable,
      mode == child_mode::exhaustion ? "--run_test=p2p_gossipsub_shutdown_child/unfinished_join_exhaustion" :
                                      "--run_test=p2p_gossipsub_shutdown_child/wait_for_parent_cleanup",
      "--report_level=no", "--log_level=nothing"};
   auto native = std::array<char*, 5>{};
   for (auto index = std::size_t{}; index < arguments.size(); ++index) { native[index] = arguments[index].data(); }
   auto terminate_and_reap = [&](pid_t* pid) noexcept {
      if (*pid <= 0) { return; }
      static_cast<void>(::kill(*pid, SIGKILL));
      const auto cleanup_until = std::min(std::chrono::steady_clock::now() + 2s, terminal_deadline.expiry());
      // Handler failures may consume a signal wait. Drive the already prepared
      // context only to this fixed boundary, then make one final nonblocking reap.
      for (auto attempt = 0U; attempt < 2U && *pid > 0; ++attempt) {
         try {
            reap();
            if (*pid <= 0) { break; }
            if (context.stopped()) { context.restart(); }
            context.run_until(cleanup_until);
         } catch (...) {
            // Preserve the primary launcher exception; unreaped ownership below
            // is fatal, never silently released or marked as successfully joined.
         }
         if (std::chrono::steady_clock::now() >= cleanup_until) { break; }
      }
      if (*pid > 0) { try { reap(); } catch (...) {} }
      if (*pid > 0) { gossipsub_test_shutdown::fail_closed(); }
      if (exception_cleanup) { *exception_cleanup = result; }
   };
   // Declare the guard after every callback it can drive during exception cleanup.
   auto process_guard = std::unique_ptr<pid_t, decltype(terminate_and_reap)>{&process, terminate_and_reap};
   const auto started = ::posix_spawn(&process, executable.c_str(), nullptr, nullptr, native.data(), environ);
   if (started != 0) { throw std::system_error{started, std::generic_category(), "spawn shutdown exhaustion child"}; }
   if (mode == child_mode::fail_after_spawn) { throw std::logic_error{"injected child launcher failure after exec"}; }
   reap();
   context.run();
   if (wait_error != 0) { throw std::system_error{wait_error, std::generic_category(), "join shutdown exhaustion child"}; }
   if (!result.reaped) { gossipsub_test_shutdown::fail_closed(); }
   return result;
}
#endif

gossipsub_test_shutdown_fixture::~gossipsub_test_shutdown_fixture() noexcept {
   {
      auto shutdown = gossipsub_test_shutdown{runtime, first, second,
         [this] { release_workers(); }, [this](auto deadline) { join_workers(deadline); },
         [this] { cancel_workers(); }};
   }
   try { _listener.close(); }
   catch (const std::exception& error) { BOOST_ERROR(error.what()); }
   catch (...) { BOOST_ERROR("native shutdown fixture listener stop failed"); }
   if (_accepting.valid()) {
      if (_accepting.wait_for(5s) == std::future_status::ready) {
         try { _accepted = _accepting.get(); } catch (...) {}
      } else { gossipsub_test_shutdown::fail_closed(); }
   }
   if (_accepted.valid()) {
      try {
         auto closing = boost::asio::co_spawn(runtime.context(), _accepted.async_close(), boost::asio::use_future);
         if (closing.wait_for(5s) == std::future_status::ready) { closing.get(); }
         else { gossipsub_test_shutdown::fail_closed(); }
      } catch (const std::exception& error) { BOOST_ERROR(error.what()); }
      catch (...) { BOOST_ERROR("native shutdown fixture socket close failed"); }
   }
   try {
      auto closing = boost::asio::co_spawn(runtime.context(), _listener.async_close(), boost::asio::use_future);
      if (closing.wait_for(5s) == std::future_status::ready) { closing.get(); }
      else { gossipsub_test_shutdown::fail_closed(); }
   } catch (const std::exception& error) { BOOST_ERROR(error.what()); }
   catch (...) { BOOST_ERROR("native shutdown fixture listener close failed"); }
}

} // namespace forge::tests::p2p
