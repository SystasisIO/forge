module;

#include <cstdint>

module forge.vm.wasm.interpret.types;

namespace forge::vm::wasm::interpret {

void module::validate_instantiation() const {
   detail::check<exceptions::interpreter>((error == nullptr), error);
   const auto validate_data = []<typename Module>(const Module& mod) {
      const auto available_memory =
          mod.memories.size() == 0 ? 0 : mod.memories[0].limits.initial * static_cast<std::uint64_t>(page_size);
      for (std::uint32_t i = 0; i < mod.data.size(); ++i) {
         const auto& segment = mod.data[i];
         const std::uint32_t offset = segment.offset.value.i32;
         const auto required_memory = static_cast<std::uint64_t>(offset) + segment.data.size();
         detail::check<exceptions::memory>((required_memory <= available_memory), "data out of range");
      }
   };
   if (jit_mod)
      validate_data(*jit_mod);
   else
      validate_data(*this);
}

void module::normalize_types() {
   type_aliases.resize(types.size());
   for (std::uint32_t i = 0; i < types.size(); ++i) {
      std::uint32_t canonical = 0;
      for (; canonical < i; ++canonical) {
         if (types[canonical] == types[i]) {
            break;
         }
      }
      type_aliases[i] = canonical;
   }

   const auto imported_functions_size = get_imported_functions_size();
   fast_functions.resize(functions.size() + imported_functions_size);
   for (std::uint32_t i = 0; i < imported_functions_size; ++i) {
      fast_functions[i] = type_aliases[imports[i].type.func_t];
   }
   for (std::uint32_t i = 0; i < functions.size(); ++i) {
      fast_functions[i + imported_functions_size] = type_aliases[functions[i]];
   }
}

} // namespace forge::vm::wasm::interpret
