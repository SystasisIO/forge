module;

#include <coroutine>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <limits>
#include <exception>
#include <forge/exceptions/macros.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <sys/wait.h>
#include <type_traits>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.compiler_session;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"
#include "details/compiler_session_impl_child.hxx"

namespace forge::vm::wasm::optimizing {

static_assert(std::is_nothrow_move_constructible_v<boost::asio::awaitable<void>>);
static_assert(std::is_nothrow_move_constructible_v<target_profile>);

void compiler_session::impl::acquire_signal_owner() {
#if defined(__APPLE__)
   try {
      signal_owner.acquire(runtime.context().get_executor());
   } catch (const asio::exceptions::rejected&) {
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "incompatible existing SIGCHLD ownership");
   } catch (const asio::exceptions::invalid_state&) {
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "SIGCHLD registration could not be verified");
   } catch (const asio::exceptions::internal&) {
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "SIGCHLD registration failed");
   }
#else
   struct sigaction current{};
   if (::sigaction(SIGCHLD, nullptr, &current) != 0 || (current.sa_flags & SA_NOCLDWAIT) != 0 ||
       current.sa_handler != SIG_DFL)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "incompatible existing SIGCHLD ownership");
#endif
}

void compiler_session::impl::release_signal_owner() {
#if defined(__APPLE__)
   try {
      signal_owner.release();
   } catch (const asio::exceptions::invalid_state&) {
      FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "SIGCHLD registration no longer has confirmed ownership");
   } catch (const asio::exceptions::internal&) {
      FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "SIGCHLD registration could not be released");
   }
#endif
}

void compiler_session::impl::initialization::cancel() noexcept {
   stop.request_stop();
   {
      const auto lock = std::scoped_lock{mutex};
      if (!completed) {
         error = cancelled_error;
         completed = true;
      }
   }
   changed.notify(); // never join registration or release a live job from a callback
}

boost::asio::awaitable<void> compiler_session::impl::wait_initialize(std::shared_ptr<initialization> setup) {
   // Only the independent outcome/notification is pinned. No job, child, task
   // handle or executor from the injected runtime survives in this lazy waiter.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   for (;;) {
      const auto observed = setup->changed.epoch();
      {
         const auto lock = std::scoped_lock{setup->mutex};
         if (setup->completed) {
            if (setup->error)
               std::rethrow_exception(setup->error);
            co_return;
         }
      }
      co_await setup->changed.async_wait(observed);
   }
}

boost::asio::awaitable<void> compiler_session::impl::run_initialize(std::shared_ptr<job> record, std::stop_token stop) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   auto error = std::exception_ptr{};
   try {
      {
         const auto lock = std::scoped_lock{index_mutex};
         if (stopped || stop.stop_requested() || record->exchange.session != generation)
            std::rethrow_exception(cancelled_error);
      }
      // Reuse the shared canonical profile owner; wrong OS/CPU/features/version
      // fails BEFORE opening or changing any cache path.
      auto profile = co_await compute.execute(
          asio::compute::task_options{.name = "wasm-preflight-profile", .parent_stop_token = stop},
          [record](asio::compute::context& context) {
             context.throw_if_stop_requested();
             auto configuration = compile_configuration{};
             configuration.target = record->setup->target;
             auto target = canonicalize(std::move(configuration)).target;
             if (!target.matches_host())
                FORGE_THROW_EXCEPTION(exceptions::unsupported_target, "compiler profile differs from native host");
             return target;
          });
      record->setup->target = std::move(profile);
      // Cold default is checked once for the first owner. Further Darwin
      // sessions may compose only with the unchanged SAME-owned registration.
      acquire_signal_owner();
      auto body = co_await compute.execute(
          asio::compute::task_options{.name = "wasm-preflight-encode", .parent_stop_token = stop},
          [record, ceiling = options.max_configuration_bytes](asio::compute::context& context) {
             context.throw_if_stop_requested();
             auto encoded = compiler_protocol::encode(record->setup->target);
             if (encoded.size() > ceiling)
                FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler preflight profile exceeds budget");
             return encoded;
          });
      static_cast<void>(co_await exchange(record, std::move(body), stop));
      // Above is only exact HELLO/ACK/EOF + physical exit/reap: no WASM/LLVM.
      co_await compute.execute(
          asio::compute::task_options{.name = "wasm-cache-initialize", .parent_stop_token = stop},
          [this, record](asio::compute::context& context) { initialize_cache(record->setup->target, context); });
      if (stop.stop_requested() || record->setup->stop.stop_requested())
         std::rethrow_exception(cancelled_error);
   } catch (...) {
      error = std::current_exception();
   }
   finish_initialize(record, std::move(error));
}

void compiler_session::impl::finish_initialize(const std::shared_ptr<job>& record, std::exception_ptr error) {
   // Joining a user stop callback is outside the index mutex. Its weak capture
   // only cancels this published control handle; no ownership cycle exists.
   auto registration = std::unique_ptr<std::stop_callback<std::function<void()>>>{};
   auto already_finalized = false;
   auto retire = false;
   {
      const auto lock = std::scoped_lock{index_mutex};
      if (record->setup->completing)
         return;
      already_finalized = record->setup->finalized;
      if (already_finalized) {
         // Publishing an outcome/joining registration is once-only. A child
         // retained by an earlier failed drain can become quiescent on retry;
         // that later owner must still retire the SAME tracked live record.
         retire = !record->process || record->process->is_quiescent();
      } else {
         record->setup->completing = true;
         registration = std::move(record->setup->registration);
      }
   }
   if (already_finalized) {
      if (retire)
         release_job(record);
      return;
   }
   registration.reset(); // sole finisher; joining occurs outside index exclusion
   auto retained = false;
   {
      const auto lock = std::scoped_lock{index_mutex};
      if (stopped || record->exchange.session != generation || record->setup->stop.stop_requested())
         error = cancelled_error;
      retained = record->process && !record->process->is_quiescent();
      if (retained) {
         stopped = true; // shutdown_incomplete keeps the exact child/FD/live record
         if (!error)
            error = cancelled_error;
      }
      if (!error) {
         initialized_target.emplace(std::move(record->setup->target));
         phase = initialization_phase::ready;
      } else {
         phase = initialization_phase::failed;
      }
      record->setup->finalized = true;
      record->setup->completing = false;
      {
         const auto outcome_lock = std::scoped_lock{record->setup->mutex};
         if (!record->setup->completed) {
            record->setup->error = std::move(error);
            record->setup->completed = true;
         } // a pending/running cancellation already published its terminal outcome
      }
   }
   record->setup->changed.notify();
   if (!retained)
      release_job(record);
   // Do NOT announce session quiescence or close ownership here. The scheduler
   // still retains captures until its final completion; explicit shutdown drains
   // those plus compute/child callbacks before releasing the cache lock.
}

} // namespace forge::vm::wasm::optimizing
