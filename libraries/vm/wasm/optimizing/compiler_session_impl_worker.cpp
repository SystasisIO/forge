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
         code = co_await prepare(record, std::move(output), stop);
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
   release_resident(std::exchange(record->resident_bytes, 0));
   for (std::size_t i = 0; i < options.max_tickets; ++i) {
      if (const auto consumer = record->consumers[i].lock()) {
         if (error)
            consumer->fail(error);
         else if (result)
            consumer->complete(*result);
      }
   }
   if (record->process && !record->process->is_quiescent()) {
      // Keep the complete owned record/FD/reap state for explicit cleanup retry.
      // It cannot be evicted or destroyed merely because the work coroutine
      // delivered a typed failure. No future admission is allowed while stopping.
      const auto lock = std::scoped_lock{index_mutex};
      stopped = true;
   } else {
      release_job(record);
   }
}

bool compiler_session::impl::reserve_resident(std::uint64_t bytes) {
   for (std::size_t attempt = 0; attempt <= options.max_tickets; ++attempt) {
      auto evicted = std::optional<entry>{};
      {
         const auto lock = std::scoped_lock{index_mutex};
         const auto charge_lock = std::scoped_lock{account->mutex};
         if (bytes <= options.max_resident_bytes - account->resident_bytes) {
            account->resident_bytes += bytes;
            return true;
         }
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
      evicted.reset(); // sole immutable mapping owner drops outside index lock
      release_resident(released);
   }
   return false;
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

boost::asio::awaitable<artifact>
compiler_session::impl::prepare(std::shared_ptr<job> record, std::vector<std::uint8_t> bytes, std::stop_token stop) {
   co_await compute.execute(
       asio::compute::task_options{.name = "wasm-decode-prepare", .parent_stop_token = stop},
       [this, record, bytes = std::move(bytes)](asio::compute::context& context) {
          context.throw_if_stop_requested();
          auto code = std::optional<artifact>{artifact::decode(bytes, *record->verified, options.max_response_bytes)};
          if (code->image().configuration != record->request->configuration)
             FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "compiler artifact configuration mismatch");
          // Sole existing planner, outside every index/product writer lock.
          const auto planned = code->prepared_byte_size();
          if (!reserve_resident(planned))
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "prepared compiler artifact exceeds resident budget");
          auto committed = false;
          try {
             context.throw_if_stop_requested();
             code->prepare();
             if (code->byte_size() != planned)
                FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native preparation accounting mismatch");
             context.throw_if_stop_requested();
             record->prepared.emplace(std::move(*code));
             record->resident_bytes += planned;
             committed = true;
          } catch (...) {
             if (!committed) {
                code.reset(); // unmap/drop decoded storage before releasing reservation
                release_resident(planned);
             }
             throw;
          }
       });
   // compute checks cancellation after work returns. The prepared object is
   // retained by record even on that path; finish() drops it BEFORE accounting.
   co_return *record->prepared;
}

} // namespace forge::vm::wasm::optimizing
