#pragma once

namespace forge::vm::wasm::optimizing {

// One fixed cold mapping through the existing allocator. No generic allocator,
// dynamic stack cache or executable-memory ownership is introduced here.
class native_stack {
 public:
   struct bounds {
      std::uintptr_t low;
      std::uintptr_t high;
   };
   explicit native_stack(std::uint64_t usable_bytes);
   ~native_stack() = default;
   native_stack(const native_stack&) = delete;
   native_stack& operator=(const native_stack&) = delete;
   [[nodiscard]] bounds usable() const noexcept;
   [[nodiscard]] std::span<const std::byte> guard() const noexcept;
   [[nodiscard]] static bounds current_thread_bounds();
   [[nodiscard]] static std::uintptr_t current_pointer() noexcept;

   // The SINGLE normal-call primitive serves guest entry and host bounce. It
   // preserves the ordinary ABI; no jump-buffer, Mach state or private OS API.
   using entry = std::uint64_t (*)(void*) noexcept;
   [[nodiscard]] static std::uint64_t call(void* aligned_top, entry callback, void* state,
                                           std::uintptr_t* saved_pointer) noexcept;
   // ABI primitive reserves a return anchor and alignment space, not guest
   // function budget. x86 red zone is excluded when selecting a host top.
   static constexpr std::uint64_t anchor_bytes = 32;
   static constexpr std::uint64_t host_red_zone = 128;
   // Local physical-frame cap, independent of the logical source profile.
   // The page-rounded guard does not replace failing-frame room.
   static constexpr std::uint64_t max_frame_bytes = 64 * 1024;

 private:
   static std::size_t mapping_bytes(std::uint64_t usable_bytes);
   interpret::contiguous_allocator _storage;
   std::byte* _base = nullptr;
   std::size_t _page = 0;
   std::size_t _guard_bytes = 0;
   std::size_t _bytes = 0;
};

} // namespace forge::vm::wasm::optimizing
