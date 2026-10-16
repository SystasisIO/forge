module;

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <new>
#include <span>
#include <string>
#include <tuple>
#include <vector>

module forge.tooling.wasm_compiler.compiler;

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.exceptions;

#include "details/lowering.hxx"
#include "details/object_image.hxx"

namespace forge::tooling::wasm_compiler {
namespace {
namespace interpret = vm::wasm::interpret;
namespace optimizing = vm::wasm::optimizing;

interpret::host_function signature(const interpret::func_type& source) {
   auto result = interpret::host_function{};
   result.params.assign(source.param_types.data(), source.param_types.data() + source.param_types.size());
   if (source.return_count)
      result.ret.push_back(source.return_type);
   return result;
}
} // namespace

optimizing::artifact compiler::compile(const optimizing::compile_request& request) const {
   auto payload = optimizing::artifact_payload{};
   payload.configuration = optimizing::canonicalize(request.configuration);
   payload.wasm = crypto::digest::sha256::hash(std::span{request.wasm});
   if (!payload.configuration.limits.max_generated_bytes || !payload.configuration.limits.max_function_stack_bytes)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit, "LLVM compiler requires explicit output budgets");

   // Exactly the existing exported typed-writer/parser boundary. Finalization
   // and instantiation checks are shared with the interpreter, not a second
   // validation engine or product admission-policy revalidation.
   auto parsed = interpret::module{};
   parsed.allocator.use_default_memory();
   auto debug = interpret::null_debug_info{};
   auto input = request.wasm;
   try {
      interpret::interpreter::parser<std::nullptr_t, interpret::options, interpret::null_debug_info>{
          parsed.allocator, payload.configuration.execution}
          .parse_module(input, parsed, debug);
      parsed.finalize();
      parsed.validate_instantiation();
   } catch (const interpret::exceptions::allocation&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit,
                            "canonical WASM parser exhausted its allocation budget");
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit, "canonical WASM parser allocation is unavailable");
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module,
                            "canonical WASM preparation rejected compiler input");
   }

   if (!parsed.get_functions_total())
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compiler_unavailable, "module has no native entry point");
   // finalize() already owns structural type equivalence. Compact its first
   // canonical representatives only; no second equality/canonicalizer exists.
   auto module_types = std::vector<std::uint32_t>(parsed.types.size());
   for (std::uint32_t i = 0; i < parsed.types.size(); ++i) {
      const auto representative = parsed.type_aliases[i];
      if (representative == i) {
         module_types[i] = static_cast<std::uint32_t>(payload.types.size());
         payload.types.push_back(signature(parsed.types[i]));
      } else
         module_types[i] = module_types[representative];
   }
   payload.functions.reserve(parsed.get_functions_total());
   for (std::uint32_t i = 0; i < parsed.get_functions_total(); ++i) {
      const auto type_index = module_types[parsed.fast_functions[i]];
      auto import_index = UINT32_MAX;
      if (i < parsed.get_imported_functions_size()) {
         const auto& import = parsed.imports[i];
         const auto module = std::string{import.module_str.data(), import.module_str.data() + import.module_str.size()};
         const auto name = std::string{import.field_str.data(), import.field_str.data() + import.field_str.size()};
         const auto& imports = payload.configuration.imports;
         const auto found =
             std::lower_bound(imports.begin(), imports.end(), std::tie(module, name),
                              [](const auto& value, const auto& key) { return std::tie(value.module, value.name) < key; });
         if (found == imports.end() || found->module != module || found->name != name ||
             found->signature != payload.types[type_index])
            FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_import,
                                  "compiler import has no matching canonical binding signature");
         import_index = static_cast<std::uint32_t>(found - imports.begin());
      }
      payload.functions.push_back({{}, type_index, import_index, 0});
   }
   for (std::uint32_t i = 0; i < parsed.exports.size(); ++i) {
      const auto& item = parsed.exports[i];
      if (item.kind == interpret::external_kind::Function)
         payload.exports.push_back(
             {{item.field_str.data(), item.field_str.data() + item.field_str.size()}, item.index});
   }
   for (std::uint32_t i = 0; i < parsed.globals.size(); ++i) {
      const auto& global = parsed.globals[i];
      auto bits = std::uint64_t{0};
      const auto width =
          global.type.content_type == interpret::i32 || global.type.content_type == interpret::f32 ? 4 : 8;
      std::memcpy(&bits, &global.init.value, width);
      payload.globals.push_back({global.type.content_type, global.type.mutability, bits});
   }
   if (parsed.memories.size()) {
      const auto& memory = parsed.memories[0].limits;
      payload.memory = {true, memory.initial,
                        static_cast<std::uint32_t>(memory.flags ? memory.maximum : interpret::max_pages)};
   }
   for (std::uint32_t i = 0; i < parsed.data.size(); ++i) {
      const auto& data = parsed.data[i];
      payload.data.push_back(
          {static_cast<std::uint32_t>(data.offset.value.i32), {data.data.data(), data.data.data() + data.data.size()}});
   }
   if (parsed.tables.size())
      payload.table.assign(parsed.tables[0].table.data(),
                           parsed.tables[0].table.data() + parsed.tables[0].table.size());
   payload.start = parsed.start;

   const auto object = lowering::emit(parsed, payload, module_types);
   object_image::normalize(object, payload);
   return optimizing::artifact::from_payload(std::move(payload));
}

} // namespace forge::tooling::wasm_compiler
