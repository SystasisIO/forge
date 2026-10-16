#pragma once

namespace forge::vm::wasm::optimizing {

struct executor::impl {
   struct invocation;
   struct thread_state;
   struct bridge;

   explicit impl(asio::task::scheduler& scheduler, options limits);
   static boost::asio::awaitable<void> start(std::shared_ptr<impl> owner,
                                             std::chrono::steady_clock::time_point deadline, std::stop_token stop);
   static boost::asio::awaitable<void> shutdown(std::shared_ptr<impl> owner);
   static void prepare_thread(std::shared_ptr<impl> owner);
   void release_thread();
   invocation_result invoke(const artifact& code, std::uint64_t max_frame_bytes, std::span<const std::uint32_t> imports,
                            host_thunk thunk, void* host, bool requires_host, std::uint32_t function_index,
                            std::span<const interpret::operand_stack_elem> arguments, interpret::wasm_allocator& memory,
                            std::chrono::steady_clock::time_point deadline, std::stop_token caller_stop,
                            std::stop_token compute_stop);

   static std::uint64_t host_entry(void* state) noexcept;
   static void host_body(void* state) noexcept;
   static trap_reason host_call(invocation_abi*, std::uint32_t, const interpret::expr_value*,
                                interpret::expr_value*) noexcept;
   static std::int32_t grow_memory(invocation_abi*, std::uint32_t) noexcept;
   static trap_reason numeric(invocation_abi*, std::uint32_t, std::uint32_t, const interpret::expr_value*,
                              interpret::expr_value*) noexcept;
   static trap_reason poll(invocation_abi*) noexcept;
   [[noreturn]] static void trap(invocation_abi*, trap_reason) noexcept;
   static void bounce(bridge& request) noexcept;
   static interpret::expr_value raw(const interpret::operand_stack_elem& value, interpret::value_type type);
   static interpret::operand_stack_elem operand(interpret::expr_value value, interpret::value_type type);

   options limits;
   executor_epoch epoch;
   static thread_local std::unique_ptr<thread_state> thread;
   static const invocation_callbacks callbacks;
};

} // namespace forge::vm::wasm::optimizing
