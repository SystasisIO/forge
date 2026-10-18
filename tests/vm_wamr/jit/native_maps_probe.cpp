#include "details/engine_module.hxx"
#include "details/engine_thread_scope.hxx"

#include <aot_llvm_extra.h>
#include <aot_llvm.h>
#include <aot_orc_extra.h>
#include <wasm_runtime.h>
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#if defined(BH_PLATFORM_DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace vm = forge::vm::wasm::interpret::detail;

namespace {
void require(bool value, const char* text) {
   if (!value)
      throw std::runtime_error(text);
}
void checked(LLVMErrorRef error) {
   if (!error)
      return;
   char* text = LLVMGetErrorMessage(error);
   std::string owned{text};
   LLVMDisposeErrorMessage(text);
   throw std::runtime_error(owned);
}

struct trace {
   struct mapping {
      void* token;
      void* address;
      std::uint64_t size;
      bool live;
   };
   explicit trace(WASMLoadBudget owner) : upstream(owner) {}
   WASMLoadBudget upstream;
   std::stop_source* cancel = nullptr;
   std::size_t maps = 0;
   std::vector<mapping> mappings;
   WASMLoadBudget hooks() {
      return {this,
              [](void* p, std::uint64_t retained, std::uint64_t peak) -> void* {
                 auto& self = *static_cast<trace*>(p);
                 if (self.cancel)
                    self.cancel->request_stop();
                 auto* token = self.upstream.begin_mapping(self.upstream.context, retained, peak);
                 if (token)
                    self.mappings.push_back({token, nullptr, retained, false});
                 return token;
              },
              [](void* p, void* token, void* address) {
                 auto& self = *static_cast<trace*>(p);
                 self.upstream.complete_mapping(self.upstream.context, token, address);
                 if (address) {
                    ++self.maps;
                    for (auto& item : self.mappings)
                       if (item.token == token) {
                          item.address = address;
                          item.live = true;
                       }
                 }
              },
              [](void* p, void* address, std::uint64_t retained) {
                 auto& self = *static_cast<trace*>(p);
                 const bool released = self.upstream.unmap(self.upstream.context, address, retained);
                 if (released)
                    for (auto& item : self.mappings)
                       if (item.address == address)
                          item.live = false;
                 return released;
              },
              [](void* p) {
                 auto& self = *static_cast<trace*>(p);
                 return self.upstream.check(self.upstream.context);
              },
              [](void* p) {
                 auto& self = *static_cast<trace*>(p);
                 return self.upstream.before_protect(self.upstream.context);
              },
              [](void* p) {
                 auto& self = *static_cast<trace*>(p);
                 self.upstream.allocation_failed(self.upstream.context);
              },
              [](void* p) {
                 auto& self = *static_cast<trace*>(p);
                 self.upstream.protection_failed(self.upstream.context);
              }};
   }
};

void permissions(const trace& observed) {
#if defined(BH_PLATFORM_DARWIN)
   unsigned code = 0, read_only = 0, writable = 0;
   for (const auto& map : observed.mappings) {
      if (!map.live)
         continue;
      auto address = static_cast<mach_vm_address_t>(reinterpret_cast<std::uintptr_t>(map.address));
      const auto start = address;
      mach_vm_size_t size{};
      vm_region_basic_info_data_64_t info{};
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object{};
      const auto result = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                         reinterpret_cast<vm_region_info_t>(&info), &count, &object);
      if (object)
         mach_port_deallocate(mach_task_self(), object);
      require(result == KERN_SUCCESS && address <= start && start - address + map.size <= size,
              "Native mapping permissions could not be inspected");
      if (info.protection & VM_PROT_EXECUTE) {
         ++code;
#if defined(BUILD_TARGET_AARCH64)
         require(os_thread_jit_write_protect_supported(), "MAP_JIT lacks effective thread W^X");
#else
         require(!(info.protection & VM_PROT_WRITE), "Native code VMA is writable and executable");
#endif
      } else if (info.protection & VM_PROT_WRITE)
         ++writable;
      else if (info.protection & VM_PROT_READ)
         ++read_only;
   }
   require(code && writable && read_only, "Code/RO/RW permission groups were not preserved");
#else
   (void)observed;
#endif
}

// Exercise the actual recoverable pass-parser path without producing an AOT
// artifact or supplying guessed compiler state to the JIT.
void pass_recovery(LLVMModuleRef fixture) {
   const std::unique_ptr<char, decltype(&LLVMDisposeMessage)> before{LLVMPrintModuleToString(fixture),
                                                                     LLVMDisposeMessage};
   const std::unique_ptr<LLVMOpaqueModule, decltype(&LLVMDisposeModule)> recovery{LLVMCloneModule(fixture),
                                                                                  LLVMDisposeModule};
   require(recovery != nullptr, "Pass recovery module cloning failed");
   const auto module = recovery.get();
   const auto nounwind = LLVMGetEnumAttributeKindForName("nounwind", 8);
   const bool was_nounwind = LLVMGetEnumAttributeAtIndex(LLVMGetNamedFunction(module, "answer"),
                                                         LLVMAttributeFunctionIndex, nounwind) != nullptr;
   const std::unique_ptr<char, decltype(&LLVMDisposeMessage)> triple{LLVMGetDefaultTargetTriple(), LLVMDisposeMessage};
   LLVMTargetRef target{};
   char* error{};
   if (LLVMGetTargetFromTriple(triple.get(), &target, &error)) {
      std::string diagnostic = error ? error : "LLVM target lookup failed";
      if (error)
         LLVMDisposeMessage(error);
      throw std::runtime_error(diagnostic);
   }
   const std::unique_ptr<LLVMOpaqueTargetMachine, decltype(&LLVMDisposeTargetMachine)> machine{
       LLVMCreateTargetMachine(target, triple.get(), "generic", "", LLVMCodeGenLevelAggressive, LLVMRelocPIC,
                               LLVMCodeModelLarge),
       LLVMDisposeTargetMachine};
   require(machine != nullptr, "LLVM target machine creation failed");
   LLVMSetTarget(module, triple.get());
   AOTCompContext context{};
   context.target_machine = machine.get();
   context.opt_level = 3;
   context.disable_llvm_lto = true;
   context.llvm_passes = "forge-probe-not-an-llvm-pass";
   require(!aot_apply_llvm_new_pass_manager(&context, module),
           "Invalid pass pipeline did not return a recoverable failure");
   require(aot_get_last_error() && *aot_get_last_error(), "Pass failure diagnostic is absent");
   context.llvm_passes = "function(sroa)";
   require(aot_apply_llvm_new_pass_manager(&context, module), "Pass manager did not recover after invalid pipeline");
   const std::unique_ptr<char, decltype(&LLVMDisposeMessage)> after{LLVMPrintModuleToString(fixture),
                                                                    LLVMDisposeMessage};
   require(std::string_view{before.get()} == std::string_view{after.get()},
           "Pass recovery mutated the native mapping fixture");
   const bool now_nounwind = LLVMGetEnumAttributeAtIndex(LLVMGetNamedFunction(module, "answer"),
                                                         LLVMAttributeFunctionIndex, nounwind) != nullptr;
   std::cout << "Pass recovery clone nounwind=" << was_nounwind << "->" << now_nounwind
             << "; mapping fixture unchanged\n";
}

enum class scenario {
   success,
   ceiling,
   aggregate_ceiling,
   mapping,
   later_mapping,
   protection,
   cancelled,
   cleanup,
   factory
};

void trial(scenario test) {
   std::stop_source stop;
   std::atomic<bool> hold{test == scenario::cleanup};
   vm::engine_load_budget::faults faults;
   if (test == scenario::mapping)
      faults.mapping = 1;
   if (test == scenario::later_mapping)
      faults.mapping = 2;
   if (test == scenario::protection)
      faults.protection = 1;
   if (test == scenario::cleanup)
      faults.hold_unmapping = &hold;
   auto budget = vm::engine_load_budget::create(4 * 1024 * 1024, stop.get_token(), faults);
   auto runtime = vm::engine_runtime::acquire();
   const vm::engine_thread_scope thread;
   const vm::engine_load_budget::scope allocation{budget};
   // This test isolates the object-layer mapper, not WASM code generation.
   // The empty bytecode module supplies the existing runtime and budget hooks.
   constexpr std::array<std::uint8_t, 8> wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   vm::engine_module source(runtime, wasm, {}, true, false, budget);
   const auto baseline = budget->current_bytes();
   trace observed{source.native_load_budget()};
   if (test == scenario::cancelled)
      observed.cancel = &stop;
   auto hooks = observed.hooks();
   if (test == scenario::factory) {
      LLVMOrcLLLazyJITRef invalid{};
      auto refused = LLVMOrcCreateBoundedLLJIT(&invalid, LLVMOrcCreateLLLazyJITBuilder(), &hooks, 0);
      require(refused != nullptr && invalid == nullptr, "Invalid JIT factory did not return Error/null");
      LLVMConsumeError(refused);
      require(observed.maps == 0 && budget->current_bytes() == baseline,
              "Failed JIT factory retained a mapping or owner charge");
      checked(LLVMOrcDisposeLLLazyJIT(nullptr));
   }
   const auto limit = test == scenario::ceiling             ? 1
                      : test == scenario::aggregate_ceiling ? os_getpagesize()
                                                            : 4 * 1024 * 1024;
   LLVMOrcLLJITRef raw{};
   auto builder = LLVMOrcCreateLLJITBuilder();
   auto configured = aot_configure_bounded_jit_builder(builder, &hooks, limit);
   if (configured) {
      LLVMOrcDisposeLLJITBuilder(builder);
      checked(configured);
   }
   auto result = LLVMOrcCreateLLJIT(&raw, builder); // Consumes builder on both paths.
   const auto dispose = [](LLVMOrcOpaqueLLJIT* jit) { LLVMConsumeError(LLVMOrcDisposeLLJIT(jit)); };
   auto jit = std::unique_ptr<LLVMOrcOpaqueLLJIT, decltype(dispose)>{raw, dispose};
   LLVMOrcExecutorAddress address{};
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
   const bool initial_protection = os_thread_jit_write_protect_state();
   struct restore_protection {
      bool value;
      ~restore_protection() {
         os_thread_jit_write_protect_np(value);
      }
   } restore{initial_protection};
   os_thread_jit_write_protect_np(false);
#endif
   if (!result) {
      constexpr char ir[] = R"(
@immutable = constant i32 41
@counter = global i32 1
define i32 @answer() {
  %constant = load volatile i32, ptr @immutable
  %old = load i32, ptr @counter
  %next = add i32 %old, 1
  store i32 %next, ptr @counter
  %result = add i32 %constant, %old
  ret i32 %result
}
)";
      auto llvm_context = LLVMContextCreate();
      auto context = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(llvm_context);
      auto buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(ir, sizeof(ir) - 1, "native-map-probe");
      LLVMModuleRef module{};
      char* error{};
      const auto parsed = LLVMParseIRInContext(llvm_context, buffer, &module, &error);
      if (parsed) {
         std::string diagnostic = error ? error : "LLVM parse failed";
         if (error)
            LLVMDisposeMessage(error);
         LLVMOrcDisposeThreadSafeContext(context);
         throw std::runtime_error(diagnostic);
      }
      if (test == scenario::factory)
         pass_recovery(module);
      auto safe = LLVMOrcCreateNewThreadSafeModule(module, context);
      LLVMOrcDisposeThreadSafeContext(context);
      result = LLVMOrcLLJITAddLLVMIRModule(jit.get(), LLVMOrcLLJITGetMainJITDylib(jit.get()), safe);
      if (!result)
         result = LLVMOrcLLJITLookup(jit.get(), &address, "answer");
   }
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
   require(!os_thread_jit_write_protect_state(), "Native finalization changed caller thread W^X state");
#endif
   const bool success = test == scenario::success || test == scenario::cleanup || test == scenario::factory;
   if (!success) {
      require(result != nullptr && address == 0, "Native resource denial was not recoverable");
      LLVMConsumeError(result);
      const auto expected = test == scenario::cancelled    ? vm::engine_load_budget::failure::cancelled
                            : test == scenario::protection ? vm::engine_load_budget::failure::protection
                                                           : vm::engine_load_budget::failure::allocation;
      require(budget->status() == expected, "Native denial lost its typed owner reason");
      if (test == scenario::ceiling || test == scenario::aggregate_ceiling || test == scenario::mapping ||
          test == scenario::later_mapping || test == scenario::cancelled)
         require(observed.maps == 0, "Native map occurred after pre-admission denial");
   } else {
      checked(result);
      std::cout << "Native map scenario=" << static_cast<unsigned>(test) << " maps=" << observed.maps << '\n';
      require(address != 0 && observed.maps >= 3, "Code/RO/RW were not separately mapped");
      permissions(observed);
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
      const bool previous = os_thread_jit_write_protect_state();
      os_thread_jit_write_protect_np(true);
#endif
      const auto answer = reinterpret_cast<std::int32_t (*)()>(static_cast<std::uintptr_t>(address));
      const auto first = answer();
      const auto second = answer();
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
      os_thread_jit_write_protect_np(previous);
#endif
      require(first == 42 && second == 43, "Native code/RO/RW execution is incorrect");
   }
   jit.reset(); // Must synchronously destroy the mapper before its callback context.
   if (test == scenario::cleanup) {
      require(budget->status() == vm::engine_load_budget::failure::cleanup,
              "Failed physical unmapping lost its typed reason");
      require(budget->current_bytes() > baseline && !budget->retry_cleanup(),
              "Failed physical mappings were refunded or forgotten");
      hold = false;
      require(budget->retry_cleanup(), "Physical cleanup did not recover after fault release");
   }
   require(budget->current_bytes() == baseline, "JIT maps or bridge allocations leaked their owner charge");
}
} // namespace

int main() {
   try {
      const auto runtime = vm::engine_runtime::acquire();
      for (const auto test :
           {scenario::success, scenario::ceiling, scenario::aggregate_ceiling, scenario::mapping,
            scenario::later_mapping, scenario::protection, scenario::cancelled, scenario::cleanup, scenario::factory}) {
         trial(test);
         trial(scenario::success);
      }
      std::cout << "Native map ownership, denial, cleanup and recovery PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
