#include "fixture.hxx"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/system/system_error.hpp>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <functional>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <iostream>
#include <boost/scope/scope_exit.hpp>
#include <thread>
#include <stop_token>

import forge.exceptions;
import forge.net.transport.exceptions;

using namespace tlsn_test;

namespace {

struct temporary_directory {
   temporary_directory() {
      auto name = std::string{FORGE_TLSNOTARY_TEST_OUTPUT "/faults-XXXXXX"};
      check(::mkdtemp(name.data()) != nullptr, "test temporary directory failed");
      path = name;
      for (const auto* mode : {"hang", "crash", "oversize", "truncated", "unknown", "read", "flood", "success-queued"})
         std::filesystem::create_symlink(FORGE_TLSNOTARY_FAULT_WORKER, path / mode);
   }

   ~temporary_directory() {
      std::filesystem::remove_all(path);
   }

   std::filesystem::path path;
};

template <typename Exception, typename Work> void throws(Work work) {
   auto found = false;
   try {
      work();
   } catch (const tlsn::exceptions::failure& error) {
      found = error.value() == Exception::value;
      check(std::string{error.what()}.find("secret-sentinel") == std::string::npos, "error leaked a private sentinel");
   } catch (const Exception& error) {
      found = true;
      check(std::string{error.what()}.find("secret-sentinel") == std::string::npos, "error leaked a private sentinel");
   }

   check(found, "expected typed failure absent");
}

void no_children() {
   auto status = int{};
   const auto value = ::waitpid(-1, &status, WNOHANG);
   check(value == -1 && errno == ECHILD, "worker child remained after cleanup");
}

std::future<tlsn::result> start_verifier(forge::asio::runtime& runtime, tlsn::engine& owner,
                                         forge::net::transport::stream peer, const identity& identity, std::uint64_t id,
                                         std::chrono::milliseconds timeout = 10s, std::stop_token stop = {}) {
   auto request = tlsn::verifier_request{.peer = std::move(peer),
                                         .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                         .options = options(id)};
   request.options.timeout = timeout;
   request.options.stop = stop;
   return asio::co_spawn(runtime.context(), owner.verify(std::move(request)), asio::use_future);
}

void wait_counts(tlsn::engine& owner, unsigned active, unsigned queued) {
   const auto until = std::chrono::steady_clock::now() + 3s;
   while (std::chrono::steady_clock::now() < until) {
      const auto snapshot = owner.snapshot();
      if (snapshot.active == active && snapshot.queued == queued)
         return;
      std::this_thread::sleep_for(1ms);
   }

   throw std::runtime_error{"test admission transition timeout"};
}

void wait_worker_started(const temporary_directory& files) {
   const auto until = std::chrono::steady_clock::now() + 3s;
   while (!std::filesystem::exists(files.path / "hang.started") && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(1ms);
   check(std::filesystem::exists(files.path / "hang.started"), "fault worker did not acknowledge bootstrap");
}

class leaking_stream final : public forge::net::transport::detail::stream_concept {
 public:
   bool valid() const noexcept override {
      return true;
   }

   std::int64_t id() const noexcept override {
      return 9;
   }

   asio::awaitable<void> async_write(std::span<const std::uint8_t>) override {
      throw std::runtime_error{"secret-sentinel write"};
      co_return;
   }

   asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      throw forge::exceptions::context_error{"secret-sentinel read"};
      co_return std::vector<std::uint8_t>{};
   }

   asio::awaitable<void> async_close() override {
      throw forge::exceptions::context_error{"secret-sentinel close"};
      co_return;
   }

   void cancel() override {}
};

class oversized_stream final : public forge::net::transport::detail::stream_concept {
 public:
   bool valid() const noexcept override {
      return true;
   }

   std::int64_t id() const noexcept override {
      return 10;
   }

   asio::awaitable<void> async_write(std::span<const std::uint8_t>) override {
      co_return;
   }

   asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      auto value = std::vector<std::uint8_t>(1);
      value.reserve(65537);
      co_return value;
   }

   asio::awaitable<void> async_close() override {
      co_return;
   }

   void cancel() override {}
};

class delayed_close_stream final : public forge::net::transport::detail::stream_concept {
 public:
   bool valid() const noexcept override {
      return open.load();
   }

   std::int64_t id() const noexcept override {
      return 20;
   }

   asio::awaitable<void> async_write(std::span<const std::uint8_t>) override {
      co_return;
   }

   asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      co_return std::vector<std::uint8_t>{};
   }

   asio::awaitable<void> async_close() override {
      close_entered.store(true);
      while (!close_released.load()) {
         auto epoch = changed.epoch();
         if (!close_released.load())
            static_cast<void>(co_await changed.async_wait(epoch));
      }

      close_done.store(true);
   }

   void cancel() override {
      open.store(false);
   }

   void release() {
      close_released.store(true);
      changed.notify();
   }

   std::atomic<bool> open{true};
   std::atomic<bool> close_entered{false};
   std::atomic<bool> close_released{false};
   std::atomic<bool> close_done{false};
   forge::asio::notification changed;
};

class queued_close_stream final : public forge::net::transport::detail::stream_concept {
 public:
   bool valid() const noexcept override {
      return !closed.load();
   }

   std::int64_t id() const noexcept override {
      return 21;
   }

   asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override {
      const auto lock = std::scoped_lock{mutex};
      queued.insert(queued.end(), bytes.begin(), bytes.end());
      co_return;
   }

   asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      throw std::runtime_error{"ownership fixture did not request a peer read"};
      co_return std::vector<std::uint8_t>{};
   }

   asio::awaitable<void> async_close() override {
      close_entered.store(true);
      changed.notify();
      while (!released.load() && !cancelled.load()) {
         const auto epoch = changed.epoch();
         if (!released.load() && !cancelled.load())
            static_cast<void>(co_await changed.async_wait(epoch));
      }

      {
         const auto lock = std::scoped_lock{mutex};
         delivered.insert(delivered.end(), queued.begin(), queued.end());
         queued.clear();
      }
      closed.store(true);
   }

   void cancel() override {
      {
         const auto lock = std::scoped_lock{mutex};
         discarded += queued.size();
         queued.clear();
      }
      cancelled.store(true);
      changed.notify();
   }

   void release() {
      released.store(true);
      changed.notify();
   }

   asio::awaitable<void> wait_for_close() {
      while (!close_entered.load()) {
         const auto epoch = changed.epoch();
         if (!close_entered.load())
            static_cast<void>(co_await changed.async_wait(epoch));
      }
   }

   std::atomic<bool> close_entered{false};
   std::atomic<bool> released{false};
   std::atomic<bool> cancelled{false};
   std::atomic<bool> closed{false};
   forge::asio::notification changed;
   std::mutex mutex;
   std::vector<std::uint8_t> queued;
   std::vector<std::uint8_t> delivered;
   std::size_t discarded = 0;
};

void successful_teardown_tests(const temporary_directory& files, const identity& identity) {
   // Only host ownership is exercised with this fault worker. Native MPC tests
   // below remain the evidence for authentication and source/disclosure rules.
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1}};
   for (auto mode = 0; mode != 3; ++mode) {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "success-queued"}};
      const auto cleanup = engine_cleanup{runtime, owner};
      auto stream = std::make_shared<queued_close_stream>();
      const auto release = boost::scope::scope_exit{[stream] { stream->release(); }};
      auto stop = std::stop_source{};
      auto configuration = options(90 + mode);
      configuration.reveal_sent.clear();
      configuration.reveal_received.clear();
      configuration.timeout = 2s;
      configuration.stop = stop.get_token();
      auto request = tlsn::verifier_request{.peer = forge::net::transport::detail::stream_access::make_cancelable(
                                                stream, [stream]() noexcept { stream->cancel(); }),
                                            .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                            .options = configuration};
      auto completion = asio::co_spawn(runtime.context(), owner.verify(std::move(request)), asio::use_future);
      auto closing = asio::co_spawn(runtime.context(), stream->wait_for_close(), asio::use_future);
      check(closing.wait_for(3s) == std::future_status::ready, "successful worker did not enter graceful close");
      closing.get();
      check(completion.wait_for(20ms) == std::future_status::timeout, "session bypassed the graceful close barrier");
      check(!stream->cancelled.load(), "successful worker reset its peer before graceful close");
      {
         const auto lock = std::scoped_lock{stream->mutex};
         check(stream->queued.size() == 4 && stream->discarded == 0, "successful worker discarded queued final bytes");
      }
      if (mode == 0) {
         stream->release();
         static_cast<void>(completion.get());
         const auto lock = std::scoped_lock{stream->mutex};
         check(stream->delivered.size() == 4 && stream->discarded == 0, "graceful close did not drain final bytes");
      } else if (mode == 1) {
         throws<tlsn::exceptions::deadline_exceeded>([&] { static_cast<void>(completion.get()); });
         check(stream->cancelled.load(), "graceful close deadline did not cancel its transport");
      } else {
         stop.request_stop();
         throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(completion.get()); });
         check(stream->cancelled.load(), "caller cancellation did not stop graceful close");
      }
      check(stream->closed.load() && owner.snapshot().active == 0, "graceful close retained its ownership barrier");
      forge::asio::blocking::run(runtime, owner.shutdown());
      no_children();
   }
   forge::asio::blocking::run(runtime, compute.shutdown());
}

void cleanup_barrier_tests(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1}};
   auto owner = tlsn::engine{
       runtime, compute.get_executor(), {.worker_path = files.path / "hang", .max_active = 1, .max_queued = 1}};
   const auto owner_cleanup = engine_cleanup{runtime, owner};
   auto sockets = pair(runtime);
   auto first = start_verifier(runtime, owner, std::move(sockets.first), identity, 31);
   wait_counts(owner, 1, 0);
   auto delayed = std::make_shared<delayed_close_stream>();
   const auto release_delayed = boost::scope::scope_exit{[delayed] { delayed->release(); }};
   auto delayed_transport = forge::net::transport::detail::stream_access::make_cancelable(
       delayed, [delayed]() noexcept { delayed->cancel(); });
   auto queued = start_verifier(runtime, owner, std::move(delayed_transport), identity, 32);
   wait_counts(owner, 1, 1);
   check(owner.cancel(32), "queued delayed close cancellation missed");
   const auto until = std::chrono::steady_clock::now() + 3s;
   while (!delayed->close_entered.load() && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(1ms);
   check(delayed->close_entered.load() && !delayed->valid(), "invalidated queued stream close barrier skipped");
   auto draining = asio::co_spawn(runtime.context(), owner.shutdown(), asio::use_future);
   check(draining.wait_for(30ms) == std::future_status::timeout, "shutdown bypassed queued stream close barrier");
   delayed->release();
   static_cast<void>(draining.get());
   check(delayed->close_done.load(), "queued stream terminal close was not joined");
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(queued.get()); });
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(first.get()); });
   no_children();
   forge::asio::blocking::run(runtime, compute.shutdown());
}

void active_close_barrier_test(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "hang"}};
   const auto owner_cleanup = engine_cleanup{runtime, owner};
   auto delayed = std::make_shared<delayed_close_stream>();
   const auto release_delayed = boost::scope::scope_exit{[delayed] { delayed->release(); }};
   auto reentered = std::atomic<bool>{false};
   auto stream =
       forge::net::transport::detail::stream_access::make_cancelable(delayed, [delayed, &owner, &reentered]() noexcept {
          static_cast<void>(owner.snapshot());
          reentered.store(true);
          delayed->cancel();
       });
   std::filesystem::remove(files.path / "hang.started");
   auto cancellation = asio::cancellation_signal{};
   auto request = tlsn::verifier_request{.peer = std::move(stream),
                                         .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                         .options = options(35)};
   auto running = asio::co_spawn(runtime.context(), owner.verify(std::move(request)),
                                 asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
   wait_counts(owner, 1, 0);
   wait_worker_started(files);
   cancellation.emit(asio::cancellation_type::all);
   check(running.wait_for(30ms) == std::future_status::timeout, "ambient cancellation detached an owned session");
   owner.request_stop();
   auto draining = asio::co_spawn(runtime.context(), owner.shutdown(), asio::use_future);
   check(draining.wait_for(30ms) == std::future_status::timeout, "active invalidated stream close barrier skipped");
   check(reentered.load(), "caller cancellation hook was not executed");
   delayed->release();
   static_cast<void>(draining.get());
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(running.get()); });
   no_children();
   forge::asio::blocking::run(runtime, compute.shutdown());
}

void pending_spawn_tests(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1, .max_pending_tasks = 2}};
   auto mutex = std::mutex{};
   auto release = std::condition_variable{};
   auto held = false;
   auto unblock = false;
   auto occupied = compute.get_executor().try_submit({.name = "test-spawn-fence"}, [&] {
      auto lock = std::unique_lock{mutex};
      held = true;
      release.notify_all();
      release.wait(lock, [&] { return unblock; });
   });
   check(occupied.has_value(), "test compute occupancy failed");
   {
      auto lock = std::unique_lock{mutex};
      release.wait(lock, [&] { return held; });
   }

   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "hang"}};
   const auto owner_cleanup = engine_cleanup{runtime, owner};
   auto sockets = pair(runtime);
   auto future = start_verifier(runtime, owner, std::move(sockets.first), identity, 41);
   wait_counts(owner, 1, 0);
   check(owner.cancel(41), "pre-spawn cancellation missed");
   {
      const auto lock = std::scoped_lock{mutex};
      unblock = true;
   }

   release.notify_all();
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(future.get()); });
   forge::asio::blocking::run(runtime, std::move(*occupied).wait());
   forge::asio::blocking::run(runtime, owner.shutdown());
   no_children();
   compute.request_stop();
   auto stopped_owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "hang"}};
   const auto stopped_cleanup = engine_cleanup{runtime, stopped_owner};
   auto more = pair(runtime);
   auto stopped = start_verifier(runtime, stopped_owner, std::move(more.first), identity, 42);
   throws<tlsn::exceptions::transport_failed>([&] { static_cast<void>(stopped.get()); });
   forge::asio::blocking::run(runtime, stopped_owner.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
   no_children();
}

void lifecycle_tests(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   auto owner = tlsn::engine{
       runtime, compute.get_executor(), {.worker_path = files.path / "hang", .max_active = 1, .max_queued = 1}};
   const auto owner_cleanup = engine_cleanup{runtime, owner};
   auto first_pair = pair(runtime);
   auto second_pair = pair(runtime);
   auto third_pair = pair(runtime);
   std::filesystem::remove(files.path / "hang.started");
   auto first = start_verifier(runtime, owner, std::move(first_pair.first), identity, 1);
   wait_counts(owner, 1, 0);
   wait_worker_started(files);
   auto second = start_verifier(runtime, owner, std::move(second_pair.first), identity, 2);
   wait_counts(owner, 1, 1);
   auto third = start_verifier(runtime, owner, std::move(third_pair.first), identity, 3);
   throws<tlsn::exceptions::overloaded>([&] { static_cast<void>(third.get()); });
   check(owner.cancel(2), "queued cancel not found");
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(second.get()); });
   auto queued_timeout_pair = pair(runtime);
   auto queued_timeout = start_verifier(runtime, owner, std::move(queued_timeout_pair.first), identity, 6, 20ms);
   throws<tlsn::exceptions::deadline_exceeded>([&] { static_cast<void>(queued_timeout.get()); });
   check(owner.cancel(1), "active cancel not found");
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(first.get()); });
   check(owner.snapshot().active == 0 && owner.snapshot().queued == 0, "cancel retained admission");
   auto token_pair = pair(runtime);
   auto stop = std::stop_source{};
   std::filesystem::remove(files.path / "hang.started");
   auto token_cancel = start_verifier(runtime, owner, std::move(token_pair.first), identity, 7, 10s, stop.get_token());
   wait_worker_started(files);
   stop.request_stop();
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(token_cancel.get()); });
   auto fourth_pair = pair(runtime);
   auto fourth = start_verifier(runtime, owner, std::move(fourth_pair.first), identity, 4, 50ms);
   throws<tlsn::exceptions::deadline_exceeded>([&] { static_cast<void>(fourth.get()); });
   auto fifth_pair = pair(runtime);
   auto fifth = start_verifier(runtime, owner, std::move(fifth_pair.first), identity, 5);
   wait_counts(owner, 1, 0);
   owner.request_stop();
   forge::asio::blocking::run(runtime, owner.shutdown());
   throws<tlsn::exceptions::cancelled>([&] { static_cast<void>(fifth.get()); });
   no_children();
   forge::asio::blocking::run(runtime, compute.shutdown());
}

void concurrent_admission_test(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 8}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2}};
   auto owner = tlsn::engine{
       runtime, compute.get_executor(), {.worker_path = files.path / "hang", .max_active = 1, .max_queued = 1}};
   const auto cleanup = engine_cleanup{runtime, owner};
   auto peers = std::vector<forge::net::transport::stream>{};
   auto futures = std::vector<std::future<tlsn::result>>{};
   for (unsigned n = 0; n < 32; ++n) {
      auto sockets = pair(runtime);
      peers.push_back(std::move(sockets.second));
      futures.push_back(start_verifier(runtime, owner, std::move(sockets.first), identity, 100 + n));
   }
   const auto until = std::chrono::steady_clock::now() + 3s;
   while (owner.snapshot().accepted + owner.snapshot().rejected < 32 && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(1ms);
   const auto state = owner.snapshot();
   check(state.accepted == 2 && state.rejected == 30 && state.active == 1 && state.queued == 1,
         "concurrent admission exceeded active/queue ownership bounds");
   std::cout << "concurrent admission accepted=" << state.accepted << " rejected=" << state.rejected
             << " active=" << state.active << " queued=" << state.queued << std::endl;
   owner.request_stop();
   forge::asio::blocking::run(runtime, owner.shutdown());
   auto overloads = unsigned{};
   auto stopped = unsigned{};
   for (auto& future : futures) {
      try {
         static_cast<void>(future.get());
      } catch (const tlsn::exceptions::overloaded&) {
         ++overloads;
      } catch (const tlsn::exceptions::cancelled&) {
         ++stopped;
      } catch (const tlsn::exceptions::stopped&) {
         ++stopped;
      }
   }
   check(overloads == 30 && stopped == 2, "concurrent admission terminal outcomes mismatch");
   no_children();
   forge::asio::blocking::run(runtime, compute.shutdown());
}

void foreign_signal_handler(int) {}
void signal_guard_test(const temporary_directory& files, const identity& identity) {
   struct sigaction previous{};
   check(::sigaction(SIGCHLD, nullptr, &previous) == 0, "test SIGCHLD inspection failed");
   const auto restore = boost::scope::scope_exit{[&] { ::sigaction(SIGCHLD, &previous, nullptr); }};
   struct sigaction custom{};
   custom.sa_handler = foreign_signal_handler;
   sigemptyset(&custom.sa_mask);
   check(::sigaction(SIGCHLD, &custom, nullptr) == 0, "test SIGCHLD install failed");
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "hang"}};
   const auto cleanup = engine_cleanup{runtime, owner};
   auto sockets = pair(runtime);
   auto future = start_verifier(runtime, owner, std::move(sockets.first), identity, 70);
   throws<tlsn::exceptions::worker_failed>([&] { static_cast<void>(future.get()); });
   forge::asio::blocking::run(runtime, owner.shutdown());
   struct sigaction observed{};
   check(::sigaction(SIGCHLD, nullptr, &observed) == 0 && observed.sa_handler == foreign_signal_handler,
         "foreign SIGCHLD handler was replaced");
   forge::asio::blocking::run(runtime, compute.shutdown());
   no_children();
}

void lazy_operation_test(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "hang"}};
   const auto cleanup = engine_cleanup{runtime, owner};
   auto sockets = pair(runtime);
   auto value = tlsn::verifier_request{.peer = std::move(sockets.first),
                                       .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                       .options = options(80)};
   auto unpolled = owner.verify(std::move(value));
   forge::asio::blocking::run(runtime, owner.shutdown());
   check(owner.snapshot().accepted == 0, "unpolled operation acquired native resources");
   auto future = asio::co_spawn(runtime.context(), std::move(unpolled), asio::use_future);
   throws<tlsn::exceptions::stopped>([&] { static_cast<void>(future.get()); });
   no_children();
   forge::asio::blocking::run(runtime, compute.shutdown());
}

void fault_tests(const temporary_directory& files, const identity& identity) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2}};
   for (const auto* mode : {"crash", "oversize", "truncated", "unknown"}) {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / mode}};
      const auto owner_cleanup = engine_cleanup{runtime, owner};
      auto streams = pair(runtime);
      auto future = start_verifier(runtime, owner, std::move(streams.first), identity, 1);
      if (std::string{mode} == "oversize" || std::string{mode} == "unknown")
         throws<tlsn::exceptions::protocol_error>([&] { static_cast<void>(future.get()); });
      else if (std::string{mode} == "crash")
         throws<tlsn::exceptions::worker_failed>([&] { static_cast<void>(future.get()); });
      else
         throws<tlsn::exceptions::transport_failed>([&] { static_cast<void>(future.get()); });
      forge::asio::blocking::run(runtime, owner.shutdown());
      no_children();
   }

   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "flood"}};
      const auto cleanup = engine_cleanup{runtime, owner};
      auto model = std::make_shared<delayed_close_stream>();
      model->release();
      auto stream = forge::net::transport::detail::stream_access::make(model);
      auto future = start_verifier(runtime, owner, std::move(stream), identity, 2);
      check(future.wait_for(3s) == std::future_status::ready, "worker credit flood did not terminate promptly");
      auto rejected = false;
      try {
         static_cast<void>(future.get());
      } catch (const tlsn::exceptions::protocol_error&) {
         rejected = true;
      } catch (const tlsn::exceptions::limit_exceeded&) {
         rejected = true;
      }

      check(rejected, "worker credit flood accepted");
      forge::asio::blocking::run(runtime, owner.shutdown());
      no_children();
   }

   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "read"}};
      const auto cleanup = engine_cleanup{runtime, owner};
      auto stream = forge::net::transport::detail::stream_access::make(std::make_shared<oversized_stream>());
      auto future = start_verifier(runtime, owner, std::move(stream), identity, 3);
      throws<tlsn::exceptions::limit_exceeded>([&] { static_cast<void>(future.get()); });
      forge::asio::blocking::run(runtime, owner.shutdown());
      no_children();
   }

   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "read"}};
      const auto owner_cleanup = engine_cleanup{runtime, owner};
      auto stream = forge::net::transport::detail::stream_access::make(std::make_shared<leaking_stream>());
      auto future = start_verifier(runtime, owner, std::move(stream), identity, 4);
      throws<tlsn::exceptions::transport_failed>([&] { static_cast<void>(future.get()); });
      forge::asio::blocking::run(runtime, owner.shutdown());
      no_children();
   }

   {
      auto owner =
          tlsn::engine{runtime, compute.get_executor(), {.worker_path = files.path / "secret-sentinel-missing"}};
      auto streams = pair(runtime);
      auto future = start_verifier(runtime, owner, std::move(streams.first), identity, 5);
      throws<tlsn::exceptions::worker_failed>([&] { static_cast<void>(future.get()); });
      forge::asio::blocking::run(runtime, owner.shutdown());
      no_children();
   }

   forge::asio::blocking::run(runtime, compute.shutdown());
}

void mpc_test(const std::filesystem::path& worker, const identity& identity, unsigned failure) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   auto verifier_runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = worker, .max_active = 1}};
   auto verifier_owner =
       tlsn::engine{verifier_runtime, compute.get_executor(), {.worker_path = worker, .max_active = 1}};
   auto https = https_fixture{runtime, identity};
   auto service = asio::co_spawn(runtime.context(), https.serve(), asio::use_future);
   const auto cleanup = boost::scope::scope_exit{[&] {
      try {
         forge::asio::blocking::run(verifier_runtime, verifier_owner.shutdown());
      } catch (...) {
      }

      try {
         forge::asio::blocking::run(runtime, owner.shutdown());
      } catch (...) {
      }
   }};
   auto streams = pair(runtime);
   auto proof = tlsn::prover_request{.peer = std::move(streams.first),
                                     .server = https.connect(runtime),
                                     .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                     .request_bytes = request(),
                                     .options = options(1)};
   auto verification =
       tlsn::verifier_request{.peer = std::move(streams.second),
                              .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                              .options = options(2)};
   if (failure == 4) {
      proof.options.deferred_decryption = false;
      verification.options.deferred_decryption = false;
   }

   if (failure == 5) {
      proof.options.reveal_sent.clear();
      proof.options.reveal_received.clear();
      verification.options.reveal_sent.clear();
      verification.options.reveal_received.clear();
   }

   if (failure == 1)
      verification.source.trust_roots_der = {make_identity().root};
   if (failure == 2)
      verification.source.server_name = "other.local";
   if (failure == 3)
      verification.options.reveal_sent = {{0, 19}};
   auto verified =
       asio::co_spawn(verifier_runtime.context(), verifier_owner.verify(std::move(verification)), asio::use_future);
   auto proved = asio::co_spawn(runtime.context(), owner.prove(std::move(proof)), asio::use_future);
   const auto until = std::chrono::steady_clock::now() + 100s;
   while ((verified.wait_for(0ms) != std::future_status::ready || proved.wait_for(0ms) != std::future_status::ready) &&
          std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(10ms);
   if (verified.wait_for(0ms) != std::future_status::ready || proved.wait_for(0ms) != std::future_status::ready) {
      const auto prover_state = owner.snapshot();
      const auto verifier_state = verifier_owner.snapshot();
      std::cerr << "MPC timeout prover-active=" << prover_state.active << " verifier-active=" << verifier_state.active
                << " prover-ready=" << (proved.wait_for(0ms) == std::future_status::ready)
                << " verifier-ready=" << (verified.wait_for(0ms) == std::future_status::ready) << std::endl;
      if (proved.wait_for(0ms) == std::future_status::ready)
         static_cast<void>(proved.get());
      if (verified.wait_for(0ms) == std::future_status::ready)
         static_cast<void>(verified.get());
      throw std::runtime_error{"MPC completion timeout"};
   }

   if (failure >= 1 && failure <= 3) {
      auto rejected = false;
      try {
         static_cast<void>(verified.get());
      } catch (const tlsn::exceptions::verification_failed&) {
         rejected = true;
      } catch (const tlsn::exceptions::failure& error) {
         rejected = error.value() == tlsn::exceptions::code::verification_failed;
      }

      check(rejected, "incorrect source or disclosure mask accepted");
      try {
         static_cast<void>(proved.get());
      } catch (...) {
      }
   } else {
      const auto result = verified.get();
      const auto prover_result = proved.get();
      check(result.server_name == "tlsn.local" && result.sent_size == request().size() && result.received_size > 0,
            "authenticated source or transcript lengths absent");
      if (failure == 5) {
         check(result.sent.empty() && result.received.empty(), "identity-only profile disclosed plaintext");
      } else {
         check(result.sent.size() == 1 && result.received.size() == 1, "authenticated result ranges absent");
         check(result.sent.front().bytes.size() == 18, "authenticated request range mismatch");
         const auto response = std::string{result.received.front().bytes.begin(), result.received.front().bytes.end()};
         check(response.find("7318921") != std::string::npos && response.find("secret-sentinel") == std::string::npos,
               "authenticated response mismatch or hidden key disclosed");
      }
      check(prover_result.sent.empty() && prover_result.received.empty(), "prover returned authenticated data");
   }

   static_cast<void>(service.get());
   forge::asio::blocking::run(verifier_runtime, verifier_owner.shutdown());
   forge::asio::blocking::run(runtime, owner.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
   no_children();
}
} // namespace
int main(int argc, char** argv) {
   try {
      const auto worker = std::filesystem::path{argc > 1 ? argv[1] : FORGE_TLSNOTARY_WORKER_PATH};
      const auto identity = make_identity();
      const auto files = temporary_directory{};
      std::cout << "stage successful-teardown" << std::endl;
      successful_teardown_tests(files, identity);
      std::cout << "stage lifecycle" << std::endl;
      lifecycle_tests(files, identity);
      std::cout << "stage queued-close" << std::endl;
      cleanup_barrier_tests(files, identity);
      std::cout << "stage active-close" << std::endl;
      active_close_barrier_test(files, identity);
      std::cout << "stage spawn-fence" << std::endl;
      pending_spawn_tests(files, identity);
      std::cout << "stage concurrent-admission" << std::endl;
      concurrent_admission_test(files, identity);
      std::cout << "stage signal-guard" << std::endl;
      signal_guard_test(files, identity);
      std::cout << "stage lazy-operation" << std::endl;
      lazy_operation_test(files, identity);
      std::cout << "stage faults" << std::endl;
      fault_tests(files, identity);
      const auto cases = std::array{"deferred", "wrong-root", "wrong-server", "wrong-mask", "online", "identity-only"};
      for (unsigned failure = 0; failure < cases.size(); ++failure) {
         std::cout << "stage MPC case " << failure << " " << cases[failure] << std::endl;
         mpc_test(worker, identity, failure);
         std::cout << "MPC " << cases[failure] << " PASS" << std::endl;
      }

      std::cout << "TLSNotary native sessions, negative source/mask, lifecycle and faults PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "TLSNotary test failed: " << error.what() << '\n';
      return 1;
   }
}
