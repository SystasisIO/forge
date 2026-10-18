#include "details/engine_module.hxx"
#include "details/engine_limits.hxx"
#include "../vm_wasm_execution/fixtures.hxx"
#include <iostream>

import forge.vm.wasm.interpret.options;

namespace vm = forge::vm::wasm::interpret;
namespace detail = vm::detail;
using metric = detail::engine_limits::metric;
using fixtures::bytes;

namespace {
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }

detail::engine_limits measure_module(const bytes& module) {
   detail::engine_module loaded(detail::engine_runtime::acquire(), module,
      std::make_shared<detail::engine_bindings>(std::vector<detail::engine_bindings::function>{}));
   return detail::engine_limits(loaded.get());
}

detail::engine_limits measure(bytes body, bytes parameters = {}, bytes locals = {0}, bool memory = false) {
   bytes module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   bytes type{1, 0x60};
   fixtures::size(type, parameters.size());
   type.insert(type.end(), parameters.begin(), parameters.end());
   type.push_back(0);
   fixtures::section(module, 1, type);
   fixtures::section(module, 3, {1, 0});
   if (memory) fixtures::section(module, 5, {1, 0, 1});
   locals.insert(locals.end(), body.begin(), body.end());
   bytes code{1};
   fixtures::size(code, locals.size());
   code.insert(code.end(), locals.begin(), locals.end());
   fixtures::section(module, 10, code);
   return measure_module(module);
}

detail::engine_limits measure_functions(const std::vector<bytes>& bodies, bytes parameters = {}) {
   bytes module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   bytes type{1, 0x60};
   fixtures::size(type, parameters.size());
   type.insert(type.end(), parameters.begin(), parameters.end());
   type.push_back(0);
   fixtures::section(module, 1, type);
   bytes functions;
   fixtures::size(functions, bodies.size());
   functions.insert(functions.end(), bodies.size(), 0);
   fixtures::section(module, 3, functions);
   bytes code;
   fixtures::size(code, bodies.size());
   for (const auto& body : bodies) {
      fixtures::size(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
   }
   fixtures::section(module, 10, code);
   return measure_module(module);
}

template <typename Options>
void rejected(const detail::engine_limits& measured, Options options, metric field, std::uint64_t expected) {
   try { measured.check_instructions(options); }
   catch (const detail::engine_limits::exceeded& error) {
      require(error.field == field && error.actual == expected && error.actual > error.allowed,
         "Wrong instruction-limit accounting");
      return;
   }
   throw std::runtime_error("Instruction limit was ignored");
}

vm::options unlimited() {
   vm::options options{};
   options.max_code_bytes = UINT32_MAX;
   options.max_local_sets = UINT32_MAX;
   options.max_nested_structures = UINT32_MAX;
   options.max_br_table_elements = UINT32_MAX;
   options.max_memory_offset = UINT32_MAX;
   options.max_func_local_bytes = UINT64_MAX;
   return options;
}
}

int main() {
   try {
      const bytes body{0x42, 0, 0x44, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0x1a, 0x1a, 0x0b};
      const auto measured = measure(body, {0x7f, 0x7e}, {2, 1, 0x7f, 1, 0x7e});
      auto limits = unlimited();
      limits.max_code_bytes = body.size() + 5;
      limits.max_local_sets = 2;
      measured.check_instructions(limits);
      auto below = limits;
      --below.max_code_bytes;
      rejected(measured, below, metric::code_bytes, limits.max_code_bytes);
      below = limits; --below.max_local_sets;
      rejected(measured, below, metric::local_sets, 2);
      for (unsigned flags = 0; flags < 8; ++flags) {
         const auto expected = (flags & 1 ? 12U : 0) + (flags & 2 ? 12U : 0) + (flags & 4 ? 16U : 0);
         require(measured.function_bytes(flags) == expected, "Wrong logical params/locals/stack bytes");
         limits.max_func_local_bytes_flags = static_cast<vm::max_func_local_bytes_flags_t>(flags);
         limits.max_func_local_bytes = expected;
         measured.check_instructions(limits);
         if (expected) {
            below = limits; --below.max_func_local_bytes;
            rejected(measured, below, metric::function_bytes, expected);
         }
      }
      bool invalid_flags = false;
      try { (void)measured.function_bytes(8); }
      catch (const std::invalid_argument&) { invalid_flags = true; }
      require(invalid_flags, "Invalid accounting flag was silently ignored");

      const auto zero_locals = measure({0x0b}, {}, {2, 0, 0x7f, 0, 0x7e});
      require(zero_locals.get(metric::local_sets) == 2 && zero_locals.function_bytes(7) == 0,
         "Zero-sized local declarations lost their set count");
      const auto dead = measure({0x00, 0x42, 0, 0x42, 0, 0x1a, 0x1a, 0x0b});
      require(dead.function_bytes(4) == 0, "Unreachable operands inflated logical stack");
      const auto nested_dead = measure({0x00, 0x02, 0x40, 0x42, 0, 0x1a, 0x0b, 0x0b});
      require(nested_dead.function_bytes(4) == 0, "Nested unreachable operands inflated logical stack");
      const auto nested = measure({0x02, 0x40, 0x03, 0x40, 0x0b, 0x0b, 0x0b});
      limits = unlimited(); limits.max_nested_structures = 3;
      nested.check_instructions(limits);
      --limits.max_nested_structures;
      rejected(nested, limits, metric::nested_structures, 3);
      struct controls { std::uint32_t max_control_depth; };
      nested.check_instructions(controls{2});
      rejected(nested, controls{1}, metric::control_depth, 2);
      measure({0x0b}).check_instructions(controls{0});
      const auto branches = measure({0x02, 0x40, 0x41, 0, 0x0e, 2, 0, 0, 0, 0x0b, 0x0b});
      limits = unlimited(); limits.max_br_table_elements = 2;
      branches.check_instructions(limits);
      --limits.max_br_table_elements;
      rejected(branches, limits, metric::br_table_elements, 2);
      const auto offset = measure({0x41, 0, 0x28, 2, 0x81, 1, 0x1a, 0x0b}, {}, {0}, true);
      limits = unlimited(); limits.max_memory_offset = 129;
      offset.check_instructions(limits);
      --limits.max_memory_offset;
      rejected(offset, limits, metric::memory_offset, 129);
      const auto otherwise = measure({0x41, 0, 0x04, 0x40, 0x05, 0x0b, 0x0b});
      struct legacy { std::uint32_t max_legacy_nested_structures; };
      otherwise.check_instructions(controls{1});
      otherwise.check_instructions(legacy{2});
      rejected(otherwise, legacy{1}, metric::legacy_depth, 2);
      const auto carried = measure_functions({{0, 0x0b}, {0, 0x41, 0, 0x04, 0x40, 0x05, 0x0b, 0x0b}});
      carried.check_instructions(legacy{3});
      rejected(carried, legacy{2}, metric::legacy_depth, 3);
      const auto mixed = measure_functions({
         {0, 0x42, 0, 0x42, 0, 0x42, 0, 0x42, 0, 0x42, 0, 0x1a, 0x1a, 0x1a, 0x1a, 0x1a, 0x0b},
         {1, 8, 0x7e, 0x42, 0, 0x1a, 0x0b}}, {0x7e});
      require(mixed.function_bytes(7) == 80 && mixed.function_bytes(6) == 72,
         "Function-byte admission summed unrelated per-function maxima");
      std::cout << "PASS instruction limits, all byte flags, unreachable stack and control/legacy boundaries\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n'; return 1;
   }
}
