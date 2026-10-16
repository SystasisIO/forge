module;

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.asio.notification;
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.floating_point_environment;
import forge.vm.wasm.optimizing.exceptions;

#include "details/executor_epoch.hxx"
#include "details/native_stack.hxx"
#include "details/executor_impl.hxx"

namespace forge::vm::wasm::optimizing {
namespace {
constexpr std::size_t recovery_bytes = 128 * 1024;
// Existing ordinary-host admission / paused raw-bridge gap. This is NOT the
// guest bottom reserve and must not require 4MiB of ordinary pthread stack.
constexpr std::uint64_t bridge_bytes = 4096;
// Spring donor's conservative fallback headroom, inside the explicitly
// configured usable guest mapping. Not a mathematical libc stack-size maximum;
// arbitrary host C++ executes only on the ordinary host stack.
constexpr std::uint64_t guest_reserve_bytes = 4 * 1024 * 1024;

std::uintptr_t aligned_top(std::uintptr_t pointer) noexcept {
   return pointer & ~std::uintptr_t{15};
}
} // namespace

struct executor::impl::thread_state {
   std::shared_ptr<impl> owner;
   native_stack guest;
   interpret::contiguous_allocator reserve;
   std::span<std::byte> recovery;
   native_stack::bounds host_bounds;
   invocation* active = nullptr;
   bool prepared = false;

   explicit thread_state(std::shared_ptr<impl> value)
       : owner(std::move(value)), guest(owner->limits.max_guest_stack_bytes), reserve(recovery_bytes),
         recovery(reserve.alloc<std::byte>(recovery_bytes), recovery_bytes),
         host_bounds(native_stack::current_thread_bounds()) {}
   ~thread_state() {
      // Also covers compute startup failure, whose already-started workers may
      // exit without invoking on_worker_stop. Borrowed storage is still alive.
      if (active != nullptr)
         std::terminate();
      if (prepared && interpret::release_signal_recovery_thread().stage != interpret::signal_boundary_stage::none)
         std::terminate();
   }
};

struct executor::impl::invocation {
   impl& owner;
   thread_state& worker;
   const artifact& code;
   std::span<const std::uint32_t> imports;
   host_thunk thunk;
   void* host;
   interpret::wasm_allocator& memory;
   invocation* previous;
   invocation_abi context;
   std::optional<executor_epoch::activation> activation;
   std::vector<interpret::expr_value> globals;
   std::vector<table_entry> table;
   std::vector<interpret::expr_value> arguments;
   interpret::expr_value result{};
   entry_function entry = nullptr;
   std::uintptr_t guest_top = 0;
   std::uintptr_t saved_host_pointer = 0;
   std::uintptr_t paused_guest_pointer = 0;
   trap_reason reason = trap_reason::none;
   std::exception_ptr host_error;
   interpret::signal_boundary_status bridge_status;
   bool capacity_unavailable = false;
};

struct executor::impl::bridge {
   enum class operation : std::uint8_t { host, grow, numeric, poll };
   invocation* call;
   operation kind;
   std::uint32_t index = 0;
   std::uint32_t count = 0;
   const interpret::expr_value* arguments = nullptr;
   interpret::expr_value* result = nullptr;
   std::int32_t grown = -1;
   trap_reason reason = trap_reason::none;
   std::uintptr_t saved_guest_pointer = 0;
};

thread_local std::unique_ptr<executor::impl::thread_state> executor::impl::thread;
const invocation_callbacks executor::impl::callbacks{host_call, grow_memory, numeric, poll, trap};

executor::impl::impl(asio::task::scheduler& scheduler, options value) : limits(value), epoch(scheduler) {
   if (limits.max_guest_stack_bytes <= guest_reserve_bytes + native_stack::anchor_bytes ||
       limits.min_host_stack_headroom < bridge_bytes ||
       limits.min_host_stack_headroom > UINTPTR_MAX - native_stack::host_red_zone - bridge_bytes)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "local native stack capacity too small");
}

boost::asio::awaitable<void> executor::impl::start(std::shared_ptr<impl> owner,
                                                   std::chrono::steady_clock::time_point deadline,
                                                   std::stop_token stop) {
   co_await owner->epoch.start(deadline, stop);
}

boost::asio::awaitable<void> executor::impl::shutdown(std::shared_ptr<impl> owner) {
   // Enter in the enclosing coroutine before awaiting mandatory child cleanup.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   co_await owner->epoch.shutdown();
}

void executor::impl::prepare_thread(std::shared_ptr<impl> owner) {
   if (thread) {
      if (thread->owner.get() != owner.get() || !thread->prepared)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker belongs to another owner");
      return;
   }
   try {
      auto prepared = std::make_unique<thread_state>(std::move(owner));
      const auto status = interpret::prepare_signal_recovery_thread(prepared->recovery);
      if (status.stage != interpret::signal_boundary_stage::none)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker recovery was not admitted");
      prepared->prepared = true;
      thread = std::move(prepared);
   } catch (const interpret::exceptions::allocation&) {
      // All partial mappings/reserves have unwound before this operational
      // classification. Shared allocators and guest/host semantics are unchanged.
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker allocation unavailable");
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker allocation unavailable");
   }
}

void executor::impl::release_thread() {
   if (!thread)
      return;
   if (thread->owner.get() != this || thread->active != nullptr)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker still has a live invocation");
   const auto status = interpret::release_signal_recovery_thread();
   if (status.stage != interpret::signal_boundary_stage::none)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker recovery release failed");
   thread->prepared = false;
   thread.reset();
}

interpret::expr_value executor::impl::raw(const interpret::operand_stack_elem& value, interpret::value_type type) {
   auto result = interpret::expr_value{};
   switch (type) {
   case interpret::i32:
      if (!value.is_a<interpret::i32_const_t>())
         FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "native i32 operand type mismatch");
      result.i32 = value.to_i32();
      break;
   case interpret::i64:
      if (!value.is_a<interpret::i64_const_t>())
         FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "native i64 operand type mismatch");
      result.i64 = value.to_i64();
      break;
   case interpret::f32:
      if (!value.is_a<interpret::f32_const_t>())
         FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "native f32 operand type mismatch");
      result.f32 = value.to_fui32();
      break;
   case interpret::f64:
      if (!value.is_a<interpret::f64_const_t>())
         FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "native f64 operand type mismatch");
      result.f64 = value.to_fui64();
      break;
   default:
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native signature contains a nonnumeric type");
   }
   return result;
}

interpret::operand_stack_elem executor::impl::operand(interpret::expr_value value, interpret::value_type type) {
   switch (type) {
   case interpret::i32:
      return interpret::i32_const_t{static_cast<std::uint32_t>(value.i32)};
   case interpret::i64:
      return interpret::i64_const_t{static_cast<std::uint64_t>(value.i64)};
   case interpret::f32:
      return interpret::f32_const_t{std::bit_cast<float>(value.f32)};
   case interpret::f64:
      return interpret::f64_const_t{std::bit_cast<double>(value.f64)};
   default:
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native signature contains a nonnumeric type");
   }
}

invocation_result executor::impl::invoke(const artifact& code, std::uint64_t max_frame_bytes,
                                         std::span<const std::uint32_t> imports, host_thunk thunk, void* host,
                                         bool requires_host, std::uint32_t function_index,
                                         std::span<const interpret::operand_stack_elem> arguments,
                                         interpret::wasm_allocator& memory,
                                         std::chrono::steady_clock::time_point deadline, std::stop_token caller_stop,
                                         std::stop_token compute_stop) {
   // Public invocation is an ordinary host C++ entry; registered callbacks and
   // nested VM callers already enter via the existing whole-NULL boundary.
   // All owners/FP guards/results are outside the destructor-free raw checkpoint.
   const auto environment = interpret::floating_point_environment{};
   if (!thread || thread->owner.get() != this || !thread->prepared)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native worker was not prepared");
   if (requires_host && host == nullptr)
      FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "stateful host instance is required");
   if (thunk == nullptr || imports.size() != code.image().configuration.imports.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native host binding is inconsistent");
   const auto functions = code.functions(); // Ready-only accessor, never prepare.
   const auto& image = code.image();
   if (function_index >= functions.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native function index is outside artifact");
   const auto& signature = image.types[functions[function_index].type_index];
   if (arguments.size() != signature.params.size())
      FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "native argument count does not match signature");
   for (auto* outer = thread->active; outer; outer = outer->previous)
      if (&outer->memory == &memory)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                               "nested native linear memory aliases outer");

   const auto host_pointer = native_stack::current_pointer();
   const auto host_bounds = thread->host_bounds;
   if (host_pointer < host_bounds.low || host_pointer > host_bounds.high ||
       host_pointer - host_bounds.low < limits.min_host_stack_headroom + native_stack::host_red_zone + bridge_bytes)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "ordinary host stack headroom unavailable");
   auto guest_top = thread->guest.usable().high;
   if (thread->active) {
      if (thread->active->paused_guest_pointer < bridge_bytes)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "outer guest stack is not paused");
      guest_top = aligned_top(thread->active->paused_guest_pointer - bridge_bytes);
   }
   const auto guest_bounds = thread->guest.usable();
   if (guest_top < guest_bounds.low || guest_top > guest_bounds.high ||
       guest_top - guest_bounds.low <= native_stack::anchor_bytes)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "nested native stack capacity unavailable");

   // F is an admitted failing callee's full frame: its machine prologue can
   // touch the stack (and call the epoch slow path) BEFORE its IR byte guard.
   // One G bottom reserve is shared by nested invocations, never subtracted
   // from each callback's paused top. Sequential subtraction cannot overflow.
   auto remaining = aligned_top(guest_top) - guest_bounds.low - native_stack::anchor_bytes;
   if (max_frame_bytes > remaining)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native failing-frame room unavailable");
   remaining -= max_frame_bytes;
   if (guest_reserve_bytes > remaining)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native fallback reserve unavailable");
   remaining -= guest_reserve_bytes;
   if (functions[function_index].stack_bytes > remaining)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native entry frame capacity unavailable");
   if (image.start != UINT32_MAX && functions[image.start].stack_bytes > remaining)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native start frame capacity unavailable");

   auto call = invocation{*this, *thread, code, imports, thunk, host, memory, thread->active};
   const auto restore = interpret::scope_guard{[&] { thread->active = call.previous; }};
   thread->active = &call;
   call.guest_top = aligned_top(guest_top);
   call.context.owner = &call;
   call.context.callbacks = &callbacks;
   call.context.functions = functions.data();
   call.context.function_count = static_cast<std::uint32_t>(functions.size());
   call.context.max_call_depth = image.configuration.execution.max_call_depth;
   call.context.stack_low = guest_bounds.low;
   call.context.stack_high = call.guest_top;
   call.context.stack_guard_low = reinterpret_cast<std::uintptr_t>(thread->guest.guard().data());
   call.context.stack_guard_high = call.context.stack_guard_low + thread->guest.guard().size();
   call.context.stack_remaining_bytes = remaining;
   try {
      call.activation.emplace(epoch, call.context, deadline, caller_stop, compute_stop);
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native activation allocation unavailable");
   }
   if (const auto stopped = call.activation->poll(); stopped != trap_reason::none)
      return {stopped, {}, {}};

   try {
      call.arguments.reserve(arguments.size());
      call.globals.reserve(image.globals.size());
      call.table.reserve(image.table.size());
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native activation storage unavailable");
   }
   for (std::size_t index = 0; index < arguments.size(); ++index)
      call.arguments.push_back(raw(arguments[index], signature.params[index]));
   for (const auto& initial : image.globals) {
      auto value = interpret::expr_value{};
      std::memcpy(&value, &initial.bits, sizeof(value));
      call.globals.push_back(value);
   }
   for (const auto index : image.table)
      call.table.push_back({index, index == UINT32_MAX ? UINT32_MAX : functions[index].type_index});
   call.context.globals = call.globals.data();
   call.context.global_count = static_cast<std::uint32_t>(call.globals.size());
   call.context.table = call.table.data();
   call.context.table_count = static_cast<std::uint32_t>(call.table.size());
   if (image.memory.defined) {
      call.context.max_pages = std::min(image.memory.max_pages, image.configuration.execution.max_pages);
      if (image.memory.initial_pages > call.context.max_pages)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native initial memory exceeds execution limit");
      try {
         memory.reset(image.memory.initial_pages);
      } catch (const interpret::exceptions::allocation&) {
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native initial memory unavailable");
      } catch (const std::bad_alloc&) {
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native initial memory unavailable");
      }
      call.context.memory_base = memory.get_base_ptr<std::uint8_t>();
      call.context.current_pages = image.memory.initial_pages;
      call.context.memory_bytes = static_cast<std::uint64_t>(call.context.current_pages) * interpret::page_size;
      for (const auto& segment : image.data)
         std::memcpy(call.context.memory_base + segment.offset, segment.bytes.data(), segment.bytes.size());
   } else {
      try {
         memory.reset();
      } catch (const interpret::exceptions::allocation&) {
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native initial memory unavailable");
      } catch (const std::bad_alloc&) {
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native initial memory unavailable");
      }
   }

   // Start and export share ONE fresh globals/table/memory/activation state.
   const auto run = [&](std::uint32_t index, const interpret::expr_value* raw_arguments) {
      if (const auto stopped = call.activation->poll(); stopped != trap_reason::none) {
         call.reason = stopped;
         return;
      }
      call.entry = functions[index].entry;
      auto entry_state = std::pair{&call, raw_arguments};
      const auto code_range = code.native_code();
      const auto memory_range = image.memory.defined ? memory.get_span() : interpret::span<std::byte>{};
      const auto result = interpret::invoke_native_with_signals(
          {code_range, {memory_range.data(), memory_range.size()}, thread->guest.guard()}, thread->recovery,
          +[](void* opaque) noexcept {
             const auto state = *static_cast<decltype(entry_state)*>(opaque);
             auto& invocation = *state.first;
             // Arguments are invocation-owned; start supplies nullptr. The raw
             // entry adapter alone lives below the signal checkpoint.
             struct guest_state {
                impl::invocation* call;
                const interpret::expr_value* arguments;
             };
             auto guest = guest_state{&invocation, state.second};
             static_cast<void>(native_stack::call(
                 reinterpret_cast<void*>(invocation.guest_top),
                 +[](void* raw) noexcept -> std::uint64_t {
                    const auto& guest = *static_cast<guest_state*>(raw);
                    guest.call->entry(&guest.call->context, guest.arguments, &guest.call->result);
                    return 0;
                 },
                 &guest, &invocation.saved_host_pointer));
          },
          &entry_state);
      if (call.bridge_status.stage != interpret::signal_boundary_stage::none || call.capacity_unavailable)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native host bridge was unavailable");
      if (result.status.stage != interpret::signal_boundary_stage::none)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native recovery entry was unavailable");
      if (call.reason == trap_reason::none)
         switch (result.fault) {
         case interpret::native_signal_fault::none:
            break;
         case interpret::native_signal_fault::memory:
            call.reason = trap_reason::memory_bounds;
            break;
         case interpret::native_signal_fault::stack:
            call.reason = trap_reason::native_stack_exhausted;
            break;
         case interpret::native_signal_fault::arithmetic:
            call.reason = trap_reason::integer_divide_by_zero;
            break;
         default:
            call.reason = trap_reason::unreachable;
            break;
         }
   };
   if (image.start != UINT32_MAX)
      run(image.start, nullptr);
   if (call.reason == trap_reason::none) {
      call.reason = call.activation->poll();
      if (call.reason == trap_reason::none)
         run(function_index, call.arguments.data());
   }
   const auto final = call.activation->finish(); // Callback deregistration precedes final real clock check.
   if (call.reason == trap_reason::none)
      call.reason = final;
   // Only the owned local frame allowance/physical guard uses this reason.
   // Canonical call-depth overflow remains a guest trap. Raw recovery, host
   // cleanup and stop callback deregistration have completed before this throw;
   // ordinary C++ unwind restores the active invocation and retires its owners.
   if (call.reason == trap_reason::native_stack_exhausted)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "local native stack exhausted");
   auto result = invocation_result{call.reason, {}, call.host_error};
   if (call.reason == trap_reason::none && !signature.ret.empty())
      result.value = operand(call.result, signature.ret.front());
   return result;
}

std::uint64_t executor::impl::host_entry(void* opaque) noexcept {
   auto& request = *static_cast<bridge*>(opaque);
   auto& call = *request.call;
   const auto previous = call.paused_guest_pointer;
   call.paused_guest_pointer = request.saved_guest_pointer;
   call.bridge_status = interpret::invoke_without_signal_recovery(host_body, &request);
   call.paused_guest_pointer = previous;
   if (call.bridge_status.stage != interpret::signal_boundary_stage::none)
      request.reason = trap_reason::host_exception;
   return 0;
}

void executor::impl::host_body(void* opaque) noexcept {
   auto& request = *static_cast<bridge*>(opaque);
   auto& call = *request.call;
   request.reason = call.activation->poll();
   if (request.reason != trap_reason::none)
      return;
   try {
      const auto environment = interpret::floating_point_environment{};
      switch (request.kind) {
      case bridge::operation::host: {
         if (request.index >= call.imports.size())
            FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native import ordinal outside binding");
         const auto& signature = call.code.image().configuration.imports[request.index].signature;
         auto operands = interpret::operand_stack{};
         for (std::size_t index = 0; index < signature.params.size(); ++index)
            operands.push(operand(request.arguments[index], signature.params[index]));
         call.thunk(call.host,
                    interpret::execution_interface{reinterpret_cast<char*>(call.context.memory_base),
                                                   static_cast<std::size_t>(call.context.memory_bytes), &operands},
                    call.imports[request.index]);
         if (!signature.ret.empty())
            *request.result = raw(operands.pop(), signature.ret.front());
         break;
      }
      case bridge::operation::grow: {
         const auto previous = call.context.current_pages;
         if (request.index > call.context.max_pages - previous)
            break;
         try {
            call.memory.alloc<char>(request.index);
         } catch (const interpret::exceptions::allocation&) {
            break; // Normal WASM memory.grow failure, never an interrupt encoding.
         }
         call.context.memory_base = call.memory.get_base_ptr<std::uint8_t>();
         call.context.current_pages = static_cast<std::uint32_t>(call.memory.get_current_page());
         call.context.memory_bytes = static_cast<std::uint64_t>(call.context.current_pages) * interpret::page_size;
         request.grown = static_cast<std::int32_t>(previous);
         break;
      }
      case bridge::operation::numeric:
         if (request.count > 2)
            FORGE_THROW_EXCEPTION(interpret::exceptions::numeric_arity, "native numeric argument count too large");
         try {
            *request.result = interpret::execute_numeric(static_cast<interpret::opcodes>(request.index),
                                                         {request.arguments, request.count});
         } catch (const interpret::exceptions::interpreter&) {
            request.reason = trap_reason::invalid_conversion;
         }
         break;
      case bridge::operation::poll:
         break;
      }
   } catch (const interpret::exceptions::memory&) {
      request.reason = trap_reason::memory_bounds;
   } catch (...) {
      call.host_error = std::current_exception();
      request.reason = trap_reason::host_exception;
   }
   // Every converter/proxy/user/local-catch/FP destructor has completed while
   // NULL. In particular grow's int32 result must not hide a terminal interrupt.
   const auto after = call.activation->poll();
   if (request.reason == trap_reason::none)
      request.reason = after;
}

void executor::impl::bounce(bridge& request) noexcept {
   auto& call = *request.call;
   const auto pointer = call.saved_host_pointer;
   const auto bounds = call.worker.host_bounds;
   if (pointer < bounds.low || pointer > bounds.high ||
       pointer - bounds.low <
           call.owner.limits.min_host_stack_headroom + native_stack::host_red_zone + native_stack::anchor_bytes) {
      call.capacity_unavailable = true;
      trap(&call.context, trap_reason::host_exception);
   }
   const auto top = aligned_top(pointer - native_stack::host_red_zone);
   static_cast<void>(
       native_stack::call(reinterpret_cast<void*>(top), host_entry, &request, &request.saved_guest_pointer));
}

trap_reason executor::impl::host_call(invocation_abi* context, std::uint32_t index,
                                      const interpret::expr_value* arguments, interpret::expr_value* result) noexcept {
   auto request =
       bridge{static_cast<invocation*>(context->owner), bridge::operation::host, index, 0, arguments, result};
   bounce(request);
   return request.reason;
}

std::int32_t executor::impl::grow_memory(invocation_abi* context, std::uint32_t delta) noexcept {
   auto request = bridge{static_cast<invocation*>(context->owner), bridge::operation::grow, delta};
   bounce(request);
   // POD-only tail: no C++ host frame remains when existing raw recovery leaves.
   if (request.reason != trap_reason::none)
      trap(context, request.reason);
   return request.grown;
}

trap_reason executor::impl::numeric(invocation_abi* context, std::uint32_t opcode, std::uint32_t count,
                                    const interpret::expr_value* arguments, interpret::expr_value* result) noexcept {
   auto request =
       bridge{static_cast<invocation*>(context->owner), bridge::operation::numeric, opcode, count, arguments, result};
   bounce(request);
   return request.reason;
}

trap_reason executor::impl::poll(invocation_abi* context) noexcept {
   auto request = bridge{static_cast<invocation*>(context->owner), bridge::operation::poll};
   bounce(request);
   return request.reason;
}

[[noreturn]] void executor::impl::trap(invocation_abi* context, trap_reason reason) noexcept {
   auto& call = *static_cast<invocation*>(context->owner);
   if (call.reason == trap_reason::none)
      call.reason = reason == trap_reason::none ? trap_reason::unreachable : reason;
   interpret::leave_native_with_signals();
}

} // namespace forge::vm::wasm::optimizing
