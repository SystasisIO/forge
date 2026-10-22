module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <forge/exceptions/macros.hpp>
#include <utility>

module forge.vm.wasm.interpret.debug_info;

import forge.vm.wasm.interpret.exceptions;

namespace forge::vm::wasm::interpret {

profile_instr_map::profile_instr_map(std::shared_ptr<const void> owner, native_translate translate) noexcept
    : _native_owner(std::move(owner)), _native_context(_native_owner.get()),
      _native_translate(_native_context ? translate : nullptr) {}

profile_instr_map::profile_instr_map(profile_instr_map&& source) noexcept {
   *this = std::move(source);
}

profile_instr_map& profile_instr_map::operator=(profile_instr_map&& source) noexcept {
   if (this == &source)
      return *this;
   _native_owner = std::move(source._native_owner);
   _native_context = std::exchange(source._native_context, nullptr);
   _native_translate = std::exchange(source._native_translate, nullptr);
   base_address = std::exchange(source.base_address, nullptr);
   code_size = std::exchange(source.code_size, 0);
   data = std::move(source.data);
   offset_to_addr = data.data();
   offset_to_addr_len = std::exchange(source.offset_to_addr_len, 0);
   source.offset_to_addr = nullptr;
   return *this;
}

void profile_instr_map::builder::on_code_start(const void* compiled_base, const void* wasm_code_start) {
   code_base = compiled_base;
   wasm_base = wasm_code_start;
}

void profile_instr_map::builder::on_function_start(const void* code_addr, const void* wasm_addr) {
   data.push_back({
       static_cast<std::uint32_t>(reinterpret_cast<const char*>(code_addr) - reinterpret_cast<const char*>(code_base)),
       static_cast<std::uint32_t>(reinterpret_cast<const char*>(wasm_addr) - reinterpret_cast<const char*>(wasm_base)),
   });
}

void profile_instr_map::builder::on_instr_start(const void* code_addr, const void* wasm_addr) {
   data.push_back({
       static_cast<std::uint32_t>(reinterpret_cast<const char*>(code_addr) - reinterpret_cast<const char*>(code_base)),
       static_cast<std::uint32_t>(reinterpret_cast<const char*>(wasm_addr) - reinterpret_cast<const char*>(wasm_base)),
   });
}

void profile_instr_map::builder::on_code_end(const void* code_addr, const void*) {
   code_end = code_addr;
}

void profile_instr_map::set(builder&& value) {
   _native_context = nullptr;
   _native_translate = nullptr;
   _native_owner.reset();
   data = std::move(value.data);
   std::sort(data.begin(), data.end(),
             [](const addr_entry& lhs, const addr_entry& rhs) { return lhs.offset < rhs.offset; });
   base_address = value.code_base;
   if (base_address == nullptr || value.code_end == nullptr) {
      code_size = 0;
   } else {
      const auto base = reinterpret_cast<std::uintptr_t>(base_address);
      const auto end = reinterpret_cast<std::uintptr_t>(value.code_end);
      code_size = end >= base ? end - base : 0;
   }
   offset_to_addr = data.data();
   offset_to_addr_len = data.size();
}

void profile_instr_map::relocate(const void* new_base) {
   if (_native_context)
      FORGE_THROW_EXCEPTION(exceptions::profile, "Immutable JIT instruction maps cannot be relocated");
   base_address = new_base;
}

std::uint32_t profile_instr_map::translate(const void* pc) const noexcept {
   if (_native_translate)
      return _native_translate(_native_context, reinterpret_cast<std::uintptr_t>(pc));
   if (offset_to_addr_len == 0) {
      return 0xFFFFFFFFu;
   }

   const auto address = reinterpret_cast<std::uintptr_t>(pc);
   const auto base = reinterpret_cast<std::uintptr_t>(base_address);
   if (address < base) {
      return 0xFFFFFFFFu;
   }

   const auto diff = static_cast<std::size_t>(address - base);
   if (diff >= code_size || diff < offset_to_addr[0].offset) {
      return 0xFFFFFFFFu;
   }

   const auto offset = static_cast<std::uint32_t>(diff);
   std::size_t lower = 0;
   std::size_t upper = offset_to_addr_len;
   while (upper - lower > 1) {
      const auto middle = lower + (upper - lower) / 2;
      if (offset_to_addr[middle].offset <= offset) {
         lower = middle;
      } else {
         upper = middle;
      }
   }
   return offset_to_addr[lower].wasm_addr;
}

} // namespace forge::vm::wasm::interpret
