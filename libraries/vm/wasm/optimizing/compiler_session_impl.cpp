module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/system/error_code.hpp>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <exception>
#include <forge/exceptions/macros.hpp>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.compiler_session;

import forge.raw.raw;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"

namespace forge::vm::wasm::optimizing {
namespace {

std::atomic<std::uint64_t> next_session{1};

std::uint64_t session_generation() {
   auto value = next_session.load(std::memory_order_relaxed);
   for (;;) {
      if (value == UINT64_MAX)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler session generation exhausted");
      if (next_session.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
         return value;
   }
}

bool add(std::uint64_t& value, std::uint64_t extra) noexcept {
   if (extra > UINT64_MAX - value)
      return false;
   value += extra;
   return true;
}

bool scaled_add(std::uint64_t& value, std::uint64_t count, std::uint64_t bytes) noexcept {
   return (bytes == 0 || count <= UINT64_MAX / bytes) && add(value, count * bytes);
}

session_options validate_options(session_options value, const asio::compute::executor& compute) {
#if !(defined(__linux__) && defined(__x86_64__)) && !(defined(__APPLE__) && defined(__aarch64__))
   FORGE_THROW_EXCEPTION(exceptions::unsupported_target, "compiler session requires Linux x86-64 or Darwin ARM64");
#endif
   if (!compute.valid() || !value.helper.is_absolute() || !value.cache.is_absolute() || value.worker_count == 0 ||
       value.max_pending_jobs == 0 || value.max_tickets == 0 || value.max_imports == 0 ||
       value.max_imports > raw::max_array_elements || value.max_configuration_bytes == 0 ||
       value.max_request_bytes == 0 || value.max_response_bytes == 0 ||
       value.max_configuration_bytes > raw::max_byte_array_size || value.max_request_bytes > raw::max_byte_array_size ||
       value.max_response_bytes > raw::max_byte_array_size || value.max_inflight_bytes == 0 ||
       value.max_resident_bytes == 0 || value.max_disk_bytes == 0 || value.max_disk_entries == 0 ||
       value.max_disk_entries > std::numeric_limits<std::size_t>::max() - 2 || value.helper_timeout.count() <= 0 ||
       value.worker_count > std::numeric_limits<std::size_t>::max() - value.max_pending_jobs)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "invalid finite compiler session options");
   for (const auto limit :
        {value.helper_limits.cpu_seconds, value.helper_limits.address_space_bytes, value.helper_limits.stack_bytes}) {
      if (limit == 0 || limit >= static_cast<std::uint64_t>(RLIM_INFINITY) ||
          limit > std::numeric_limits<rlim_t>::max())
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler helper resource limit is not finite");
   }
   return value;
}

} // namespace

compiler_session::impl::impl(asio::runtime& runtime_value, asio::compute::executor compute_value,
                             session_options options_value)
    : runtime{runtime_value}, compute{std::move(compute_value)},
      options{validate_options(std::move(options_value), compute)},
      scheduler{runtime, asio::task::scheduler::options{.max_blocking_tasks = 1,
                                                        .max_awaitable_tasks = options.worker_count,
                                                        .max_pending_tasks = options.max_pending_jobs}},
      account{std::make_shared<accounting>()}, live_slots{options.worker_count + options.max_pending_jobs},
      generation{session_generation()} {
   auto index_bytes = std::uint64_t{};
   auto live_bytes = std::uint64_t{};
   auto disk_metadata_bytes = std::uint64_t{};
   // Decode workspace is reserved BEFORE any Raw decode. The existing codec's
   // cumulative element ceiling bounds all non-byte vectors. Include the largest
   // SAME persisted/request element, validation relocation tuples and planner
   // offsets, plus independent input/encoding/checksum byte buffers.
   const auto element_bytes =
       std::max({sizeof(import_signature), sizeof(image_section), sizeof(image_relocation),
                 sizeof(interpret::host_function), sizeof(compiled_function), sizeof(compiled_export),
                 sizeof(global_initial_value), sizeof(memory_initial_segment), sizeof(std::uint32_t)});
   if (!scaled_add(index_bytes, options.max_tickets, sizeof(std::optional<entry>)) ||
       !scaled_add(disk_metadata_bytes, options.max_disk_entries + 1, sizeof(disk_entry)) ||
       !add(index_bytes, disk_metadata_bytes) || !scaled_add(live_bytes, live_slots, sizeof(std::shared_ptr<job>)) ||
       !scaled_add(workspace_bytes, raw::max_array_elements, element_bytes + 4 * sizeof(std::uint64_t)) ||
       !scaled_add(workspace_bytes, options.max_response_bytes, 8) ||
       !scaled_add(workspace_bytes, options.max_request_bytes, 3) ||
       !scaled_add(workspace_bytes, options.max_configuration_bytes, 4) || index_bytes > options.max_resident_bytes ||
       live_bytes > options.max_inflight_bytes || workspace_bytes > options.max_inflight_bytes - live_bytes)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler session accounting preallocation exceeds budget");
   index = std::make_unique<std::optional<entry>[]>(options.max_tickets);
   live = std::make_unique<std::shared_ptr<job>[]>(live_slots);
   disk = std::make_unique<disk_entry[]>(options.max_disk_entries + 1);
   account->resident_bytes = index_bytes;
   account->inflight_bytes = live_bytes;
   cancelled_error = std::make_exception_ptr(exceptions::cancelled{"compiler session was stopped"});
   allocation_error = std::make_exception_ptr(exceptions::compile_limit{"compiler session allocation failed"});
}

compiler_session::impl::~impl() {
   if (!quiescent || shutdown_active || signal_owner)
      std::terminate();
   close_cache(); // only after all tracked scheduler/compute/child drains
}

std::optional<std::uint64_t> compiler_session::impl::configuration_bytes(const compile_configuration& value,
                                                                         const session_options& limits) noexcept {
   if (value.imports.size() > limits.max_imports || value.imports.capacity() > limits.max_imports)
      return {};
   auto bytes = std::uint64_t{sizeof(value)};
   if (!scaled_add(bytes, value.imports.capacity(), sizeof(import_signature)))
      return {};
   for (const auto& binding : value.imports) {
      if (!add(bytes, binding.module.capacity()) || !add(bytes, binding.name.capacity()) ||
          !scaled_add(bytes, binding.signature.params.capacity(), sizeof(interpret::value_type)) ||
          !scaled_add(bytes, binding.signature.ret.capacity(), sizeof(interpret::value_type)))
         return {};
   }
   const auto& target = value.target;
   if (!add(bytes, target.triple.capacity()) || !add(bytes, target.cpu.capacity()) ||
       !add(bytes, target.features.capacity()) || !add(bytes, target.llvm_version.capacity()) ||
       bytes > limits.max_configuration_bytes)
      return {};
   return bytes;
}

std::optional<std::uint64_t> compiler_session::impl::request_bytes(const compile_request& value,
                                                                   const session_options& limits) noexcept {
   auto bytes = configuration_bytes(value.configuration, limits);
   if (!bytes || !add(*bytes, value.wasm.capacity()) || *bytes > limits.max_request_bytes)
      return {};
   return bytes;
}

std::optional<artifact> compiler_session::impl::ready(const crypto::digest::sha256& identity,
                                                      const compile_configuration& expected) const {
   if (!configuration_bytes(expected, options))
      return {};
   const auto lock = std::scoped_lock{index_mutex};
   if (stopped || phase != initialization_phase::ready)
      return {};
   for (std::size_t i = 0; i < options.max_tickets; ++i) {
      const auto& candidate = index[i];
      if (candidate && candidate->identity == identity && candidate->result && candidate->result->code.is_prepared() &&
          candidate->configuration == expected)
         return candidate->result->code;
   }
   return {};
}

compiler_session::enqueue_result compiler_session::impl::enqueue(const crypto::digest::sha256& hint,
                                                                 compile_request&& request,
                                                                 asio::task::priority priority) {
   const auto request_charge = request_bytes(request, options);
   auto job_charge = std::uint64_t{sizeof(job)};
   // Exact-sized consumer slots, never implementation-defined vector growth.
   if (!request_charge || !add(job_charge, *request_charge) || !add(job_charge, workspace_bytes) ||
       !scaled_add(job_charge, options.max_tickets, sizeof(std::weak_ptr<ticket::state>)))
      return {admission_status::saturated, {}};
   constexpr auto ticket_charge =
       std::uint64_t{sizeof(ticket::state) + sizeof(ticket::state::outcome) + sizeof(completion)};
   const auto lock = std::scoped_lock{index_mutex};
   if (stopped || next_job == UINT64_MAX)
      return {admission_status::stopped, {}};
   if (phase != initialization_phase::ready)
      return {admission_status::not_initialized, {}};
   auto free_slot = live_slots;
   for (std::size_t i = 0; i < live_slots; ++i)
      if (!live[i]) {
         free_slot = i;
         break;
      }
   if (free_slot == live_slots)
      return {admission_status::saturated, {}};
   {
      const auto charge_lock = std::scoped_lock{account->mutex};
      auto total = account->inflight_bytes;
      if (account->tickets == options.max_tickets || !add(total, job_charge) || !add(total, ticket_charge) ||
          total > options.max_inflight_bytes)
         return {admission_status::saturated, {}};
      account->inflight_bytes = total;
      ++account->tickets;
   }
   auto consumer = std::shared_ptr<ticket::state>{};
   auto record = std::shared_ptr<job>{};
   auto ticket_charge_owned = false;
   auto job_charge_owned = false;
   try {
      auto* raw_consumer = new ticket::state{};
      ticket_charge_owned = true; // shared_ptr invokes its deleter if control-block allocation fails
      consumer = std::shared_ptr<ticket::state>{raw_consumer, [charge = account](ticket::state* value) {
                                                   delete value;
                                                   const auto charge_lock = std::scoped_lock{charge->mutex};
                                                   charge->inflight_bytes -= ticket_charge;
                                                   --charge->tickets;
                                                }};
      auto* raw_job = new job{};
      raw_job->charged_bytes = job_charge;
      job_charge_owned = true;
      record = std::shared_ptr<job>{raw_job, [charge = account](job* value) {
                                       const auto bytes = value->charged_bytes;
                                       delete value;
                                       const auto charge_lock = std::scoped_lock{charge->mutex};
                                       charge->inflight_bytes -= bytes;
                                    }};
      record->exchange = {generation, next_job++, 1};
      record->hint = hint;
      record->request.emplace(std::move(request));
      record->queued_time = std::chrono::steady_clock::now();
      record->consumers = std::make_unique<std::weak_ptr<ticket::state>[]>(options.max_tickets);
      record->consumers[0] = consumer;
      live[free_slot] = record;
      record->handle = scheduler.submit(asio::task::awaitable{
          .priority = priority, .name = "wasm-compile", .work = [this, record](asio::task::context& context) {
             return run(record, context.stop_token());
          }});
      if (!record->handle.accepted()) {
         live[free_slot].reset();
         return {admission_status::saturated, {}};
      }
      quiescent = false;
      return {admission_status::accepted_unverified, ticket{std::move(consumer)}};
   } catch (...) {
      const auto error = std::current_exception();
      live[free_slot].reset();
      const auto charge_lock = std::scoped_lock{account->mutex};
      if (!job_charge_owned)
         account->inflight_bytes -= job_charge;
      if (!ticket_charge_owned) {
         account->inflight_bytes -= ticket_charge;
         --account->tickets;
      }
      try {
         std::rethrow_exception(error);
      } catch (const std::bad_alloc&) {
         std::rethrow_exception(allocation_error); // Cold error, not another allocation on admission failure.
      }
   }
}

boost::asio::awaitable<void> compiler_session::impl::initialize(target_profile expected, std::stop_token stop) {
   // This is deliberately NOT a coroutine: claim/capture/admit eagerly, before
   // handing a lazy waiter to the caller. No file/process/LLVM work runs here.
   const auto lock = std::scoped_lock{index_mutex};
   if (stopped || phase != initialization_phase::cold || next_job == UINT64_MAX)
      FORGE_THROW_EXCEPTION(asio::exceptions::invalid_state, "compiler initialization already claimed or stopped");
   phase = initialization_phase::initializing;
   auto charge = std::uint64_t{sizeof(job) + sizeof(initialization) +
                               sizeof(std::stop_callback<std::function<void()>>) + sizeof(std::weak_ptr<job>)};
   auto reserved = false;
   auto record = std::shared_ptr<job>{};
   auto charge_owned = false;
   auto accepted = false;
   try {
      auto profile_configuration = compile_configuration{};
      profile_configuration.target = std::move(expected);
      const auto profile_bytes = configuration_bytes(profile_configuration, options);
      if (!profile_bytes || !add(charge, *profile_bytes) || !add(charge, workspace_bytes))
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler initialization metadata exceeds budget");
      {
         const auto charge_lock = std::scoped_lock{account->mutex};
         if (charge > options.max_inflight_bytes - account->inflight_bytes)
            FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler initialization reservation exceeds budget");
         account->inflight_bytes += charge;
         reserved = true;
      }
      auto* raw_record = new job{};
      raw_record->charged_bytes = charge;
      charge_owned = true;
      record = std::shared_ptr<job>{raw_record, [charge_owner = account](job* value) {
                                       const auto bytes = value->charged_bytes;
                                       delete value;
                                       const auto charge_lock = std::scoped_lock{charge_owner->mutex};
                                       charge_owner->inflight_bytes -= bytes;
                                    }};
      record->exchange = {generation, next_job++, 1};
      record->queued_time = std::chrono::steady_clock::now();
      record->setup =
          std::shared_ptr<initialization>{new initialization{}, [charge_owner = account](initialization* value) {
                                             const auto bytes = value->charged_bytes;
                                             delete value;
                                             const auto charge_lock = std::scoped_lock{charge_owner->mutex};
                                             charge_owner->inflight_bytes -= bytes;
                                          }};
      // Transfer only after BOTH shared owners/control blocks exist. If either
      // allocation throws, its existing deleter owns exactly one reservation.
      // A retained lazy outcome conservatively keeps the full initialization
      // workspace charged, even after the tracked job/child has been destroyed.
      record->setup->charged_bytes = std::exchange(record->charged_bytes, 0);
      record->setup->cancelled_error = cancelled_error;
      record->setup->target = std::move(profile_configuration.target);
      record->setup->registration = std::make_unique<std::stop_callback<std::function<void()>>>(
          stop, [weak = std::weak_ptr<job>{record}]() noexcept {
             if (const auto value = weak.lock()) {
                value->setup->cancel();
                // Assignment of the immutable handle happens-before this
                // acquire. Before publication, eager submit checks sticky stop.
                if (value->setup->submitted.load(std::memory_order_acquire))
                   static_cast<void>(value->handle.cancel());
             }
          });
      // Allocate the owning waiter BEFORE submit. A coroutine allocation throw
      // after an accepted task must never erase its retained raw-this record.
      auto waiter = wait_initialize(record->setup);
      live[0] = record; // admission is closed; this is the only possible control job
      record->handle = scheduler.submit(asio::task::awaitable{.priority = asio::task::priority::max(),
                                                              .name = "wasm-initialize",
                                                              .work = [this, record](asio::task::context& context) {
                                                                 return run_initialize(record, context.stop_token());
                                                              }});
      accepted = record->handle.accepted();
      if (!accepted)
         FORGE_THROW_EXCEPTION(exceptions::queue_full, "compiler initialization was not admitted");
      quiescent = false;
      record->setup->submitted.store(true, std::memory_order_release);
      if (record->setup->stop.stop_requested())
         static_cast<void>(record->handle.cancel());
      return waiter; // awaitable's noexcept move; no further allocation after admission
   } catch (...) {
      phase = initialization_phase::failed;
      if (accepted) {
         // Defensive future-proofing: never orphan an already accepted job.
         stopped = true;
         quiescent = false;
         static_cast<void>(record->handle.cancel());
      } else {
         live[0].reset(); // SAME scheduler strong-admission guarantee: no retained runnable
      }
      if (reserved && !charge_owned) {
         const auto charge_lock = std::scoped_lock{account->mutex};
         account->inflight_bytes -= charge;
      }
      throw;
   }
}

void compiler_session::impl::release_job(const std::shared_ptr<job>& record) {
   const auto lock = std::scoped_lock{index_mutex};
   for (std::size_t i = 0; i < live_slots; ++i)
      if (live[i] == record)
         live[i].reset();
   // Charge is released by the final shared owner after request/IPC/consumer
   // storage and scheduler callback captures have actually been destroyed.
}

void compiler_session::impl::request_stop() {
   {
      const auto lock = std::scoped_lock{index_mutex};
      stopped = true;
   }
   // Existing scheduler stop only cancels pending tasks. Explicitly cancel
   // every retained live handle outside the index lock, including running jobs.
   // Admission is already closed. Pin one bounded slot at a time, without an
   // allocating cancellation snapshot. A concurrently removed record has
   // already delivered its outcome; no new job can occupy that slot now.
   for (std::size_t slot = 0; slot < live_slots; ++slot) {
      auto record = std::shared_ptr<job>{};
      {
         const auto lock = std::scoped_lock{index_mutex};
         record = live[slot];
      }
      if (!record)
         continue;
      if (record->setup)
         record->setup->cancel(); // pending work may never enter run_initialize()
      static_cast<void>(record->handle.cancel());
      if (record->consumers)
         for (std::size_t i = 0; i < options.max_tickets; ++i)
            if (const auto consumer = record->consumers[i].lock())
               consumer->fail(cancelled_error);
   }
   scheduler.request_stop();
}

} // namespace forge::vm::wasm::optimizing
