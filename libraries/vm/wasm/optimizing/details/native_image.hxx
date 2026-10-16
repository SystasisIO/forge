#pragma once

// Included after the artifact module declaration/imports. All implementation
// units remain attached to that one owning module, never a sibling's module.
namespace forge::vm::wasm::optimizing {

class native_image {
 public:
   struct layout {
      std::vector<std::uint64_t> offsets;
      std::uint64_t code_bytes = 0;
      std::uint64_t total_bytes = 0;
      std::uint64_t mapping_bytes = 0;
      std::uint64_t allocation_bytes = 0;
   };

   static layout plan(const artifact_payload& payload);
   explicit native_image(const artifact_payload& payload);
   ~native_image() noexcept;
   native_image(const native_image&) = delete;
   native_image& operator=(const native_image&) = delete;

   std::span<const std::byte> code() const noexcept;
   std::span<const native_function> functions() const noexcept;
   std::uint64_t byte_size() const noexcept;

 private:
   void* _mapping = nullptr;
   std::size_t _mapping_bytes = 0;
   std::byte* _base = nullptr;
   std::size_t _code_bytes = 0;
   std::uint64_t _allocation_bytes = 0;
   std::unique_ptr<native_function[]> _functions;
   std::size_t _function_count = 0;
};

} // namespace forge::vm::wasm::optimizing
