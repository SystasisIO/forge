module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

export module forge.vm.wasm.optimizing.compiler_session;

export import forge.asio.compute;
export import forge.asio.notification;
export import forge.asio.runtime;
export import forge.asio.task;
export import forge.vm.wasm.optimizing.compiler_protocol;

import forge.asio.child_signal_registration;

export namespace forge::vm::wasm::optimizing {

struct session_options {
   std::filesystem::path helper;
   std::filesystem::path cache;
   std::size_t worker_count = 0;
   std::size_t max_pending_jobs = 0;
   std::size_t max_tickets = 0;
   std::size_t max_imports = 0;
   std::uint64_t max_configuration_bytes = 0;
   std::uint64_t max_request_bytes = 0;
   std::uint64_t max_response_bytes = 0;
   std::uint64_t max_inflight_bytes = 0;
   std::uint64_t max_resident_bytes = 0;
   // Encoded cache payload lengths/entry slots, including live temporaries,
   // quarantined files and opaque crash orphans. Not filesystem/RSS overhead.
   std::uint64_t max_disk_bytes = 0;
   std::size_t max_disk_entries = 0;
   std::chrono::milliseconds helper_timeout{};
   compiler_protocol::resource_limits helper_limits;
   // Zero inherits a per-active-worker share of the existing finite resident
   // and inflight budgets, after fixed metadata and precharged job/tickets.
   // Default resident/workspace ceilings also obey current unreserved shared
   // headroom. Resident first tries existing ready eviction; external/weak pins
   // remain charged. Decoded storage plus a positive native ceiling must fit.
   // Queued jobs retain their charged storage during another preparation.
   // max_tickets changes actual metadata costs, never the share's divisor.
   // Artifact/configuration/token bytes are subtracted from the resident share.
   // Nonzero is an explicit hard ceiling, never shrunk to available headroom,
   // and not a promise that every valid artifact fits.
   std::uint64_t max_preparation_resident_bytes = 0;
   std::uint64_t max_preparation_workspace_bytes = 0;
};

enum class admission_status { accepted_unverified, saturated, stopped, not_initialized };

// Runtime, compute pool/executor and their storage must remain alive through
// successful shutdown(). compute::pool::request_stop() may precede shutdown:
// cleanup submits no new compute work, but joins already submitted work and
// requires the Asio runtime to keep servicing child reap/timers/task drains.
// Destroy/join the compute/runtime owners only after successful session drain.
// Cold SIGCHLD default/no-SA_NOCLDWAIT/no-competing-reaper is required.
// Darwin sessions on distinct runtimes may share SAME-owned native Asio
// registrations. Foreign disposition changes are rejected; callers must not
// replace the handler/reap children while any session owns a registration.
// A failed checked unregister retains its registration and injected runtime.
// shutdown's deadline escalates owned children to SIGKILL; it is not a bound
// on OS reap latency. shutdown_incomplete retains ownership for cleanup retry.
// Destroying a non-quiescent session is a lifecycle violation, not an implicit
// blocking wait or detached cleanup. Limits account owned bytes, not process RSS.
class compiler_session {
 public:
   enum class completion_status { already_ready, loaded_from_disk, compiled };
   struct completion {
      artifact code;
      std::chrono::steady_clock::time_point queued_time;
      std::chrono::steady_clock::time_point completed_time;
      completion_status status;
   };

   class ticket;
   struct enqueue_result;
   struct memory_usage {
      std::uint64_t resident_bytes;
      std::uint64_t inflight_bytes;
   };

   compiler_session(asio::runtime&, asio::compute::executor, session_options);
   ~compiler_session();
   compiler_session(const compiler_session&) = delete;
   compiler_session& operator=(const compiler_session&) = delete;
   compiler_session(compiler_session&&) = delete;
   compiler_session& operator=(compiler_session&&) = delete;

   // Eagerly claims/submits one tracked cold operation before returning its
   // lazy waiter. SAME helper HELLO-only/limits/sole reap and bounded exclusive
   // cache preflight precede admission; no WASM/parser/LLVM compile is needed.
   // Repeated/concurrent calls are invalid_state. Failure keeps admission
   // closed. Even a discarded waiter requires explicit shutdown() while the
   // injected owners remain alive (compute admission may already be stopped);
   // never destroy a non-quiescent owner.
   // After successful shutdown, a retained waiter reads only its independent
   // terminal outcome/notification. It pins no job, child or old executor and
   // may be consumed on a new runtime or discarded after ALL old injected
   // owners/storage are destroyed. Cancellation delivery alone is not shutdown.
   boost::asio::awaitable<void> initialize(target_profile expected, std::stop_token stop);

   // Prepared/in-memory only. No canonicalization, hash, filesystem, planner,
   // mapping or worker wait. Full expected configuration equality is required.
   // Cold/initializing/failed/stopped sessions return null.
   std::optional<artifact> ready(const crypto::digest::sha256&, const compile_configuration& canonical_expected) const;
   // Shared account snapshot, including external pins after shutdown.
   memory_usage usage() const;
   // Bounded capacity/count admission only. Hint is not authority or dedup.
   // Full WASM identify/canonicalization occur on compute before cache mutation.
   enqueue_result enqueue(const crypto::digest::sha256& hint, compile_request&&, asio::task::priority);
   void request_stop();
   boost::asio::awaitable<void> shutdown(std::chrono::steady_clock::time_point escalation);

 private:
   struct impl;
   std::unique_ptr<impl> _impl;
};

class compiler_session::ticket {
 public:
   ticket() noexcept;
   ~ticket();
   ticket(ticket&&) noexcept;
   ticket& operator=(ticket&&) noexcept;
   ticket(const ticket&) = delete;
   ticket& operator=(const ticket&) = delete;
   bool valid() const noexcept;
   // Cancels this consumer only, never shared compilation or a task handle.
   bool cancel() noexcept;
   // Transfers ownership immediately, before the lazy coroutine is returned.
   boost::asio::awaitable<completion> wait(std::chrono::steady_clock::time_point deadline, std::stop_token stop) &&;

 private:
   struct state;
   explicit ticket(std::shared_ptr<state>);
   static boost::asio::awaitable<completion> wait_owned(std::shared_ptr<state>, std::chrono::steady_clock::time_point,
                                                        std::stop_token);
   std::shared_ptr<state> _state;
   friend class compiler_session;
};

struct compiler_session::enqueue_result {
   admission_status status;
   std::optional<ticket> completion;
};

} // namespace forge::vm::wasm::optimizing
