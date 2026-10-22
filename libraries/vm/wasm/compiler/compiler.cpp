module;

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <llvm/Config/llvm-config.h>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <wasm_export.h>
#include <wasm.h>
#include <wasm_runtime.h>
#include "details/engine_compiler.hxx"
#include "../interpret/details/engine_limits.hxx"

module forge.vm.wasm.compiler.compiler;

import forge.vm.wasm.interpret.capacity_profile;
import forge.vm.wasm.optimizing.exceptions;

namespace forge::vm::wasm::compiler {

std::string_view compiler::llvm_version() noexcept {
   return LLVM_VERSION_STRING;
}

namespace {

interpret::host_function signature(const WASMFuncType& type) {
   auto result = interpret::host_function{};
   if (type.result_count > 1)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "WAMR profile excludes multi-value signatures");
   const auto numeric = [](std::uint8_t value) {
      if (value != VALUE_TYPE_I32 && value != VALUE_TYPE_I64 && value != VALUE_TYPE_F32 && value != VALUE_TYPE_F64)
         FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "WAMR profile requires numeric signatures");
      return static_cast<interpret::value_type>(value);
   };
   for (std::uint32_t i = 0; i < type.param_count; ++i)
      result.params.push_back(numeric(type.types[i]));
   for (std::uint32_t i = 0; i < type.result_count; ++i)
      result.ret.push_back(numeric(type.types[type.param_count + i]));
   return result;
}

void admit(WASMModuleCommon* raw, optimizing::artifact_payload& payload) {
   const auto limits = interpret::detail::engine_limits{raw};
   limits.check_profile<interpret::capacity_profile>(payload.configuration.execution);
   limits.check_metadata(payload.configuration.execution);
   limits.check_instructions(payload.configuration.execution);
   limits.validate_instantiation();
   if (raw->module_type != Wasm_Module_Bytecode)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "WAMR compiler accepts original WASM only");
   const auto& module = *reinterpret_cast<const WASMModule*>(raw);
   if (module.import_table_count || module.import_memory_count || module.import_global_count)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_import,
                            "WAMR compiler permits numeric function imports only");
   const auto type_index = [&](const WASMFuncType* type) {
      auto value = signature(*type);
      const auto found = std::find(payload.types.begin(), payload.types.end(), value);
      if (found != payload.types.end())
         return static_cast<std::uint32_t>(found - payload.types.begin());
      payload.types.push_back(std::move(value));
      return static_cast<std::uint32_t>(payload.types.size() - 1);
   };
   for (std::uint32_t i = 0; i < module.import_function_count; ++i) {
      const auto& imported = module.import_functions[i].u.function;
      const auto index = type_index(imported.func_type);
      const auto& bindings = payload.configuration.imports;
      const auto key = std::pair{std::string_view(imported.module_name), std::string_view(imported.field_name)};
      const auto found =
          std::lower_bound(bindings.begin(), bindings.end(), key, [](const auto& value, const auto& name) {
             return std::pair{std::string_view(value.module), std::string_view(value.name)} < name;
          });
      if (found == bindings.end() || found->module != key.first || found->name != key.second ||
          found->signature != payload.types[index])
         FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_import, "WAMR compiler import signature is unbound");
      payload.functions.push_back({{}, index, static_cast<std::uint32_t>(found - bindings.begin()), 0});
   }
   for (std::uint32_t i = 0; i < module.function_count; ++i)
      payload.functions.push_back({{}, type_index(module.functions[i]->func_type), UINT32_MAX, 0});
   for (std::uint32_t i = 0; i < module.export_count; ++i) {
      const auto& exported = module.exports[i];
      if (exported.kind == EXPORT_KIND_FUNC)
         payload.exports.push_back({std::string{exported.name, exported.name_len}, exported.index});
   }
   if (module.memory_count) {
      const auto& memory = module.memories[0];
      payload.memory = {
          true, static_cast<std::uint32_t>(std::uint64_t{memory.init_page_count} * memory.num_bytes_per_page / 65536),
          static_cast<std::uint32_t>(std::uint64_t{memory.max_page_count} * memory.num_bytes_per_page / 65536)};
   }
   payload.start = module.start_function;
}

} // namespace

optimizing::artifact compiler::compile(const optimizing::compile_request& request) const {
   auto payload = optimizing::artifact_payload{};
   payload.configuration = optimizing::canonicalize(request.configuration);
   if (!payload.configuration.target.matches_host())
      FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_target,
                            "WAMR compiler requires the exact native profile");
   const auto& limits = payload.configuration.limits;
   if (!limits.max_generated_bytes || limits.max_generated_bytes > UINT32_MAX || !limits.max_function_stack_bytes)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit,
                            "WAMR compiler requires finite explicit byte budgets");
   if (request.wasm.size() > interpret::capacity_profile::max_module_bytes ||
       payload.configuration.execution.max_call_depth > interpret::capacity_profile::max_call_depth)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit, "WASM exceeds the supported capacity profile");
   payload.source = request.wasm;
   payload.wasm = crypto::digest::sha256::hash(std::span{request.wasm});
   try {
      payload.aot = detail::engine_compiler{}.compile(
          request.wasm, interpret::capacity_profile::max_module_bytes,
          static_cast<std::uint32_t>(limits.max_generated_bytes),
          [&](WASMModuleCommon* module) { admit(module, payload); },
          payload.configuration.execution.allow_zero_blocktype);
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const interpret::detail::engine_limits::exceeded&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit, "WASM exceeds its source admission limits");
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit, "WAMR compiler allocation unavailable");
   } catch (const std::length_error&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compile_limit, "WAMR compiler output exceeds its byte budget");
   } catch (const std::invalid_argument&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "WAMR compiler rejected its original source");
   } catch (const std::runtime_error&) {
      FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "WAMR compiler rejected its original source");
   }
   return optimizing::artifact::from_payload(std::move(payload), optimizing::artifact::origin::isolated_compiler);
}

} // namespace forge::vm::wasm::compiler
