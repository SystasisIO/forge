module;

#include <cstdint>

export module forge.vm.wasm.interpret.capacity_profile;

export namespace forge::vm::wasm::interpret {

// Common admission envelope, independent of a caller's smaller logical budget.
struct capacity_profile {
   static constexpr std::uint32_t version = 1;
   static constexpr std::uint32_t max_module_bytes = 16 * 1024 * 1024;
   static constexpr std::uint32_t max_function_storage_bytes = 32 * 1024;
   static constexpr std::uint32_t max_function_results = 1;
   static constexpr std::uint32_t max_call_depth = 1024;
   static constexpr std::uint32_t max_control_depth = 1024;
   static constexpr std::uint32_t max_pages = UINT32_MAX / 65536;
   static constexpr std::uint32_t max_instance_metadata_bytes = INT32_MAX;
};

} // namespace forge::vm::wasm::interpret
