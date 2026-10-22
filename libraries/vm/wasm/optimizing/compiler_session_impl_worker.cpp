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
#include <algorithm>
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
#include "details/compiler_session_probe.hxx"

module forge.vm.wasm.optimizing.compiler_session;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"
#include "details/compiler_session_impl_child.hxx"

namespace forge::vm::wasm::optimizing {

boost::asio::awaitable<void> compiler_session::impl::run(std::shared_ptr<job> record, std::stop_token stop) {
   // Task cancellation uses its existing stop_token. Do not let an unrelated
   // associated coroutine cancellation abandon a running compute operation or
   // the sole owned child reaper before its callback captures have drained.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   try {
      const auto identity = co_await compute.execute(
          asio::compute::task_options{.name = "wasm-verify-request", .parent_stop_token = stop},
          [record](asio::compute::context& context) {
             context.throw_if_stop_requested();
             record->request->configuration = canonicalize(std::move(record->request->configuration));
             const auto result = identify(*record->request);
             if (result != record->hint)
                FORGE_THROW_EXCEPTION(exceptions::invalid_module, "compiler admission identity hint mismatch");
             context.throw_if_stop_requested();
             return result;
          });
      // No disk/helper/verified-index operation has happened before this check.
      record->verified = identity;
      auto ready_result = std::optional<completion>{};
      auto aliased = false;
      auto claimed = false;
      for (std::size_t attempt = 0; attempt <= options.max_tickets && !claimed; ++attempt) {
         auto need_eviction = false;
         {
            const auto lock = std::scoped_lock{index_mutex};
            if (stopped || record->exchange.session != generation || stop.stop_requested())
               FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler session was stopped");
            if (!initialized_target || record->request->configuration.target != *initialized_target)
               FORGE_THROW_EXCEPTION(exceptions::unsupported_target,
                                     "request differs from initialized compiler profile");
            auto free_slot = options.max_tickets;
            for (std::size_t i = 0; i < options.max_tickets; ++i) {
               if (!index[i]) {
                  if (free_slot == options.max_tickets)
                     free_slot = i;
                  continue;
               }
               auto& candidate = *index[i];
               if (candidate.identity != identity)
                  continue;
               if (candidate.configuration != record->request->configuration)
                  FORGE_THROW_EXCEPTION(exceptions::invalid_module,
                                        "canonical compiler identity configuration mismatch");
               if (candidate.result) {
                  ready_result = *candidate.result;
                  ready_result->status = completion_status::already_ready;
               } else {
                  // Attach only after verification. The alias's scheduled
                  // verification task ends now; it never waits for the leader and
                  // never occupies another compiler slot or resubmits a task.
                  for (std::size_t j = 0; j < options.max_tickets; ++j) {
                     if (candidate.pending->consumers[j].expired()) {
                        candidate.pending->consumers[j] = record->consumers[0];
                        aliased = true;
                        break;
                     }
                  }
                  if (!aliased)
                     FORGE_THROW_EXCEPTION(exceptions::queue_full, "canonical compiler consumers exceed bounded slots");
               }
               break;
            }
            if (!ready_result && !aliased) {
               if (free_slot == options.max_tickets) {
                  need_eviction = true;
               } else {
                  const auto configuration_charge = configuration_bytes(record->request->configuration, options);
                  if (!configuration_charge)
                     FORGE_THROW_EXCEPTION(exceptions::compile_limit, "canonical compiler metadata exceeds budget");
                  const auto charge_lock = std::scoped_lock{account->mutex};
                  if (*configuration_charge > options.max_resident_bytes - account->resident_bytes) {
                     need_eviction = true;
                  } else {
                     account->resident_bytes += *configuration_charge;
                     record->resident_bytes = *configuration_charge;
                     try {
                        index[free_slot].emplace(entry{identity, record->request->configuration, record, {}, 0});
                        // A copied string/vector need not retain the source's
                        // capacity. The job's precharged copy workspace covers
                        // construction; index exclusion prevents publication
                        // until actual owned capacities are measured here.
                        const auto actual = configuration_bytes(index[free_slot]->configuration, options);
                        if (!actual)
                           FORGE_THROW_EXCEPTION(exceptions::compile_limit, "copied compiler metadata exceeds budget");
                        const auto without_copy = account->resident_bytes - record->resident_bytes;
                        if (*actual > options.max_resident_bytes - without_copy) {
                           index[free_slot].reset(); // Drop copy before its resident reservation.
                           account->resident_bytes = without_copy;
                           record->resident_bytes = 0;
                           need_eviction = true;
                        } else {
                           account->resident_bytes = without_copy + *actual;
                           record->resident_bytes = *actual;
                           claimed = true;
                        }
                     } catch (...) {
                        index[free_slot].reset();
                        account->resident_bytes -= std::exchange(record->resident_bytes, 0);
                        throw;
                     }
                  }
               }
            }
         }
         if (ready_result || aliased)
            break;
         if (need_eviction && !evict_one())
            FORGE_THROW_EXCEPTION(exceptions::queue_full, "canonical compiler index is pinned or full");
      }
      if (ready_result) {
         finish(record, std::move(ready_result), {});
         co_return;
      }
      if (aliased) {
         release_job(record);
         co_return;
      }
      if (!claimed)
         FORGE_THROW_EXCEPTION(exceptions::queue_full, "canonical compiler index admission raced its bounded slots");
      auto code = co_await load(record, stop);
      auto status = completion_status::loaded_from_disk;
      if (!code) {
         auto encoded = co_await compute.execute(
             asio::compute::task_options{.name = "wasm-encode-request", .parent_stop_token = stop},
             [record, ceiling = options.max_request_bytes](asio::compute::context& context) {
                context.throw_if_stop_requested();
                auto bytes = record->request->encode();
                if (bytes.size() > ceiling)
                   FORGE_THROW_EXCEPTION(exceptions::compile_limit, "encoded compiler request exceeds budget");
                context.throw_if_stop_requested();
                return bytes;
             });
         auto output = co_await exchange(record, std::move(encoded), stop);
         code = co_await prepare(record, std::move(output), stop, artifact::origin::isolated_compiler);
         co_await persist(record, stop);
         status = completion_status::compiled;
      }
      if (stop.stop_requested())
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler job was stopped before publication");
      finish(record, completion{std::move(*code), record->queued_time, std::chrono::steady_clock::now(), status}, {});
   } catch (const std::bad_alloc&) {
      finish(record, {}, allocation_error); // Existing ticket outcome/wake path does not allocate.
   } catch (...) {
      finish(record, {}, std::current_exception());
   }
}

void compiler_session::impl::finish(const std::shared_ptr<job>& record, std::optional<completion> result,
                                    std::exception_ptr error) {
   if (record->setup) {
      finish_initialize(record, std::move(error));
      return;
   }
   auto cleanup_complete = true;
   if (record->cleanup) {
      cleanup_complete = record->cleanup->retry_cleanup();
      if (cleanup_complete)
         record->cleanup.reset();
      else {
         result.reset();
         error = cleanup_error;
      }
   }
   auto removed = std::optional<entry>{};
   {
      const auto lock = std::scoped_lock{index_mutex};
      const auto valid = record->exchange.session == generation && record->exchange.attempt == 1;
      if (!valid || (stopped && result)) {
         result.reset();
         error = cancelled_error; // cold preallocated terminal error; no allocation here
      }
      for (std::size_t i = 0; i < options.max_tickets; ++i) {
         if (!index[i] || index[i]->pending != record)
            continue;
         if (result && !error) {
            index[i]->result = *result;
            index[i]->charged_bytes = std::exchange(record->resident_bytes, 0);
            index[i]->pending.reset();
         } else {
            removed.emplace(std::move(*index[i]));
            index[i].reset();
         }
         break;
      }
   }
   // An unsuccessful admission never erases somebody else's canonical entry.
   // Release dropped configuration/mapping storage before its accounting charge.
   removed.reset();
   record->prepared.reset();
   // Global maintenance is bounded/advisory: remaining roots may belong to
   // unrelated sessions or external pins. This job's instance handle decides
   // whether its own cleanup is complete.
   static_cast<void>(interpret::prepared_module::preparation::retry_pending_cleanup(options.max_tickets));
   release_resident(std::exchange(record->resident_bytes, 0));
   for (std::size_t i = 0; i < options.max_tickets; ++i) {
      if (const auto consumer = record->consumers[i].lock()) {
         if (error)
            consumer->fail(error);
         else if (result)
            consumer->complete(*result);
      }
   }
   if (!cleanup_complete || (record->process && !record->process->is_quiescent())) {
      // Keep the complete owned record/FD/reap state for explicit cleanup retry.
      // It cannot be evicted or destroyed merely because the work coroutine
      // delivered a typed failure. No future admission is allowed while stopping.
      const auto lock = std::scoped_lock{index_mutex};
      stopped = true;
   } else {
      release_job(record);
   }
}

bool compiler_session::impl::reserve_resident(std::uint64_t& bytes, std::uint64_t owned_bytes, bool inherited) {
   if (bytes <= owned_bytes)
      return false;
   for (std::size_t attempt = 0; attempt <= options.max_tickets; ++attempt) {
      {
         const auto charge_lock = std::scoped_lock{account->mutex};
         if (bytes <= options.max_resident_bytes - account->resident_bytes) {
            account->resident_bytes += bytes;
            return true;
         }
      }
      if (attempt == options.max_tickets || !evict_one())
         break;
   }
   // Eviction cannot refund external/weak pins. Only the inherited policy cap
   // may shrink; the native loader still enforces the resulting positive cap.
   const auto charge_lock = std::scoped_lock{account->mutex};
   const auto available = options.max_resident_bytes - account->resident_bytes;
   if (!inherited || available <= owned_bytes)
      return false;
   bytes = std::min(bytes, available);
   account->resident_bytes += bytes;
   return true;
}

bool compiler_session::impl::evict_one() {
   auto evicted = std::optional<entry>{};
   {
      const auto lock = std::scoped_lock{index_mutex};
      for (std::size_t i = 0; i < options.max_tickets; ++i) {
         if (index[i] && index[i]->result && index[i]->result->code.is_unique()) {
            evicted.emplace(std::move(*index[i]));
            index[i].reset();
            break;
         }
      }
   }
   if (!evicted)
      return false;
   const auto released = evicted->charged_bytes;
   evicted.reset();
   release_resident(released);
   return true;
}

void compiler_session::impl::release_resident(std::uint64_t bytes) noexcept {
   const auto lock = std::scoped_lock{account->mutex};
   account->resident_bytes -= bytes;
}

void compiler_session::impl::clear_ready() {
   for (std::size_t i = 0; i < options.max_tickets; ++i) {
      auto removed = std::optional<entry>{};
      {
         const auto lock = std::scoped_lock{index_mutex};
         if (index[i] && index[i]->result) {
            removed.emplace(std::move(*index[i]));
            index[i].reset();
         }
      }
      if (removed) {
         const auto bytes = removed->charged_bytes;
         removed.reset();
         release_resident(bytes); // Only index configuration, never externally pinned storage.
      }
   }
}

boost::asio::awaitable<artifact> compiler_session::impl::prepare(std::shared_ptr<job> record,
                                                                 std::vector<std::uint8_t> bytes, std::stop_token stop,
                                                                 artifact::origin authority) {
   co_await compute.execute(
       asio::compute::task_options{.name = "wasm-decode-prepare", .parent_stop_token = stop},
       [this, record, bytes = std::move(bytes), authority](asio::compute::context& context) {
          context.throw_if_stop_requested();
          if (record->cleanup) {
             if (!record->cleanup->retry_cleanup())
                std::rethrow_exception(cleanup_error);
             record->cleanup.reset();
          }
          auto code = std::optional<artifact>{
              artifact::decode(bytes, *record->verified, options.max_response_bytes, authority)};
          if (code->image().configuration != record->request->configuration)
             FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "compiler artifact configuration mismatch");
          auto token_bytes = std::uint64_t{};
          auto charge = std::shared_ptr<resident_charge>{};
          for (std::size_t attempt = 0; !charge; ++attempt) {
             try {
                charge = std::allocate_shared<resident_charge>(
                    resident_allocator<resident_charge>{account, options.max_resident_bytes, &token_bytes}, account);
             } catch (const std::bad_alloc&) {
                if (attempt == options.max_tickets || !evict_one())
                   throw;
             }
          }
          auto limits = artifact::preparation_limits{};
          const auto resident_share = (options.max_resident_bytes - base_resident_bytes) / options.worker_count;
          const auto owned_bytes = code->byte_size();
          auto resident_overhead = owned_bytes;
          if (record->resident_bytes > UINT64_MAX - resident_overhead ||
              token_bytes > UINT64_MAX - resident_overhead - record->resident_bytes)
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler resident share size overflow");
          resident_overhead += record->resident_bytes + token_bytes;
          if (!options.max_preparation_resident_bytes && resident_overhead >= resident_share)
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "decoded artifact exceeds its resident worker share");
          limits.resident_bytes = options.max_preparation_resident_bytes ? options.max_preparation_resident_bytes
                                                                         : resident_share - resident_overhead;
          const auto inflight_share = (options.max_inflight_bytes - base_inflight_bytes) / options.worker_count;
          constexpr auto ticket_bytes = sizeof(ticket::state) + sizeof(ticket::state::outcome) + sizeof(completion);
          if (options.max_tickets > UINT64_MAX / ticket_bytes ||
              record->charged_bytes > UINT64_MAX - options.max_tickets * ticket_bytes)
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler inflight share size overflow");
          const auto inflight_overhead = record->charged_bytes + options.max_tickets * ticket_bytes;
          if (!options.max_preparation_workspace_bytes && inflight_overhead >= inflight_share)
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler job exceeds its inflight worker share");
          limits.workspace_bytes = options.max_preparation_workspace_bytes ? options.max_preparation_workspace_bytes
                                                                           : inflight_share - inflight_overhead;
          auto planned = code->prepared_byte_size(limits);
          if (!reserve_resident(planned, owned_bytes, !options.max_preparation_resident_bytes))
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "prepared compiler artifact exceeds resident budget");
          charge->bytes = planned;
          limits.resident_bytes = planned - owned_bytes;
          try {
             {
                const auto lock = std::scoped_lock{account->mutex};
                if (!options.max_preparation_workspace_bytes)
                   limits.workspace_bytes =
                       std::min(limits.workspace_bytes, options.max_inflight_bytes - account->inflight_bytes);
                if (!limits.workspace_bytes)
                   FORGE_THROW_EXCEPTION(exceptions::compile_limit,
                                         "compiler preparation has no unreserved inflight storage");
                if (limits.workspace_bytes > options.max_inflight_bytes - account->inflight_bytes)
                   FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler preparation exceeds inflight budget");
                account->inflight_bytes += limits.workspace_bytes;
                charge->workspace_bytes = limits.workspace_bytes;
             }
             detail::compiler_session_probe::reservations_held({generation, record->exchange.job, charge->bytes,
                                                                charge->workspace_bytes, record->resident_bytes,
                                                                token_bytes, false});
             context.throw_if_stop_requested();
             code->prepare(limits, context.stop_token(), charge);
             const auto actual = code->byte_size();
             if (actual > planned)
                FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native preparation exceeded its reservation");
             context.throw_if_stop_requested();
             record->prepared.emplace(std::move(*code));
             code.reset();
             // seal() has destroyed temporary projections and frozen allocation
             // growth at actual. Durable charge now belongs to physical owners.
             {
                const auto lock = std::scoped_lock{account->mutex};
                account->resident_bytes -= planned - actual;
                account->inflight_bytes -= std::exchange(charge->workspace_bytes, 0);
                charge->bytes = actual;
             }
          } catch (...) {
             if (code && !code->retry_cleanup())
                record->cleanup.emplace(std::move(*code));
             code.reset();
             // A failed checked unmap may still own charge. Neither resident nor
             // peak reservation is refunded until its final physical cleanup.
             throw;
          }
          detail::compiler_session_probe::prepared_before_return({generation, record->exchange.job, charge->bytes,
                                                                  charge->workspace_bytes, record->resident_bytes,
                                                                  token_bytes, record->prepared.has_value()});
       });
   // compute checks cancellation after work returns. The prepared object is
   // retained by record even on that path; finish() drops it BEFORE accounting.
   co_return *record->prepared;
}

} // namespace forge::vm::wasm::optimizing
