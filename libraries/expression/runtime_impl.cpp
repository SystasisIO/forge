module;

#include "details/cancellation_delivery.hxx"

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <span>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>

module forge.expression.program;

import forge.expression.exceptions;
import forge.asio.blocking;
import forge.asio.child_signal_registration;
import forge.asio.gate;
import forge.asio.notification;

#include "details/runtime_impl.hxx"

namespace forge::expression {
namespace {

void spawn_check(int error) {
   if (error != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker launch setup failed");
}

int isolated_descriptor(int fd) {
   const auto result = ::fcntl(fd, F_DUPFD_CLOEXEC, 4);
   if (result < 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker descriptor isolation failed");
   return result;
}

[[noreturn]] void exchange_error(std::exception_ptr error) {
   try {
      std::rethrow_exception(error);
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression control allocation failed");
   } catch (const boost::system::system_error& failure) {
      if (failure.code() == boost::asio::error::operation_aborted)
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression exchange cancelled");
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker transport failed");
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker transport failed");
   }
}

} // namespace

runtime::impl::impl(options value, forge::asio::runtime* injected)
    : configuration(std::move(value)),
      owned_executor(injected ? nullptr
                              : std::make_unique<forge::asio::runtime>(
                                    forge::asio::runtime_options{.worker_threads = 1, .thread_name = "expression-io"})),
      executor(injected ? *injected : *owned_executor), strand(boost::asio::make_strand(executor.context())),
      socket(strand) {
   std::error_code error;
   if (!configuration.helper.is_absolute() || !std::filesystem::is_regular_file(configuration.helper, error) ||
       ::access(configuration.helper.c_str(), X_OK) != 0 || configuration.heap_bytes < 65536U ||
       configuration.heap_bytes > 64U * 1024U * 1024U || configuration.timeout.count() <= 0 ||
       configuration.timeout > std::chrono::hours{1})
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "invalid expression worker configuration");
}

runtime::impl::~impl() {
   shutdown_sync();
}

void runtime::impl::require_host() const {
   if (executor.context().stopped() || executor.context().get_executor().running_in_this_thread())
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "synchronous expression calls require a live host boundary");
}

void runtime::impl::initialize_sync() {
   require_host();
   forge::asio::blocking::run(executor, boost::asio::co_spawn(strand, initialize(), boost::asio::use_awaitable));
}

boost::asio::awaitable<void> runtime::impl::initialize() {
   std::exception_ptr failure;
   try {
      // Acquire outside the throwing impl constructor: even a registration
      // verification failure retains this same guard for explicit cleanup.
      try {
         signal_owner.acquire(strand);
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression requires compatible child signal ownership");
      }
      launch();
      process = std::make_unique<boost::process::v2::process_handle>(strand, pid);
      boost::system::error_code error;
      socket.assign(boost::asio::local::stream_protocol{}, channel, error);
      if (error)
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker channel adoption failed");
      channel = -1;
      worker_protocol::request hello;
      hello.op = worker_protocol::operation::hello;
      const auto reply = co_await invoke(std::move(hello), {});
      if (reply.code != worker_protocol::status::ok || reply.heap_cap != configuration.heap_bytes)
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker memory admission failed");
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await shutdown();
      exchange_error(failure);
   }
}

void runtime::impl::launch() {
   std::array<int, 2> endpoints{-1, -1};
   std::array<int, 2> fault_endpoints{-1, -1};
   const auto cleanup = boost::scope::scope_exit{[&] {
      for (auto fd : endpoints)
         if (fd >= 0)
            ::close(fd);
      for (auto fd : fault_endpoints)
         if (fd >= 0)
            ::close(fd);
   }};
   if (::socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints.data()) != 0 || ::pipe(fault_endpoints.data()) != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker channels unavailable");
   // Posix spawn actions must never alias a destination (0..3), including in
   // applications that closed standard descriptors before creating a runtime.
   for (auto& fd : endpoints) {
      const auto old = fd;
      fd = isolated_descriptor(old);
      ::close(old);
   }
   for (auto& fd : fault_endpoints) {
      const auto old = fd;
      fd = isolated_descriptor(old);
      ::close(old);
   }
   const auto no_sigpipe = 1;
   if (::setsockopt(endpoints[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0 ||
       ::fcntl(endpoints[0], F_SETFL, O_NONBLOCK) != 0 || ::fcntl(fault_endpoints[0], F_SETFL, O_NONBLOCK) != 0 ||
       ::fcntl(fault_endpoints[1], F_SETFL, O_NONBLOCK) != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker channel configuration failed");
   const auto null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
   if (null_fd < 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker stderr isolation failed");
   const auto close_null = boost::scope::scope_exit{[&] { ::close(null_fd); }};
   const auto isolated_null = isolated_descriptor(null_fd);
   const auto close_isolated_null = boost::scope::scope_exit{[&] { ::close(isolated_null); }};
   posix_spawn_file_actions_t actions{};
   spawn_check(::posix_spawn_file_actions_init(&actions));
   const auto destroy_actions = boost::scope::scope_exit{[&] { ::posix_spawn_file_actions_destroy(&actions); }};
   spawn_check(::posix_spawn_file_actions_adddup2(&actions, endpoints[1], STDIN_FILENO));
   spawn_check(::posix_spawn_file_actions_adddup2(&actions, endpoints[1], STDOUT_FILENO));
   spawn_check(::posix_spawn_file_actions_adddup2(&actions, isolated_null, STDERR_FILENO));
   spawn_check(::posix_spawn_file_actions_adddup2(&actions, fault_endpoints[1], 3));
   spawn_check(::posix_spawn_file_actions_addchdir_np(&actions, "/"));
   posix_spawnattr_t attributes{};
   spawn_check(::posix_spawnattr_init(&attributes));
   const auto destroy_attributes = boost::scope::scope_exit{[&] { ::posix_spawnattr_destroy(&attributes); }};
   sigset_t mask{};
   sigset_t defaults{};
   sigemptyset(&mask);
   sigfillset(&defaults);
   spawn_check(::posix_spawnattr_setsigmask(&attributes, &mask));
   spawn_check(::posix_spawnattr_setsigdefault(&attributes, &defaults));
   spawn_check(::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                                           POSIX_SPAWN_CLOEXEC_DEFAULT));
   auto helper = configuration.helper.string();
   auto heap = std::string{"FORGE_EXPRESSION_HEAP_BYTES="} + std::to_string(configuration.heap_bytes);
   std::array<char*, 2> argv{helper.data(), nullptr};
   std::array<char*, 5> environment{const_cast<char*>("LANG=C"), const_cast<char*>("LC_ALL=C"),
                                    const_cast<char*>("TZ=UTC"), heap.data(), nullptr};
   pid_t launched = -1;
   spawn_check(::posix_spawn(&launched, helper.c_str(), &actions, &attributes, argv.data(), environment.data()));
   // Publish ownership without allocations after successful spawn.
   pid = launched;
   channel = std::exchange(endpoints[0], -1);
   faults = std::exchange(fault_endpoints[0], -1);
}

bool runtime::impl::exhausted() noexcept {
   if (faults < 0)
      return saw_exhaustion;
   std::array<char, 4096> markers;
   // Drain at most the bounded native pipe capacity, never a producer-driven
   // polling loop. One positive marker is sufficient to classify native OOM.
   for (std::size_t attempt = 0; attempt < 64; ++attempt) {
      const auto count = ::read(faults, markers.data(), markers.size());
      if (count > 0) {
         saw_exhaustion = true;
         continue;
      }
      if (count < 0 && errno == EINTR)
         continue;
      break;
   }
   return saw_exhaustion;
}

void runtime::impl::transport_check(boost::system::error_code error) {
   if (!error)
      return;
   if (exhausted())
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression worker exhausted native memory");
   if (error == boost::asio::error::operation_aborted && !stopped.load())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression exchange cancelled");
   FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker terminated without a result");
}

boost::asio::awaitable<runtime::impl::outcome> runtime::impl::transfer(worker_protocol::request& request,
                                                                       std::size_t size) {
   outcome result;
   try {
      std::array<std::uint8_t, worker_protocol::prefix_bytes> prefix;
      worker_protocol::encode_length(static_cast<std::uint32_t>(size), prefix);
      const std::array buffers = {boost::asio::buffer(prefix), boost::asio::buffer(buffer.data(), size)};
      boost::system::error_code error;
      co_await boost::asio::async_write(socket, buffers,
                                        boost::asio::redirect_error(boost::asio::use_awaitable, error));
      transport_check(error);
      co_await boost::asio::async_read(socket, boost::asio::buffer(prefix),
                                       boost::asio::redirect_error(boost::asio::use_awaitable, error));
      transport_check(error);
      std::uint32_t response_size;
      try {
         response_size = worker_protocol::decode_length(prefix);
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "invalid expression worker response prefix");
      }
      if (response_size == 0 || response_size > worker_protocol::frame_limit)
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "invalid expression worker response length");
      co_await boost::asio::async_read(socket, boost::asio::buffer(buffer.data(), response_size),
                                       boost::asio::redirect_error(boost::asio::use_awaitable, error));
      transport_check(error);
      try {
         result.reply = worker_protocol::decode_response(std::span{buffer}.first(response_size));
      } catch (const std::bad_alloc&) {
         FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression response allocation failed");
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "malformed expression worker response");
      }
      const auto& reply = result.reply;
      if (reply.sequence != request.sequence || reply.heap_cap != configuration.heap_bytes ||
          reply.heap_live > reply.heap_peak || reply.heap_peak > reply.heap_cap ||
          (request.op == worker_protocol::operation::hello && reply.code != worker_protocol::status::ok))
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "incompatible expression worker response");
   } catch (...) {
      result.error = std::current_exception();
   }
   co_return result;
}

boost::asio::awaitable<runtime::impl::admission> runtime::impl::acquire() {
   admission result;
   try {
      result.ticket.emplace(co_await writer.acquire());
   } catch (const forge::asio::exceptions::canceled&) {
      result.error = std::make_exception_ptr(exceptions::cancelled{"expression admission cancelled"});
   } catch (const forge::asio::exceptions::rejected&) {
      result.error = std::make_exception_ptr(exceptions::worker_unavailable{"expression worker is stopped"});
   } catch (...) {
      result.error = std::current_exception();
   }
   co_return result;
}

boost::asio::awaitable<worker_protocol::response> runtime::impl::invoke(worker_protocol::request request,
                                                                        const forge::variant_object& values,
                                                                        std::uint64_t* accepted_program,
                                                                        operation_options operation) {
   if (operation.cancel.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression operation cancelled");
   const auto owner_deadline = std::chrono::steady_clock::now() + configuration.timeout;
   const auto deadline = operation.deadline ? std::min(owner_deadline, *operation.deadline) : owner_deadline;
   if (std::chrono::steady_clock::now() >= deadline)
      FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "expression admission deadline exceeded");
   auto timer = std::make_shared<boost::asio::steady_timer>(strand, deadline);
   auto delivery_error =
       std::make_shared<std::atomic<detail::cancellation_delivery_error>>(detail::cancellation_delivery_error::none);
   const auto check_delivery = [&] {
      switch (delivery_error->load(std::memory_order_acquire)) {
      case detail::cancellation_delivery_error::resource_exhausted:
         FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression cancellation delivery allocation failed");
      case detail::cancellation_delivery_error::unavailable:
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression cancellation delivery failed");
      default:
         break;
      }
   };
   worker_protocol::response response;
   try {
      // Cancellation touches only this operation's timer. A request waiting for
      // admission must never stop another caller's active worker exchange.
      const auto cancellation = std::stop_callback{
          operation.cancel, [weak = std::weak_ptr{timer}, executor = strand, delivery_error]() noexcept {
             detail::deliver_cancellation(
                 [&] {
                    boost::asio::post(executor, [weak, delivery_error]() noexcept {
                       detail::deliver_cancellation(
                           [&] {
                              if (const auto timer = weak.lock())
                                 timer->cancel();
                           },
                           *delivery_error);
                    });
                 },
                 *delivery_error);
          }};
      boost::system::error_code timer_error;
      admission admitted;
      std::exception_ptr admission_error;
      try {
         using namespace boost::asio::experimental::awaitable_operators;
         auto completed = co_await (
             acquire() || timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, timer_error)));
         if (completed.index() == 0)
            admitted = std::move(std::get<0>(completed));
         else if (operation.cancel.stop_requested())
            FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression admission cancelled");
         else
            FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "expression admission deadline exceeded");
      } catch (...) {
         admission_error = std::current_exception();
      }
      if (admission_error) {
         const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
         if (cancellation.cancelled() != boost::asio::cancellation_type::none)
            FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression admission cancelled");
         exchange_error(admission_error);
      }
      if (admitted.error)
         exchange_error(admitted.error);
      if (stopped.load())
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker is stopped");
      if (operation.cancel.stop_requested())
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression admission cancelled");
      if (std::chrono::steady_clock::now() >= deadline)
         FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "expression admission deadline exceeded");
      if (sequence == std::numeric_limits<std::uint64_t>::max()) {
         stop();
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression exchange identity exhausted");
      }
      request.sequence = ++sequence;
      saw_exhaustion = false;
      (void)exhausted();
      saw_exhaustion = false;
      // Encode borrowed sync inputs or the async caller's owned input before IO.
      // A local validation error has sent nothing and leaves the worker usable.
      const auto size = worker_protocol::encode_into(request, values, buffer);
      if (operation.cancel.stop_requested())
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression admission cancelled");
      if (std::chrono::steady_clock::now() >= deadline)
         FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "expression admission deadline exceeded");
      // Admission's losing timer has completed before its next use. The same
      // absolute deadline covers queued time, serialization and actual IPC.
      timer->expires_at(deadline);
      outcome result;
      bool timed_out = false;
      bool interrupted = false;
      try {
         using namespace boost::asio::experimental::awaitable_operators;
         auto completed =
             co_await (transfer(request, size) ||
                       timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, timer_error)));
         if (completed.index() == 0)
            result = std::move(std::get<0>(completed));
         else {
            const auto inherited = co_await boost::asio::this_coro::cancellation_state;
            interrupted =
                operation.cancel.stop_requested() || inherited.cancelled() != boost::asio::cancellation_type::none;
            timed_out = !interrupted;
         }
      } catch (...) {
         result.error = std::current_exception();
      }
      if (timed_out || interrupted || result.error ||
          delivery_error->load(std::memory_order_acquire) != detail::cancellation_delivery_error::none) {
         const auto memory_failure = exhausted();
         const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
         const auto cancelled =
             operation.cancel.stop_requested() || cancellation.cancelled() != boost::asio::cancellation_type::none;
         // This is an operation-local child coroutine. Shield only its mandatory
         // physical cleanup, preserving the public caller's cancellation policy.
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
         stop();
         co_await reap();
         check_delivery();
         if (memory_failure)
            FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression worker exhausted native memory");
         if (timed_out)
            FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "expression worker deadline exceeded");
         if (cancelled)
            FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression exchange cancelled");
         exchange_error(result.error);
      }
      if (accepted_program &&
          (request.op == worker_protocol::operation::prepare || request.op == worker_protocol::operation::load) &&
          result.reply.code == worker_protocol::status::ok && result.reply.prepared_id != 0) {
         // The preallocated program state is held by the joined outer prepare
         // coroutine. Publish ownership before any reply-delivery cancellation.
         *accepted_program = result.reply.prepared_id;
      }
      response = std::move(result.reply);
   } catch (...) {
      // Unregistering above joins request_stop's callback before inspecting its
      // refusal. The existing absolute deadline bounds a failed post fallback.
      check_delivery();
      throw;
   }
   check_delivery();
   co_return response;
}

boost::asio::awaitable<worker_protocol::response> runtime::impl::submit(std::shared_ptr<impl> owner,
                                                                        worker_protocol::request request,
                                                                        const forge::variant_object& values,
                                                                        std::uint64_t* accepted_program,
                                                                        operation_options operation) {
   if (!owner)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression runtime is unavailable");
   if (owner->stopped.load())
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression worker is stopped");
   if (owner->pending.fetch_add(1) >= 16) {
      owner->pending.fetch_sub(1);
      owner->changed.notify();
      FORGE_THROW_EXCEPTION(exceptions::busy, "expression request admission is full");
   }
   const auto admitted = boost::scope::scope_exit{[&] {
      owner->pending.fetch_sub(1);
      owner->changed.notify();
   }};
   // Only the caller coroutine retains the context. The joined strand child
   // borrows it, so completion cannot destroy an owned IO runtime on itself.
   try {
      co_return co_await boost::asio::co_spawn(owner->strand,
                                               owner->invoke(std::move(request), values, accepted_program, operation),
                                               boost::asio::use_awaitable);
   } catch (...) {
      exchange_error(std::current_exception());
   }
}

void runtime::impl::stop() noexcept {
   stopped.store(true);
   writer.close();
   boost::system::error_code ignored;
   socket.cancel(ignored);
   socket.close(ignored);
   if (channel >= 0)
      ::close(std::exchange(channel, -1));
   if (pid > 0)
      (void)::kill(pid, SIGKILL);
}

boost::asio::awaitable<void> runtime::impl::reap() {
   auto exclusive = co_await reaper.acquire();
   if (pid <= 0)
      co_return;
   if (!process)
      process = std::make_unique<boost::process::v2::process_handle>(strand, pid);
   int status = W_STOPCODE(SIGSTOP);
   boost::system::error_code error;
   do {
      co_await process->async_wait(status, boost::asio::redirect_error(boost::asio::use_awaitable, error));
   } while (error.value() == EINTR);
   if (error.value() == ECHILD) {
      // External reap forfeits PID authority. Never signal a reused PID.
      pid = -1;
      process.reset();
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression child ownership was lost");
   }
   if (error || (!WIFEXITED(status) && !WIFSIGNALED(status)))
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression child reap failed");
   pid = -1; // Clear authority on the same strand as native wait completion.
   process.reset();
}

boost::asio::awaitable<void> runtime::impl::shutdown() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   auto exclusive = co_await closer.acquire();
   if (closed.load())
      co_return;
   stop();
   for (;;) {
      const auto observed = changed.epoch();
      bool pump;
      {
         const auto lock = std::scoped_lock{release_mutex};
         pump = release_scheduled;
      }
      if (pending.load() == 0 && !pump)
         break;
      co_await changed.async_wait(observed);
   }
   co_await reap();
   if (faults >= 0)
      ::close(std::exchange(faults, -1));
   signal_owner.release();
   closed.store(true);
}

void runtime::impl::shutdown_sync() noexcept {
   if (closed.load())
      return;
   if (!signal_owner.registered()) {
      closed.store(true);
      return;
   }
   // On an injected IO thread the owner must first await shutdown_async().
   // Losing a live child or stopping its only executor is never a fallback.
   try {
      require_host();
      forge::asio::blocking::run(executor, boost::asio::co_spawn(strand, shutdown(), boost::asio::use_awaitable));
   } catch (...) {
      std::terminate();
   }
}

void runtime::impl::release(std::uint64_t id) noexcept {
   if (id == 0 || stopped.load())
      return;
   const auto lock = std::scoped_lock{release_mutex};
   if (stopped.load())
      return;
   // These are exactly the worker's 64 prepared slots, not a generic job queue.
   if (release_count == releases.size()) {
      release_failed = true;
      stopped.store(true);
   } else {
      releases[release_count++] = id;
   }
   if (release_scheduled)
      return;
   release_scheduled = true;
   try {
      boost::asio::post(strand, [this] {
         try {
            boost::asio::co_spawn(strand, release_pending(),
                                  [this](std::exception_ptr error) { release_complete(std::move(error)); });
         } catch (...) {
            release_complete(std::current_exception());
         }
      });
   } catch (...) {
      release_scheduled = false;
      release_failed = true;
      stopped.store(true);
      changed.notify();
   }
}

boost::asio::awaitable<void> runtime::impl::release_pending() {
   for (;;) {
      std::uint64_t id;
      {
         const auto lock = std::scoped_lock{release_mutex};
         if (release_count == 0 || stopped.load())
            co_return;
         id = releases[--release_count];
      }
      worker_protocol::request request;
      request.op = worker_protocol::operation::release;
      request.prepared_id = id;
      const auto reply = co_await invoke(std::move(request), {});
      if (reply.code != worker_protocol::status::ok)
         FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression program release failed");
   }
}

void runtime::impl::release_complete(std::exception_ptr error) noexcept {
   bool again = false;
   bool failed;
   {
      const auto lock = std::scoped_lock{release_mutex};
      release_failed = release_failed || error != nullptr;
      failed = release_failed;
      if (failed || stopped.load())
         release_count = 0;
      if (release_count == 0)
         release_scheduled = false;
      else
         again = true;
   }
   if (failed)
      stop();
   if (again) {
      try {
         boost::asio::co_spawn(strand, release_pending(),
                               [this](std::exception_ptr next) { release_complete(std::move(next)); });
      } catch (...) {
         release_complete(std::current_exception());
      }
   }
   changed.notify();
}

} // namespace forge::expression
