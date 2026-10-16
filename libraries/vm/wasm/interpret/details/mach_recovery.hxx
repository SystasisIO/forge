#pragma once

namespace forge::vm::wasm::interpret {

// Private platform transport. These are addresses of the existing recovery
// owner's TLS cells, not another invocation ABI or copied activation record.
class mach_recovery {
 public:
   struct view {
      std::atomic<sigjmp_buf*>* destination = nullptr;
      std::span<std::byte>* code = nullptr;
      std::span<std::byte>* memory = nullptr;
      std::span<const std::byte>* stack_guard = nullptr;
      bool* strict_native = nullptr;
      std::atomic<std::size_t>* timeout_count = nullptr;
      void (*leave)(int, native_signal_fault) noexcept = nullptr;
      void (*release_handlers)() noexcept = nullptr;
   };

   // Explicit native worker preparation borrows a reserve until release.
   // The compatibility path owns a bounded reserve in the very same TLS lease.
   [[nodiscard]] static signal_boundary_status prepare(view owner, std::span<std::byte> reserve) noexcept;
   [[nodiscard]] static signal_boundary_status prepare_legacy(view owner) noexcept;
   [[nodiscard]] static signal_boundary_status release() noexcept;
   [[nodiscard]] static bool ready() noexcept;
   [[nodiscard]] static bool persistent_handlers() noexcept;
   [[nodiscard]] static bool matches_reserve(std::span<std::byte> reserve) noexcept;
   [[nodiscard]] static bool fork_child() noexcept;

   // Invocation depth is independent of a NULL destination during host C++.
   // Neither operation acquires a process lock or mutates exception ports.
   static void begin_invocation() noexcept;
   static void end_invocation() noexcept;
};

} // namespace forge::vm::wasm::interpret
