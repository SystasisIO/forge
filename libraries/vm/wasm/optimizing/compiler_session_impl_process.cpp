module;

#include <coroutine>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/scope/scope_exit.hpp>
#include <atomic>
#include <csignal>
#include <exception>
#include <forge/exceptions/macros.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <sys/wait.h>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.compiler_session;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"
#include "details/compiler_session_impl_child.hxx"

namespace forge::vm::wasm::optimizing {

boost::asio::awaitable<std::vector<std::uint8_t>>
compiler_session::impl::exchange(std::shared_ptr<job> record, std::vector<std::uint8_t> request, std::stop_token stop) {
   if (stop.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler helper launch was cancelled");
   auto process = std::make_shared<child>(runtime.context().get_executor(), options, record->exchange,
                                          record->setup ? crypto::digest::sha256{} : *record->verified,
                                          record->setup ? record->setup->target : record->request->configuration.target,
                                          record->setup != nullptr);
   {
      const auto lock = std::scoped_lock{index_mutex};
      if (stopped || record->exchange.session != generation || record->exchange.attempt != 1)
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "stale compiler helper launch");
      record->process = process; // ownership record is published before any spawn
   }
   auto launch_error = std::exception_ptr{};
   try {
      // Retain before any workspace/PID/fd mutation. The same admission stays
      // idle through async I/O/reap and can drain cleanup after compute stop.
      process->admission = co_await compute.retain(
          asio::compute::task_options{.name = "wasm-helper-lifecycle", .parent_stop_token = stop});
      // The admission links parent stop, including post-work cancellation.
      // Spawn publishes successful PID+fd ownership on child before returning;
      // even a canceled phase result must still activate/drain that real child.
      co_await process->admission.execute([process, stop] { process->spawn(stop); });
   } catch (...) {
      launch_error = std::current_exception();
   }
   auto output = std::vector<std::uint8_t>{};
   if (!launch_error || process->launched_pid.load(std::memory_order_acquire) > 0) {
      try {
         output = co_await boost::asio::co_spawn(process->strand, child::run_owned(process, std::move(request), stop),
                                                 boost::asio::use_awaitable);
      } catch (...) {
         if (!launch_error)
            launch_error = std::current_exception();
      }
   }
   auto cleanup_error = std::exception_ptr{};
   if (process->process_drained()) {
      try {
         co_await child::cleanup_workspace(process);
      } catch (...) {
         cleanup_error = std::current_exception(); // Physical owner/admission remain on the live job.
      }
   }
   if (launch_error)
      std::rethrow_exception(launch_error); // First work error wins; failed cleanup remains owned for shutdown retry.
   if (cleanup_error)
      std::rethrow_exception(cleanup_error);
   // Successful cleanup releases retained capacity before prepare/cache asks
   // for ordinary compute admission, including a single-capacity pool.
   co_return output;
}

boost::asio::awaitable<void> compiler_session::impl::shutdown(std::chrono::steady_clock::time_point escalation) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   {
      const auto lock = std::scoped_lock{index_mutex};
      if (shutdown_active)
         FORGE_THROW_EXCEPTION(asio::exceptions::invalid_state, "compiler session shutdown is already active");
      shutdown_active = true;
   }
   const auto clear_active = boost::scope::scope_exit{[this] {
      const auto lock = std::scoped_lock{index_mutex};
      shutdown_active = false;
   }};
   request_stop();
   auto records = std::vector<std::shared_ptr<job>>{};
   {
      const auto lock = std::scoped_lock{index_mutex};
      records.reserve(live_slots);
      for (std::size_t i = 0; i < live_slots; ++i)
         if (live[i])
            records.push_back(live[i]);
   }
   // Escalation and sole async reap happen with the injected runtime/compute
   // still live. Child strand serializes physical reap, authority clearing and
   // timer callbacks. OS/ownership errors preserve records and stop admission.
   for (const auto& record : records) {
      auto process = std::shared_ptr<child>{};
      {
         const auto lock = std::scoped_lock{index_mutex};
         process = record->process;
      }
      if (process)
         co_await boost::asio::co_spawn(process->strand, child::cleanup_owned(process, escalation),
                                        boost::asio::use_awaitable);
   }
   co_await scheduler.shutdown();
   // Pending jobs canceled before their work began never entered run()/finish().
   // Their ticket terminal errors and final retained accounting are drained now.
   for (const auto& record : records) {
      if (record->process && !record->process->process_drained())
         FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete,
                               "compiler child remained non-quiescent after task drain");
      if (record->process)
         co_await child::cleanup_workspace(record->process);
      finish(record, {}, cancelled_error);
   }
   {
      const auto lock = std::scoped_lock{index_mutex};
      for (std::size_t i = 0; i < live_slots; ++i)
         if (live[i])
            FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler records remained after shutdown drain");
   }
   release_signal_owner(); // Failure retains SAME registration/cache/runtime and forbids quiescence.
   close_cache();          // no live child/task/compute callback can still access the cache
   clear_ready();          // external artifact/module/weak pins carry their own charge
   static_cast<void>(interpret::prepared_module::preparation::retry_pending_cleanup(options.max_tickets));
   {
      const auto lock = std::scoped_lock{index_mutex};
      quiescent = true;
   }
   // Completed external tickets/artifact pins remain valid. No pending callback
   // captures a raw session; external storage charges have a durable shared owner.
}

} // namespace forge::vm::wasm::optimizing
