module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/exceptions/macros.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.asio.notification;
import forge.vm.wasm.interpret.exceptions;
import forge.vm.wasm.optimizing.exceptions;

#include "details/executor_epoch.hxx"
#include "details/native_stack.hxx"
#include "details/executor_impl.hxx"

namespace forge::vm::wasm::optimizing {
namespace {
constexpr std::uint64_t bridge_bytes = 4096;
constexpr std::uint64_t guest_reserve_bytes = 4 * 1024 * 1024;
std::uintptr_t aligned_top(std::uintptr_t value) noexcept {
   return value & ~std::uintptr_t{15};
}
} // namespace

struct executor::impl::thread_state {
   std::shared_ptr<impl> owner;
   native_stack guest;
   native_stack::bounds host_bounds;
   invocation* active = nullptr;
   explicit thread_state(std::shared_ptr<impl> value)
       : owner(std::move(value)), guest(owner->limits.max_guest_stack_bytes),
         host_bounds(native_stack::current_thread_bounds()) {}
   ~thread_state() {
      if (active)
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
   std::unique_ptr<interpret::runtime_context> runtime;
   std::optional<executor_epoch::activation> activation;
   std::function<void(std::uint32_t, interpret::execution_interface)> dispatch;
   std::span<const interpret::operand_stack_elem> arguments;
   std::uint32_t function_index;
   std::uintptr_t guest_top = 0;
   std::uintptr_t boundary = 0;
   std::uintptr_t saved_host_pointer = 0;
   std::uintptr_t paused_guest_pointer = 0;
   std::optional<interpret::operand_stack_elem> value;
   std::exception_ptr error;
   std::exception_ptr host_error;
};

struct executor::impl::bridge {
   invocation& call;
   std::uint32_t ordinal;
   interpret::execution_interface* interface;
   std::uintptr_t saved_guest_pointer = 0;
   std::exception_ptr error;
};

thread_local std::unique_ptr<executor::impl::thread_state> executor::impl::thread;

executor::impl::impl(asio::task::scheduler& scheduler, options value) : limits(value), epoch(scheduler) {
   if (limits.max_guest_stack_bytes <=
           guest_reserve_bytes + native_stack::max_frame_bytes + native_stack::anchor_bytes ||
       limits.min_host_stack_headroom < bridge_bytes ||
       limits.min_host_stack_headroom > UINTPTR_MAX - native_stack::host_red_zone - bridge_bytes)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "Local native stack capacity is unavailable");
}

boost::asio::awaitable<void> executor::impl::start(std::shared_ptr<impl> owner,
                                                   std::chrono::steady_clock::time_point deadline,
                                                   std::stop_token stop) {
   co_await owner->epoch.start(deadline, stop);
}
boost::asio::awaitable<void> executor::impl::shutdown(std::shared_ptr<impl> owner) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   co_await owner->epoch.shutdown();
}
void executor::impl::prepare_thread(std::shared_ptr<impl> owner) {
   if (thread) {
      if (thread->owner != owner)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                               "Native worker belongs to another executor");
      return;
   }
   try {
      thread = std::make_unique<thread_state>(std::move(owner));
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "Native worker allocation unavailable");
   }
}
void executor::impl::release_thread() {
   if (!thread)
      return;
   if (thread->owner.get() != this || thread->active)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                            "Native worker still has an active invocation");
   thread.reset();
}

std::uint64_t executor::impl::guest_entry(void* opaque) noexcept {
   auto& call = *static_cast<invocation*>(opaque);
   try {
      call.runtime->set_native_stack_boundary(reinterpret_cast<std::byte*>(call.boundary));
      call.runtime->set_wasm_allocator(&call.memory);
      call.runtime->set_max_pages(call.code.image().configuration.execution.max_pages);
      const auto stop = call.activation->guest_stop();
      call.runtime->initialize(&call.dispatch, stop);
      call.value = call.runtime->call(&call.dispatch, call.function_index, call.arguments, stop);
   } catch (...) {
      call.error = std::current_exception();
   }
   return 0;
}

void executor::impl::host_body(void* opaque) {
   auto& request = *static_cast<bridge*>(opaque);
   auto& call = request.call;
   if (request.ordinal >= call.imports.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT import ordinal is outside its binding");
   try {
      call.thunk(call.host, std::move(*request.interface), call.imports[request.ordinal]);
   } catch (...) {
      call.host_error = std::current_exception();
      throw;
   }
}

std::uint64_t executor::impl::host_entry(void* opaque) noexcept {
   auto& request = *static_cast<bridge*>(opaque);
   auto& call = request.call;
   const auto previous = std::exchange(call.paused_guest_pointer, request.saved_guest_pointer);
   try {
      host_body(&request);
   } catch (...) {
      request.error = std::current_exception();
   }
   call.paused_guest_pointer = previous;
   return 0;
}

invocation_result executor::impl::invoke(const artifact& code, std::uint64_t max_frame_bytes,
                                         std::span<const std::uint32_t> imports, host_thunk thunk, void* host,
                                         bool requires_host, std::uint32_t function_index,
                                         std::span<const interpret::operand_stack_elem> arguments,
                                         interpret::wasm_allocator& memory,
                                         std::chrono::steady_clock::time_point deadline, std::stop_token caller_stop,
                                         std::stop_token compute_stop) {
   if (!thread || thread->owner.get() != this)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "Native worker is not prepared");
   if ((requires_host && !host) || !thunk || imports.size() != code.image().configuration.imports.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT invocation has an inconsistent host binding");
   const auto functions = code.functions();
   if (function_index >= functions.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT invocation function index is out of bounds");
   const auto& signature = code.image().types[functions[function_index].type_index];
   if (arguments.size() != signature.params.size())
      FORGE_THROW_EXCEPTION(interpret::exceptions::interpreter, "AOT argument count does not match its signature");
   for (auto* outer = thread->active; outer; outer = outer->previous)
      if (&outer->memory == &memory)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                               "Nested AOT invocation aliases outer memory");
   const auto host_pointer = native_stack::current_pointer();
   const auto host_bounds = thread->host_bounds;
   if (host_pointer < host_bounds.low || host_pointer > host_bounds.high ||
       host_pointer - host_bounds.low < limits.min_host_stack_headroom + native_stack::host_red_zone + bridge_bytes)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                            "Ordinary host stack headroom is unavailable");
   auto guest_top = thread->guest.usable().high;
   if (thread->active) {
      if (thread->active->paused_guest_pointer < bridge_bytes)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "Outer AOT guest stack is not paused");
      guest_top = aligned_top(thread->active->paused_guest_pointer - bridge_bytes);
   }
   const auto bounds = thread->guest.usable();
   if (guest_top < bounds.low || guest_top > bounds.high ||
       guest_top - bounds.low <= guest_reserve_bytes + max_frame_bytes + native_stack::anchor_bytes)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                            "Local AOT native stack capacity is unavailable");

   auto call = invocation{*this,          *thread, code, imports, thunk,     host,          memory,
                          thread->active, {},      {},   {},      arguments, function_index};
   const auto restore = boost::scope::scope_exit{[&] { thread->active = call.previous; }};
   thread->active = &call;
   call.guest_top = aligned_top(guest_top);
   call.boundary = bounds.low + guest_reserve_bytes + max_frame_bytes;
   call.activation.emplace(epoch, deadline, caller_stop, compute_stop);
   if (const auto reason = call.activation->poll(); reason != trap_reason::none)
      return {reason, {}, {}};
   const auto source = code.runtime_module();
   call.runtime =
       std::make_unique<interpret::runtime_context>(*source, code.image().configuration.execution.max_call_depth);
   call.dispatch = [&](std::uint32_t index, interpret::execution_interface interface) {
      auto request = bridge{call, index, &interface};
      const auto pointer = call.saved_host_pointer;
      if (pointer < host_bounds.low || pointer > host_bounds.high ||
          pointer - host_bounds.low <
              limits.min_host_stack_headroom + native_stack::host_red_zone + native_stack::anchor_bytes)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "AOT host callback stack is unavailable");
      static_cast<void>(native_stack::call(reinterpret_cast<void*>(aligned_top(pointer - native_stack::host_red_zone)),
                                           host_entry, &request, &request.saved_guest_pointer));
      if (request.error)
         std::rethrow_exception(request.error);
   };
   static_cast<void>(
       native_stack::call(reinterpret_cast<void*>(call.guest_top), guest_entry, &call, &call.saved_host_pointer));
   const auto stopped = call.activation->finish();
   if (call.host_error)
      return {trap_reason::host_exception, {}, call.host_error};
   if (call.error) {
      try {
         std::rethrow_exception(call.error);
      } catch (const interpret::exceptions::canceled&) {
         return {stopped == trap_reason::none ? trap_reason::cancelled : stopped, {}, {}};
      } catch (const interpret::exceptions::guest_trap& trap) {
         using kind = interpret::exceptions::guest_trap_kind;
         switch (trap.kind) {
         case kind::unreachable:
            return {trap_reason::unreachable, {}, {}};
         case kind::table_bounds:
            return {trap_reason::table_bounds, {}, {}};
         case kind::indirect_type:
            return {trap_reason::indirect_type, {}, {}};
         case kind::integer_divide_by_zero:
            return {trap_reason::integer_divide_by_zero, {}, {}};
         case kind::integer_overflow:
            return {trap_reason::integer_overflow, {}, {}};
         case kind::invalid_conversion:
            return {trap_reason::invalid_conversion, {}, {}};
         case kind::logical_depth:
            return {trap_reason::stack_overflow, {}, {}};
         case kind::unknown:
         case kind::auxiliary_stack:
            FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                                  "AOT failure has no supported canonical guest classification");
         }
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "Unknown neutral AOT trap kind");
      } catch (const interpret::exceptions::memory&) {
         return {trap_reason::memory_bounds, {}, {}};
      } catch (const interpret::exceptions::execution_unavailable&) {
         throw;
      } catch (const interpret::exceptions::interpreter&) {
         // Classification must come from the shared typed engine boundary,
         // never exception text or a host-dependent canonical guest trap.
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                               "Typed AOT trap classification is unavailable");
      }
   }
   if (stopped != trap_reason::none)
      return {stopped, {}, {}};
   return {trap_reason::none, std::move(call.value), {}};
}

} // namespace forge::vm::wasm::optimizing
