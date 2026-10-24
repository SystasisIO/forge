#include "details/engine_module.hxx"
#include "details/engine_instance.hxx"
#include "../../vm_wasm_execution/profile_fixture.hxx"

#include <aot_jit_profile.h>
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <wasm_runtime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <vector>

import forge.vm.wasm.interpret.debug_info;

namespace {
namespace vm = forge::vm::wasm::interpret;
namespace engine = vm::detail;

void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}

vm::profile_instr_map observer(const std::shared_ptr<engine::engine_module>& module) {
   return {std::shared_ptr<const void>{module, module.get()}, &engine::engine_module::translate_jit_pc};
}

struct native_division {
   std::uintptr_t pc = 0;
   std::vector<std::uint8_t> code;
};

std::array<native_division, 2> verify_divisions(const fixtures::profile_fixture& fixture, engine::engine_module& module,
                                                const vm::profile_instr_map& map) {
   const auto* context = reinterpret_cast<const WASMModule*>(module.get())->comp_ctx;
   std::uint32_t count = 0;
   const auto* intervals = aot_jit_profile_intervals(context, &count);
   require(intervals && count, "Instruction map was not published");
   const auto& function = fixture.opcodes[1];
   std::array<std::uint32_t, 2> divisions{};
   unsigned expected = 0;
   auto begin = std::numeric_limits<std::uintptr_t>::max();
   std::uintptr_t end = 0;
   for (const auto offset : function)
      if (fixture.wasm[offset] == 0x6e) {
         require(expected < divisions.size(), "Unexpected fixture division count");
         divisions[expected++] = offset;
      }
   require(expected == 2, "Fixture must retain both dependent unsigned divisions");
   for (const auto& interval : std::span{intervals, count}) {
      if (std::ranges::find(function, interval.wasm_offset) != function.end()) {
         begin = std::min(begin, interval.begin);
         end = std::max(end, interval.end);
      }
   }
   require(begin < end && end - begin < 65536, "Guest function has no bounded native interval span");

   const std::unique_ptr<char, decltype(&LLVMDisposeMessage)> triple{LLVMGetDefaultTargetTriple(), LLVMDisposeMessage};
   const std::unique_ptr<char, decltype(&LLVMDisposeMessage)> cpu{LLVMGetHostCPUName(), LLVMDisposeMessage};
   const std::unique_ptr<char, decltype(&LLVMDisposeMessage)> features{LLVMGetHostCPUFeatures(), LLVMDisposeMessage};
   const std::unique_ptr<void, decltype(&LLVMDisasmDispose)> decoder{
       LLVMCreateDisasmCPUFeatures(triple.get(), cpu.get(), features.get(), nullptr, 0, nullptr, nullptr),
       LLVMDisasmDispose};
   require(decoder != nullptr, "LLVM native disassembler is unavailable");
   std::array<native_division, 2> result;
   unsigned decoded = 0;
   for (auto pc = begin; pc < end;) {
      std::array<char, 256> instruction{};
      const auto size = LLVMDisasmInstruction(decoder.get(), reinterpret_cast<std::uint8_t*>(pc), end - pc, pc,
                                              instruction.data(), instruction.size());
      require(size && size <= end - pc, "Cannot decode generated guest instruction");
      const auto text = std::string_view{instruction.data()};
#if defined(BUILD_TARGET_AARCH64)
      const bool division = text.find("\tudiv\t") != text.npos;
#elif defined(BUILD_TARGET_X86_64)
      const bool division = text.find("\tdivl\t") != text.npos;
#else
#error Unsupported instruction profile regression architecture
#endif
      if (division) {
         require(decoded < divisions.size(), "Unexpected native division count");
         const auto offset = map.translate(reinterpret_cast<const void*>(pc));
         std::cout << "division=" << decoded << " offset=" << offset << " expected=" << divisions[decoded] << '\n';
         require(offset == divisions[decoded], "Native division lost its original WASM instruction offset");
         for (std::size_t byte = 0; byte < size; ++byte)
            require(map.translate(reinterpret_cast<const void*>(pc + byte)) == divisions[decoded],
                    "Native division interval contains a missing or incorrect instruction offset");
         const auto* code = reinterpret_cast<const std::uint8_t*>(pc);
         result[decoded] = {pc, std::vector<std::uint8_t>{code, code + size}};
         ++decoded;
      }
      pc += size;
   }
   require(decoded == 2, "Both real native divisions must be checked");
   unsigned gaps = 0;
   for (std::size_t i = 0; i + 1 < count; ++i)
      if (intervals[i].end < intervals[i + 1].begin) {
         ++gaps;
         require(map.translate(reinterpret_cast<const void*>(intervals[i].end)) == UINT32_MAX,
                 "An unmapped native gap inherited the preceding instruction");
      }
   require(gaps > 0, "Regression fixture must retain unknown native gaps");
   const auto* raw = reinterpret_cast<const WASMModule*>(module.get());
   require(map.translate(raw->func_ptrs[1]) == UINT32_MAX, "Guest wrapper/prologue inherited an instruction offset");
   require(map.translate(reinterpret_cast<const void*>(&std::puts)) == UINT32_MAX, "Host code acquired a guest offset");
   require(map.translate(nullptr) == UINT32_MAX, "Null address acquired a guest offset");
   return result;
}

void trial() {
   fixtures::profile_fixture fixture{true};
   auto stop = std::stop_source{};
   unsigned entries = 0;
   engine::engine_bindings::function entered;
   entered.module = "env";
   entered.name = "entered";
   entered.invoke = [&](engine::engine_bindings::call&) {
      ++entries;
      stop.request_stop();
   };
   std::vector<engine::engine_bindings::function> functions;
   functions.push_back(std::move(entered));
   auto bindings = std::make_shared<engine::engine_bindings>(std::move(functions));
   auto runtime = engine::engine_runtime::acquire();
   auto budget = engine::engine_load_budget::create(64 * 1024 * 1024);
   auto module = std::make_shared<engine::engine_module>(runtime, fixture.wasm, bindings, false, false, budget);
   const auto preparation_start = std::chrono::steady_clock::now();
   module->compile_llvm_jit(true);
   const auto preparation_end = std::chrono::steady_clock::now();
   auto map = observer(module);
   engine::engine_instance context{module, 64 * 1024, {}, nullptr, 1, 251, nullptr, nullptr, nullptr, Mode_LLVM_JIT};
   std::array<std::uint32_t, 1> cells{130};
   require(context.invoke("other", 1, cells) && cells[0] == fixtures::profile_fixture::expected(130, true),
           "Profiled execution returned the wrong value");
   const auto execution_start = std::chrono::steady_clock::now();
   for (unsigned i = 0; i != 5; ++i) {
      cells[0] = 4000000;
      require(context.invoke("other", 1, cells), "Profiled execution failed");
   }
   const auto execution_end = std::chrono::steady_clock::now();
   std::cout << "preparation_us="
             << std::chrono::duration_cast<std::chrono::microseconds>(preparation_end - preparation_start).count()
             << " execution_5x4000000_us="
             << std::chrono::duration_cast<std::chrono::microseconds>(execution_end - execution_start).count() << '\n';
   const auto before = verify_divisions(fixture, *module, map);
   bool canceled = false;
   try {
      context.invoke("cancel", 0, cells, stop.get_token());
   } catch (const engine::engine_instance::canceled&) {
      canceled = true;
   }
   require(canceled && stop.stop_requested() && entries == 1,
           "Guest callback did not cancel the live native invocation");
   cells[0] = 130;
   require(context.invoke("other", 1, cells) && cells[0] == fixtures::profile_fixture::expected(130, true),
           "Native code was not reusable after cancellation");
   const auto after = verify_divisions(fixture, *module, map);
   for (std::size_t i = 0; i < before.size(); ++i)
      require(before[i].pc == after[i].pc && before[i].code == after[i].code,
              "Cancellation replaced or changed the mapped native division code");
   auto foreign = std::make_shared<engine::engine_module>(runtime, fixture.wasm, bindings, false, false,
                                                          engine::engine_load_budget::create(64 * 1024 * 1024));
   foreign->compile_llvm_jit(true);
   auto foreign_map = observer(foreign);
   const auto* raw = reinterpret_cast<const WASMModule*>(module->get());
   std::uint32_t count = 0;
   const auto* intervals = aot_jit_profile_intervals(raw->comp_ctx, &count);
   for (const auto& interval : std::span{intervals, count})
      require(foreign_map.translate(reinterpret_cast<const void*>(interval.begin)) == UINT32_MAX,
              "Independent profile objects share native address attribution");
   auto plain = std::make_shared<engine::engine_module>(runtime, fixture.wasm, bindings, false, false,
                                                        engine::engine_load_budget::create(64 * 1024 * 1024));
   plain->compile_llvm_jit(false);
   const auto* plain_raw = reinterpret_cast<const WASMModule*>(plain->get());
   count = 1;
   require(aot_jit_profile_intervals(plain_raw->comp_ctx, &count) == nullptr && count == 0,
           "Ordinary JIT unexpectedly published profiling intervals");
}
} // namespace

int main() {
   try {
      require(LLVMInitializeNativeTarget() == 0 && LLVMInitializeNativeDisassembler() == 0,
              "LLVM native disassembler initialization failed");
      trial();
      std::cout << "Real native instruction profile and cancellation reuse PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
