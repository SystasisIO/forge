#pragma once

namespace forge::vm::wasm::optimizing {

class native_image {
 public:
   native_image(const artifact_payload&, const interpret::prepared_module::preparation&);
   std::span<const std::byte> code() const noexcept;
   std::span<const native_function> functions() const noexcept;
   std::uint64_t byte_size() const noexcept;
   std::shared_ptr<interpret::prepared_module> runtime() const noexcept;
   void seal();

 private:
   interpret::prepared_module::preparation _allocation;
   std::shared_ptr<interpret::prepared_module> _module;
   std::vector<native_function, interpret::prepared_module::preparation::allocator<native_function>> _functions;
   std::uint64_t _bytes = 0;
};

} // namespace forge::vm::wasm::optimizing
