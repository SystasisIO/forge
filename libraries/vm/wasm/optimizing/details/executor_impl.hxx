#pragma once

namespace forge::vm::wasm::optimizing {

struct executor::impl {
   struct invocation;
   struct thread_state;
   struct bridge;
   explicit impl(asio::task::scheduler&, options);
   static boost::asio::awaitable<void> start(std::shared_ptr<impl>, std::chrono::steady_clock::time_point,
                                             std::stop_token);
   static boost::asio::awaitable<void> shutdown(std::shared_ptr<impl>);
   static void prepare_thread(std::shared_ptr<impl>);
   void release_thread();
   invocation_result invoke(const artifact&, std::uint64_t, std::span<const std::uint32_t>, host_thunk, void*, bool,
                            std::uint32_t, std::span<const interpret::operand_stack_elem>, interpret::wasm_allocator&,
                            std::chrono::steady_clock::time_point, std::stop_token, std::stop_token);
   static std::uint64_t guest_entry(void*) noexcept;
   static std::uint64_t host_entry(void*) noexcept;
   static void host_body(void*);
   static thread_local std::unique_ptr<thread_state> thread;
   options limits;
   executor_epoch epoch;
};

} // namespace forge::vm::wasm::optimizing
