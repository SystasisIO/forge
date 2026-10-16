module;

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <span>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/arm/exception.h>
#include <mach/arm/thread_status.h>

extern "C" {
#include "forge_mach_exc_server.h"
}

module forge.vm.wasm.interpret.backend;

import :signals;

#include "details/mach_recovery.hxx"

namespace forge::vm::wasm::interpret {

namespace {

// This bounds cold OS registrations, not guest recursion or chain semantics.
constexpr std::size_t registration_limit = 128;
constexpr std::size_t legacy_reserve_bytes = 128 * 1024;
constexpr exception_mask_t owned_exceptions = EXC_MASK_BAD_ACCESS | EXC_MASK_ARITHMETIC;
constexpr std::uintptr_t landing_alignment = 16;

struct previous_routes {
   std::array<exception_mask_t, EXC_TYPES_COUNT> masks{};
   std::array<mach_port_t, EXC_TYPES_COUNT> ports{};
   std::array<exception_behavior_t, EXC_TYPES_COUNT> behaviors{};
   std::array<thread_state_flavor_t, EXC_TYPES_COUNT> flavors{};
   mach_msg_type_number_t count = EXC_TYPES_COUNT;
};

struct registration {
   std::atomic<bool> ready{false};
   std::atomic<std::uint64_t> generation{0};
   std::atomic<unsigned> readers{0};
   mach_recovery::view owner;
   std::span<std::byte> reserve;
   previous_routes previous;
   mach_port_t thread = MACH_PORT_NULL;
   std::uint64_t thread_id = 0;
   mach_port_t port = MACH_PORT_NULL;
   bool own_send_right = false;
   bool allocated = false; // Cold preparation/release lock only.
};

struct service {
   std::array<registration, registration_limit> slots;
   mach_port_t port_set = MACH_PORT_NULL;
   std::thread receiver;
   std::atomic<bool> stopping{false};
   std::size_t leases = 0;
};

std::mutex preparation_mutex;
service* process_service = nullptr;
std::atomic<bool> inherited_fork{false};
bool fork_hook_installed = false;

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

[[noreturn]] void broken_restoration() noexcept {
   // Only an OS ownership/restoration invariant may use this policy.
   // Never return a borrowed landing span while an installed route references it.
   std::_Exit(EXIT_FAILURE);
}

void child_after_fork() noexcept {
   inherited_fork.store(true, std::memory_order_relaxed);
}

void drop_routes(previous_routes& routes) noexcept {
   for (mach_msg_type_number_t index = 0; index < routes.count; ++index) {
      if (MACH_PORT_VALID(routes.ports[index]) &&
          mach_port_deallocate(mach_task_self(), routes.ports[index]) != KERN_SUCCESS)
         broken_restoration();
      routes.ports[index] = MACH_PORT_NULL;
   }
   routes.count = 0;
}

bool supported_routes(const previous_routes& routes) noexcept {
   for (mach_msg_type_number_t index = 0; index < routes.count; ++index) {
      // First implementation slice deliberately does NOT synchronously call a
      // previous thread handler on the shared receiver. A debugger/crash handler
      // can wait indefinitely; an arbitrary timeout would change its semantics.
      // The EOS-VM port deliberately rejects a conflicting per-thread owner;
      // a generic forwarding proxy is outside the approved port's scope.
      if (routes.ports[index] != MACH_PORT_NULL)
         return false;
   }
   return true;
}

void restore_routes(const registration& slot) noexcept {
   // Clear only the two masks this owner installed, then restore every atomic-swap tuple exactly.
   if (thread_set_exception_ports(slot.thread, owned_exceptions, MACH_PORT_NULL, EXCEPTION_DEFAULT,
                                  THREAD_STATE_NONE) != KERN_SUCCESS)
      broken_restoration();
   for (mach_msg_type_number_t index = 0; index < slot.previous.count; ++index) {
      if (thread_set_exception_ports(slot.thread, slot.previous.masks[index], slot.previous.ports[index],
                                     slot.previous.behaviors[index], slot.previous.flavors[index]) != KERN_SUCCESS)
         broken_restoration();
   }
}

void destroy_route_port(registration& slot) noexcept {
   if (MACH_PORT_VALID(slot.port)) {
      if (mach_port_mod_refs(mach_task_self(), slot.port, MACH_PORT_RIGHT_RECEIVE, -1) != KERN_SUCCESS)
         broken_restoration();
      if (slot.own_send_right && mach_port_deallocate(mach_task_self(), slot.port) != KERN_SUCCESS)
         broken_restoration();
   }
   slot.own_send_right = false;
   slot.port = MACH_PORT_NULL;
   if (MACH_PORT_VALID(slot.thread) && mach_port_deallocate(mach_task_self(), slot.thread) != KERN_SUCCESS)
      broken_restoration();
   slot.thread = MACH_PORT_NULL;
}

struct thread_lease {
   service* receiver = nullptr;
   registration* slot = nullptr;
   void* owned_mapping = MAP_FAILED;
   std::size_t mapping_bytes = 0;
   unsigned invocations = 0;

   ~thread_lease() {
      if (inherited_fork.load(std::memory_order_relaxed))
         return; // No inherited thread/receiver cleanup in a post-fork child.
      if (slot && mach_recovery::release().stage != signal_boundary_stage::none)
         broken_restoration();
   }
};

thread_local thread_lease current_lease;

bool contains(std::span<const std::byte> range, std::uintptr_t address) noexcept {
   if (range.empty())
      return false;
   const auto start = reinterpret_cast<std::uintptr_t>(range.data());
   return address >= start && address - start < range.size();
}

// Runs on the origin thread, but only AFTER the kernel has selected our admitted
// host-owned SP. It does not execute C++ cleanup on an exhausted guest stack.
[[noreturn]] void hardware_landing(registration* slot, int signal, native_signal_fault fault) noexcept {
   sigset_t transition;
   sigemptyset(&transition);
   sigaddset(&transition, SIGSEGV);
   sigaddset(&transition, SIGBUS);
   sigaddset(&transition, SIGFPE);
   sigaddset(&transition, SIGPROF);
   if (pthread_sigmask(SIG_BLOCK, &transition, nullptr) != 0)
      broken_restoration();
   slot->owner.leave(signal, fault);
   broken_restoration();
}

kern_return_t redirect_managed(registration& slot, exception_type_t exception, mach_exception_data_t code,
                               mach_msg_type_number_t code_count) noexcept {
   auto* destination = slot.owner.destination->load(std::memory_order_seq_cst);
   if (!destination || code_count != 2)
      return KERN_FAILURE;
   if (exception == EXC_BAD_ACCESS && code[0] != KERN_INVALID_ADDRESS && code[0] != KERN_PROTECTION_FAILURE)
      return KERN_FAILURE; // Alignment, watch/debug and PAC failures are not guest memory traps.
   if (exception == EXC_ARITHMETIC && code[0] != EXC_ARM_FP_DZ)
      return KERN_FAILURE; // Only the public divide-by-zero hardware code is admitted in this first slice.

   arm_thread_state64_t state{};
   auto state_count = static_cast<mach_msg_type_number_t>(ARM_THREAD_STATE64_COUNT);
   const auto queried =
       thread_get_state(slot.thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &state_count);
   if (queried != KERN_SUCCESS || state_count != ARM_THREAD_STATE64_COUNT)
      return KERN_FAILURE;
   const auto pc = static_cast<std::uintptr_t>(arm_thread_state64_get_pc(state));
   const auto address = static_cast<std::uintptr_t>(code[1]);
   auto fault = native_signal_fault::none;
   if (*slot.owner.strict_native) {
      if (!contains(*slot.owner.code, pc))
         return KERN_FAILURE;
      if (exception == EXC_ARITHMETIC) {
         fault = native_signal_fault::arithmetic;
      } else if (contains(*slot.owner.stack_guard, address)) {
         fault = native_signal_fault::stack;
      } else if (contains(*slot.owner.memory, address)) {
         fault = native_signal_fault::memory;
      } else if (contains(*slot.owner.code, address)) {
         fault = native_signal_fault::code;
      }
   } else {
      // Interpreter PC is a host instruction, not a bytecode address. Preserve
      // its PRIVATE legacy empty-range contract for trivial guest checkpoints.
      // Public native entry never takes this branch; whole C++ host lifetimes
      // have a NULL destination and were rejected before any classification.
      if (slot.owner.code->empty() && slot.owner.memory->empty()) {
         fault = exception == EXC_ARITHMETIC ? native_signal_fault::arithmetic : native_signal_fault::memory;
      } else if (contains(*slot.owner.memory, address)) {
         fault = native_signal_fault::memory;
      } else if (contains(*slot.owner.code, address)) {
         if (slot.owner.timeout_count->load(std::memory_order_acquire) == 0)
            return KERN_SUCCESS; // Existing watchdog restored its code protection; retry the same instruction.
         fault = native_signal_fault::code;
      }
   }
   if (fault == native_signal_fault::none)
      return KERN_FAILURE;

   const auto start = reinterpret_cast<std::uintptr_t>(slot.reserve.data());
   const auto top = (start + slot.reserve.size()) & ~(landing_alignment - 1);
   const auto signal = exception == EXC_ARITHMETIC ? SIGFPE : code[0] == KERN_INVALID_ADDRESS ? SIGSEGV : SIGBUS;
   state.__x[0] = reinterpret_cast<std::uintptr_t>(&slot);
   state.__x[1] = static_cast<std::uintptr_t>(signal);
   state.__x[2] = static_cast<std::uintptr_t>(fault);
   arm_thread_state64_set_pc_fptr(state, &hardware_landing);
   arm_thread_state64_set_lr_fptr(state, &hardware_landing);
   arm_thread_state64_set_sp(state, top);
   arm_thread_state64_set_fp(state, 0);
   return thread_set_state(slot.thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), state_count);
}

// Private receiver-local state is live only during synchronous SDK demultiplexing.
// Its record is fixed-slot storage, not an invocation/context/cache duplicate.
thread_local service* receiving_service = nullptr;
thread_local bool receiving_kernel_message = false;

kern_return_t handle_exception(mach_port_t port, mach_port_t thread, mach_port_t task, exception_type_t exception,
                               mach_exception_data_t code, mach_msg_type_number_t code_count,
                               audit_token_t sender) noexcept {
   constexpr audit_token_t kernel_sender = KERNEL_AUDIT_TOKEN_VALUE;
   // SDK-generated ServerAuditToken extraction + the kernel-supplied receive
   // trailer authenticate the sender BEFORE any origin TLS/state is accessed.
   // Matching same-task send rights alone does not prove the thread is stopped.
   if (!receiving_kernel_message || std::memcmp(&sender, &kernel_sender, sizeof(sender)) != 0 || !receiving_service ||
       !mach_task_is_self(task) || code_count != 2 || (exception != EXC_BAD_ACCESS && exception != EXC_ARITHMETIC))
      return KERN_FAILURE;
   mach_port_context_t context = 0;
   if (mach_port_get_context(mach_task_self(), port, &context) != KERN_SUCCESS)
      return KERN_FAILURE;
   const auto encoded_index = static_cast<std::size_t>(context & 0xffU);
   if (encoded_index == 0 || encoded_index > registration_limit)
      return KERN_FAILURE;
   auto& slot = receiving_service->slots[encoded_index - 1];
   slot.readers.fetch_add(1, std::memory_order_seq_cst);
   kern_return_t result = KERN_FAILURE;
   // Acquire-ready publishes all non-atomic fields; generation is checked AFTER
   // that acquire. Release clears ready and drains admitted readers before reuse.
   if (slot.ready.load(std::memory_order_seq_cst) &&
       slot.generation.load(std::memory_order_acquire) == (context >> 8U)) {
      thread_identifier_info_data_t identity{};
      auto count = static_cast<mach_msg_type_number_t>(THREAD_IDENTIFIER_INFO_COUNT);
      if (thread_info(thread, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identity), &count) ==
              KERN_SUCCESS &&
          count == THREAD_IDENTIFIER_INFO_COUNT && identity.thread_id == slot.thread_id)
         result = redirect_managed(slot, exception, code, code_count);
      // Non-null previous thread routes were rejected and exactly restored
      // during cold admission. Failure now permits the genuine task/default path.
   }
   slot.readers.fetch_sub(1, std::memory_order_seq_cst);
   // Drop the borrowed record before the SDK reply may resume the origin thread.
   return result;
}

void receive_exceptions(service* owner) noexcept {
   receiving_service = owner;
   constexpr auto request_bytes = sizeof(union __RequestUnion__catch_forge_vm_mach_exc_subsystem) + MAX_TRAILER_SIZE;
   constexpr auto reply_bytes = sizeof(union __ReplyUnion__catch_forge_vm_mach_exc_subsystem);
   alignas(mach_msg_header_t) std::array<std::byte, request_bytes> request{};
   alignas(mach_msg_header_t) std::array<std::byte, reply_bytes> reply{};
   while (true) {
      auto* incoming = reinterpret_cast<mach_msg_header_t*>(request.data());
      const auto receive_options = MACH_RCV_MSG | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                   MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
      const auto received =
          mach_msg(incoming, receive_options, 0, request_bytes, owner->port_set, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
      if (received == MACH_RCV_PORT_CHANGED) {
         if (owner->stopping.load(std::memory_order_acquire))
            break;
         continue; // Cold member removal can wake a receive while sibling registrations remain active.
      }
      if ((received == MACH_RCV_PORT_DIED || received == MACH_RCV_INVALID_NAME) &&
          owner->stopping.load(std::memory_order_acquire))
         break; // Only acknowledged cold retirement may terminate this owner.
      if (received == MACH_RCV_TOO_LARGE || received == MACH_RCV_INTERRUPTED)
         continue; // Without MACH_RCV_LARGE the kernel destroys an oversized incoming message/rights.
      if (received != MACH_MSG_SUCCESS)
         broken_restoration();
      receiving_kernel_message = false;
      const auto trailer_offset = static_cast<std::size_t>(round_msg(incoming->msgh_size));
      if (trailer_offset <= request.size() - sizeof(mach_msg_audit_trailer_t)) {
         const auto* trailer = reinterpret_cast<const mach_msg_audit_trailer_t*>(request.data() + trailer_offset);
         constexpr audit_token_t kernel_sender = KERNEL_AUDIT_TOKEN_VALUE;
         receiving_kernel_message = trailer->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 &&
                                    trailer->msgh_trailer_size >= sizeof(mach_msg_audit_trailer_t) &&
                                    trailer->msgh_trailer_size <= request.size() - trailer_offset &&
                                    std::memcmp(&trailer->msgh_audit, &kernel_sender, sizeof(kernel_sender)) == 0;
      }
      auto* outgoing = reinterpret_cast<mach_msg_header_t*>(reply.data());
      static_cast<void>(forge_vm_mach_exc_server(incoming, outgoing));
      receiving_kernel_message = false;
      // The SDK server borrows descriptors. Destroy their received rights exactly
      // once, but transfer the send-once reply right to its generated reply first.
      incoming->msgh_remote_port = MACH_PORT_NULL;
      mach_msg_destroy(incoming);
      if (MACH_PORT_VALID(outgoing->msgh_remote_port)) {
         const auto sent = mach_msg(outgoing, MACH_SEND_MSG | MACH_SEND_TIMEOUT, outgoing->msgh_size, 0, MACH_PORT_NULL,
                                    0, MACH_PORT_NULL);
         if (sent != MACH_MSG_SUCCESS)
            mach_msg_destroy(outgoing);
      }
   }
   receiving_service = nullptr;
}

signal_boundary_status start_service() noexcept {
   if (!fork_hook_installed) {
      const auto registered = pthread_atfork(nullptr, nullptr, &child_after_fork);
      if (registered != 0)
         return {signal_boundary_stage::mach_service, registered, 0};
      fork_hook_installed = true;
   }
   auto* candidate = new (std::nothrow) service;
   if (!candidate)
      return {signal_boundary_stage::mach_service, ENOMEM, 0};
   const auto allocated = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &candidate->port_set);
   if (allocated != KERN_SUCCESS) {
      delete candidate;
      return {signal_boundary_stage::mach_service, allocated, 0};
   }
   try {
      candidate->receiver = std::thread{&receive_exceptions, candidate};
   } catch (...) {
      if (mach_port_mod_refs(mach_task_self(), candidate->port_set, MACH_PORT_RIGHT_PORT_SET, -1) != KERN_SUCCESS)
         broken_restoration();
      delete candidate;
      return {signal_boundary_stage::mach_service, EAGAIN, 0};
   }
   process_service = candidate;
   return {};
}

void stop_empty_service() noexcept {
   if (!process_service || process_service->leases != 0)
      return;
   auto* retired = process_service;
   process_service = nullptr;
   retired->stopping.store(true, std::memory_order_release);
   if (mach_port_mod_refs(mach_task_self(), retired->port_set, MACH_PORT_RIGHT_PORT_SET, -1) != KERN_SUCCESS)
      broken_restoration();
   retired->receiver.join();
   delete retired;
}

bool valid_view(const mach_recovery::view& owner) noexcept {
   return owner.destination && owner.code && owner.memory && owner.stack_guard && owner.strict_native &&
          owner.timeout_count && owner.leave;
}

} // namespace

signal_boundary_status mach_recovery::prepare(view owner, std::span<std::byte> reserve) noexcept {
   if (inherited_fork.load(std::memory_order_relaxed))
      return {signal_boundary_stage::fork_child, ECHILD, 0};
   if (!valid_view(owner) || !reserve.data() || reserve.size() < legacy_reserve_bytes ||
       reinterpret_cast<std::uintptr_t>(reserve.data()) > std::numeric_limits<std::uintptr_t>::max() - reserve.size())
      return {signal_boundary_stage::signal_stack, EINVAL, 0};
   if (current_lease.slot)
      return matches_reserve(reserve) ? signal_boundary_status{}
                                      : signal_boundary_status{signal_boundary_stage::thread_prepare, EBUSY, 0};
   try {
      const auto lock = std::scoped_lock{preparation_mutex};
      if (!process_service) {
         const auto status = start_service();
         if (status.stage != signal_boundary_stage::none)
            return status;
      }
      registration* slot = nullptr;
      std::size_t slot_index = 0;
      for (; slot_index < registration_limit; ++slot_index) {
         if (!process_service->slots[slot_index].allocated) {
            slot = &process_service->slots[slot_index];
            break;
         }
      }
      if (!slot) {
         stop_empty_service();
         return {signal_boundary_stage::thread_capacity, EAGAIN, 0};
      }
      const auto generation = slot->generation.load(std::memory_order_relaxed);
      if (generation == (std::numeric_limits<mach_port_context_t>::max() >> 8U)) {
         stop_empty_service();
         return {signal_boundary_stage::thread_capacity, EOVERFLOW, 0};
      }
      slot->generation.store(generation + 1, std::memory_order_relaxed);
      slot->owner = owner;
      slot->reserve = reserve;
      slot->previous = {};
      slot->thread = mach_thread_self();
      thread_identifier_info_data_t identity{};
      auto identity_count = static_cast<mach_msg_type_number_t>(THREAD_IDENTIFIER_INFO_COUNT);
      auto result = thread_info(slot->thread, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identity),
                                &identity_count);
      if (result == KERN_SUCCESS && identity_count != THREAD_IDENTIFIER_INFO_COUNT)
         result = KERN_FAILURE;
      slot->thread_id = identity.thread_id;
      if (result == KERN_SUCCESS)
         result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &slot->port);
      if (result == KERN_SUCCESS)
         result = mach_port_insert_right(mach_task_self(), slot->port, slot->port, MACH_MSG_TYPE_MAKE_SEND);
      if (result == KERN_SUCCESS)
         slot->own_send_right = true;
      if (result == KERN_SUCCESS)
         result = mach_port_set_context(mach_task_self(), slot->port, ((generation + 1) << 8U) | (slot_index + 1));
      if (result == KERN_SUCCESS)
         result = mach_port_move_member(mach_task_self(), slot->port, process_service->port_set);
      if (result != KERN_SUCCESS) {
         destroy_route_port(*slot);
         stop_empty_service();
         return {signal_boundary_stage::thread_prepare, result, 0};
      }
      result = thread_swap_exception_ports(
          slot->thread, owned_exceptions, slot->port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64,
          slot->previous.masks.data(), &slot->previous.count, slot->previous.ports.data(),
          slot->previous.behaviors.data(), slot->previous.flavors.data());
      if (result != KERN_SUCCESS || !supported_routes(slot->previous)) {
         if (result == KERN_SUCCESS)
            restore_routes(*slot);
         else
            slot->previous.count = 0; // Failed swap did not transfer an old route tuple.
         drop_routes(slot->previous);
         destroy_route_port(*slot);
         stop_empty_service();
         return {signal_boundary_stage::mach_routes, result == KERN_SUCCESS ? ENOTSUP : result, 0};
      }
      slot->allocated = true;
      slot->ready.store(true, std::memory_order_release);
      ++process_service->leases;
      current_lease.receiver = process_service;
      current_lease.slot = slot;
      return {};
   } catch (...) {
      return {signal_boundary_stage::thread_prepare, EIO, 0};
   }
}

signal_boundary_status mach_recovery::prepare_legacy(view owner) noexcept {
   if (inherited_fork.load(std::memory_order_relaxed))
      return {signal_boundary_stage::fork_child, ECHILD, 0};
   if (current_lease.slot)
      return {};
   const auto page_query = sysconf(_SC_PAGESIZE);
   if (page_query <= 0)
      return {signal_boundary_stage::thread_prepare, EINVAL, 0};
   const auto page = static_cast<std::size_t>(page_query);
   const auto bytes = ((legacy_reserve_bytes + page - 1) / page) * page;
   const auto total = bytes + page;
   auto* mapping = static_cast<std::byte*>(mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
   if (mapping == MAP_FAILED)
      return {signal_boundary_stage::thread_prepare, errno, 0};
   if (mprotect(mapping + page, bytes, PROT_READ | PROT_WRITE) != 0) {
      const auto error = errno;
      if (munmap(mapping, total) != 0)
         broken_restoration();
      return {signal_boundary_stage::thread_prepare, error, 0};
   }
   const auto status = prepare(owner, {mapping + page, bytes});
   if (status.stage != signal_boundary_stage::none) {
      if (munmap(mapping, total) != 0)
         broken_restoration();
      return status;
   }
   current_lease.owned_mapping = mapping;
   current_lease.mapping_bytes = total;
   return {};
}

signal_boundary_status mach_recovery::release() noexcept {
   if (inherited_fork.load(std::memory_order_relaxed))
      return {signal_boundary_stage::fork_child, ECHILD, 0};
   if (!current_lease.slot)
      return {};
   if (current_lease.invocations != 0)
      return {signal_boundary_stage::thread_release, EBUSY, 0};
   try {
      const auto lock = std::scoped_lock{preparation_mutex};
      auto* slot = current_lease.slot;
      const auto release_handlers = slot->owner.release_handlers;
      slot->ready.store(false, std::memory_order_seq_cst);
      restore_routes(*slot);
      while (slot->readers.load(std::memory_order_seq_cst) != 0)
         std::this_thread::yield(); // Cold retirement only; never execution or admission polling.
      destroy_route_port(*slot);
      drop_routes(slot->previous);
      slot->allocated = false;
      slot->owner = {};
      slot->reserve = {};
      current_lease.slot = nullptr;
      current_lease.receiver = nullptr;
      --process_service->leases;
      stop_empty_service();
      if (current_lease.owned_mapping != MAP_FAILED) {
         if (munmap(current_lease.owned_mapping, current_lease.mapping_bytes) != 0)
            broken_restoration();
         current_lease.owned_mapping = MAP_FAILED;
         current_lease.mapping_bytes = 0;
      }
      if (release_handlers)
         release_handlers();
      return {};
   } catch (...) {
      // A mutex failure happened before ownership mutation; keep the lease and its reserve alive.
      return {signal_boundary_stage::thread_release, EIO, 0};
   }
}

bool mach_recovery::ready() noexcept {
   return !inherited_fork.load(std::memory_order_relaxed) && current_lease.slot != nullptr;
}

bool mach_recovery::persistent_handlers() noexcept {
   return ready() && current_lease.slot->owner.release_handlers != nullptr;
}

bool mach_recovery::matches_reserve(std::span<std::byte> reserve) noexcept {
   return ready() && current_lease.slot->reserve.data() == reserve.data() &&
          current_lease.slot->reserve.size() == reserve.size();
}

bool mach_recovery::fork_child() noexcept {
   return inherited_fork.load(std::memory_order_relaxed);
}

void mach_recovery::begin_invocation() noexcept {
   if (!ready() || current_lease.invocations == std::numeric_limits<unsigned>::max())
      broken_restoration();
   ++current_lease.invocations;
}

void mach_recovery::end_invocation() noexcept {
   if (!ready() || current_lease.invocations == 0)
      broken_restoration();
   --current_lease.invocations;
}

} // namespace forge::vm::wasm::interpret

extern "C" kern_return_t catch_forge_vm_mach_exception_raise(mach_port_t port, mach_port_t thread, mach_port_t task,
                                                             exception_type_t exception, mach_exception_data_t code,
                                                             mach_msg_type_number_t count, audit_token_t sender) {
   return forge::vm::wasm::interpret::handle_exception(port, thread, task, exception, code, count, sender);
}

extern "C" kern_return_t catch_forge_vm_mach_exception_raise_state(mach_port_t, exception_type_t,
                                                                   const mach_exception_data_t, mach_msg_type_number_t,
                                                                   int*, const thread_state_t, mach_msg_type_number_t,
                                                                   thread_state_t, mach_msg_type_number_t*,
                                                                   audit_token_t) {
   return KERN_FAILURE; // This owner installs DEFAULT, never state-only requests.
}

extern "C" kern_return_t catch_forge_vm_mach_exception_raise_state_identity(
    mach_port_t, mach_port_t, mach_port_t, exception_type_t, mach_exception_data_t, mach_msg_type_number_t, int*,
    thread_state_t, mach_msg_type_number_t, thread_state_t, mach_msg_type_number_t*, audit_token_t) {
   return KERN_FAILURE;
}
