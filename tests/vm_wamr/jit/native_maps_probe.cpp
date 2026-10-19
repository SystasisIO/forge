#include "details/engine_module.hxx"
#include "details/engine_instance.hxx"
#include "details/engine_thread_scope.hxx"
#include "../../vm_wasm_execution/fixtures.hxx"

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
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
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

void permissions(const trace& observed, bool thread_protected) {
#if defined(BH_PLATFORM_DARWIN) || defined(__linux__)
   unsigned code = 0, read_only = 0, writable = 0;
   for (const auto& map : observed.mappings) {
      if (!map.live)
         continue;
#if defined(BH_PLATFORM_DARWIN)
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
      const bool read = info.protection & VM_PROT_READ;
      const bool write = info.protection & VM_PROT_WRITE;
      const bool execute = info.protection & VM_PROT_EXECUTE;
#else
      bool read = false, write = false, execute = false, found = false;
      const auto address = reinterpret_cast<std::uintptr_t>(map.address);
      auto maps = std::ifstream{"/proc/self/maps"};
      for (std::string line; std::getline(maps, line);) {
         unsigned long long begin = 0, end = 0;
         char flags[5]{};
         if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, flags) == 3 && address >= begin &&
             address < end && map.size <= end - address) {
            read = flags[0] == 'r';
            write = flags[1] == 'w';
            execute = flags[2] == 'x';
            found = true;
            break;
         }
      }
      require(found, "Native mapping permissions could not be inspected");
#endif
      require(read, "Native code/RO/RW mapping is not readable");
      if (execute) {
         ++code;
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
         if (thread_protected)
            require(os_thread_jit_write_protect_supported(), "MAP_JIT lacks effective thread W^X");
         else
            require(!write, "Ordinary native code retained write permission");
#else
         require(!thread_protected, "MAP_JIT was selected on an unsupported native target");
         require(!write, "Native code VMA is writable and executable");
#endif
      } else if (write)
         ++writable;
      else if (read)
         ++read_only;
   }
   require(code && writable && read_only, "Code/RO/RW permission groups were not preserved");
#else
   (void)observed;
   require(!thread_protected, "MAP_JIT was selected on a non-Darwin target");
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

fixtures::bytes lifecycle_fixture(bool import_only) {
   fixtures::bytes code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(code, 1, {2, 0x60, 0, 0, 0x60, 0, 1, 0x7f});
   fixtures::section(code, 2, {1, 3, 'e', 'n', 'v', 7, 'o', 'b', 's', 'e', 'r', 'v', 'e', 0, 0});
   if (import_only) {
      fixtures::section(code, 7, {1, 7, 'o', 'b', 's', 'e', 'r', 'v', 'e', 0, 0});
      fixtures::section(code, 8, {0});
      return code;
   }
   fixtures::section(code, 3, {4, 0, 1, 1, 1});
   fixtures::section(
       code, 7, {3, 4, 'l', 'e', 'a', 'f', 0, 2, 6, 'a', 'n', 's', 'w', 'e', 'r', 0, 3, 4, 't', 'r', 'a', 'p', 0, 4});
   fixtures::section(code, 8, {1});
   fixtures::section(code, 10,
                     {4, 4, 0, 0x10, 0, 0x0b, 4, 0, 0x41, 42, 0x0b, 6, 0, 0x10, 0, 0x10, 2, 0x0b, 3, 0, 0, 0x0b});
   return code;
}

struct normal_stop {};

void native_lifecycle() {
   require(wasm_runtime_get_native_code_policy(nullptr) == WASM_NATIVE_CODE_UNCOMMITTED,
           "Null native module did not fail closed");
   WASMModuleCommon unknown{};
   unknown.module_type = 255;
   require(wasm_runtime_get_native_code_policy(&unknown) == WASM_NATIVE_CODE_UNCOMMITTED,
           "Unknown module kind did not fail closed");
   auto runtime = vm::engine_runtime::acquire();
   auto owner = vm::engine_load_budget::create(16 * 1024 * 1024);
   struct callback_state {
      unsigned calls = 0;
      bool expected_protection = true;
      vm::engine_instance* nested = nullptr;
      std::stop_source* stop = nullptr;
      std::exception_ptr failure;
   } host;
   vm::engine_bindings::function observer;
   observer.module = "env";
   observer.name = "observe";
   observer.invoke = [&](vm::engine_bindings::call&) {
      ++host.calls;
      require(os_thread_jit_write_protect_state() == host.expected_protection,
              "Native host callback has the wrong thread protection state");
      if (host.nested) {
         std::array<std::uint32_t, 1> nested{};
         require(host.nested->invoke("leaf", 0, nested) && nested[0] == 42,
                 "Nested native invocation did not execute the guest leaf");
         require(os_thread_jit_write_protect_state() == host.expected_protection,
                 "Nested native invocation changed its caller protection");
      }
      if (host.stop)
         host.stop->request_stop();
      if (host.failure)
         std::rethrow_exception(host.failure);
   };
   std::vector<vm::engine_bindings::function> functions;
   functions.push_back(std::move(observer));
   auto bindings = std::make_shared<vm::engine_bindings>(std::move(functions));
   const auto bytes = lifecycle_fixture(false);
   auto source = std::make_shared<vm::engine_module>(runtime, bytes, bindings, false, false, owner);
   require(wasm_runtime_get_native_code_policy(source->get()) == WASM_NATIVE_CODE_UNCOMMITTED,
           "Deferred source published a native policy before compilation");
   host.expected_protection = os_thread_jit_write_protect_state();
   {
      vm::engine_instance interpreted(source, 64 * 1024, {}, nullptr, 1, 8);
      std::array<std::uint32_t, 1> cells{};
      require(interpreted.invoke("answer", 0, cells) && cells[0] == 42 && host.calls == 2,
              "Uncommitted native policy incorrectly blocked interpreter start/call");
   }
   host.calls = 0;
   source->compile_llvm_jit();
   const auto policy = wasm_runtime_get_native_code_policy(source->get());
   const bool thread_protected = policy == WASM_NATIVE_CODE_MAP_JIT;
   require(policy == WASM_NATIVE_CODE_RX || thread_protected, "Defined JIT bodies have no committed protection policy");
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
   require(thread_protected == os_thread_jit_write_protect_supported(), "JIT selected the wrong Darwin mapping mode");
#else
   require(!thread_protected, "MAP_JIT was published on an unsupported target");
#endif
   auto& published_policy = reinterpret_cast<WASMModule*>(source->get())->native_code_policy;
   for (const auto invalid :
        {WASM_NATIVE_CODE_UNCOMMITTED, WASM_NATIVE_CODE_NONE, static_cast<WASMNativeCodePolicy>(255)}) {
      host.calls = 0;
      published_policy = invalid; // Test-only corruption of an otherwise prepared, privately owned module.
      bool refused = false;
      try {
         vm::engine_instance rejected(source, 64 * 1024, {}, nullptr, 1, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT);
      } catch (const vm::engine_instance::unavailable&) {
         refused = true;
      }
      published_policy = policy;
      require(refused && host.calls == 0, "Invalid native policy reached guest initialization");
   }
   const auto initial = os_thread_jit_write_protect_state();
   struct restore_state {
      bool value;
      ~restore_state() {
         os_thread_jit_write_protect_np(value);
      }
   } restore{initial};
   for (const bool previous : {false, true}) {
      os_thread_jit_write_protect_np(previous);
      host.expected_protection = thread_protected || previous;
      host.calls = 0;
      std::exception_ptr normal_exit;
      {
         vm::engine_instance value(source, 64 * 1024, {}, nullptr, 1, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT,
                                   &normal_exit);
         require(host.calls == 1, "Native initialization did not run start exactly once");
         require(os_thread_jit_write_protect_state() == previous,
                 "Native initialization changed its caller protection");
         host.nested = &value;
         std::array<std::uint32_t, 1> cells{};
         for (const auto invalid :
              {WASM_NATIVE_CODE_UNCOMMITTED, WASM_NATIVE_CODE_NONE, static_cast<WASMNativeCodePolicy>(255)}) {
            published_policy = invalid;
            bool refused = false;
            try {
               value.invoke("answer", 0, cells);
            } catch (const vm::engine_instance::unavailable&) {
               refused = true;
            }
            published_policy = policy;
            require(refused && host.calls == 1, "Invalid native policy reached a guest function");
         }
         require(value.invoke("answer", 0, cells) && cells[0] == 42 && host.calls == 2,
                 "Native call reran start or failed to execute the guest");
         require(!value.invoke("trap", 0, cells), "Native unreachable did not trap");
         require(value.invoke("answer", 0, cells) && cells[0] == 42 && host.calls == 3,
                 "Native instance could not be reused after a guest trap");
         normal_exit = std::make_exception_ptr(normal_stop{});
         host.failure = normal_exit;
         bool exited = false;
         try {
            value.invoke("answer", 0, cells);
         } catch (const normal_stop&) {
            exited = value.consume_normal_exit(normal_exit);
         }
         require(exited && host.calls == 4, "Native normal exit lost its exact token or reran start");
         host.failure = {};
         require(value.invoke("answer", 0, cells) && cells[0] == 42 && host.calls == 5,
                 "Native reuse after exact normal exit failed");
         host.nested = nullptr;
      }
      require(os_thread_jit_write_protect_state() == previous, "Native call/error changed its caller protection");
      host.calls = 0;
      normal_exit = std::make_exception_ptr(normal_stop{});
      host.failure = normal_exit;
      {
         vm::engine_instance exited(source, 64 * 1024, {}, nullptr, 1, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT,
                                    &normal_exit);
         require(host.calls == 1, "Native normal start exit reran initialization");
         host.failure = {};
         std::array<std::uint32_t, 1> cells{};
         require(exited.invoke("answer", 0, cells) && cells[0] == 42 && host.calls == 2,
                 "Native normal start exit destroyed the instance or reran start");
      }
      for (const bool real_error : {false, true}) {
         for (const bool start : {false, true}) {
            std::stop_source stop;
            const auto error = real_error ? std::make_exception_ptr(std::runtime_error("native-policy-host-error"))
                                          : std::make_exception_ptr(normal_stop{});
            normal_exit = real_error ? std::exception_ptr{} : error;
            host.calls = 0;
            host.stop = start ? &stop : nullptr;
            host.failure = start ? error : std::exception_ptr{};
            bool refused = false;
            try {
               vm::engine_instance value(source, 64 * 1024, stop.get_token(), nullptr, 1, 8, nullptr, nullptr, nullptr,
                                         Mode_LLVM_JIT, &normal_exit);
               host.stop = &stop;
               host.failure = error;
               std::array<std::uint32_t, 1> cells{};
               value.invoke("answer", 0, cells, stop.get_token());
            } catch (const vm::engine_instance::canceled&) {
               refused = !real_error;
            } catch (...) {
               refused = real_error && std::current_exception() == error;
            }
            require(refused && host.calls == (start ? 1u : 2u),
                    "Native cancel/exit/first host error priority or start count changed");
            require(os_thread_jit_write_protect_state() == previous,
                    "Failed native operation did not restore protection");
            host.stop = nullptr;
            host.failure = {};
            host.calls = 0;
            vm::engine_instance fresh(source, 64 * 1024, {}, nullptr, 1, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT);
            std::array<std::uint32_t, 1> cells{};
            require(fresh.invoke("answer", 0, cells) && cells[0] == 42 && host.calls == 2,
                    "Fresh invocation after native failure was unusable");
         }
      }
      require(wasm_runtime_get_native_code_policy(source->get()) == policy,
              "Native execution changed the published module protection policy");
   }
   host.expected_protection = os_thread_jit_write_protect_state();
   host.calls = 0;
   const auto imports = lifecycle_fixture(true);
   auto import_only = std::make_shared<vm::engine_module>(runtime, imports, bindings, false, false, owner);
   import_only->compile_llvm_jit();
   require(wasm_runtime_get_native_code_policy(import_only->get()) == WASM_NATIVE_CODE_NONE,
           "Import-only JIT module did not publish NONE");
   {
      vm::engine_instance value(import_only, 64 * 1024, {}, nullptr, 1, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT);
      std::array<std::uint32_t, 1> cells{};
      require(host.calls == 1 && value.invoke("observe", 0, cells) && host.calls == 2,
              "Import-only start/call did not retain host-only semantics");
   }
   constexpr std::array<std::uint8_t, 8> empty{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   auto no_body = std::make_shared<vm::engine_module>(runtime, empty, nullptr, false, false, owner);
   no_body->compile_llvm_jit();
   require(wasm_runtime_get_native_code_policy(no_body->get()) == WASM_NATIVE_CODE_NONE,
           "Empty JIT module did not publish NONE");
   {
      vm::engine_instance value(no_body, 64 * 1024, {}, nullptr, 1, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT);
   }
   for (const bool protect_failure : {false, true}) {
      vm::engine_load_budget::faults faults;
      if (protect_failure)
         faults.protection = 1;
      else
         faults.mapping = 1;
      auto refused_owner = vm::engine_load_budget::create(16 * 1024 * 1024, {}, faults);
      auto refused = std::make_shared<vm::engine_module>(runtime, bytes, bindings, false, false, refused_owner);
      const auto before = host.calls;
      bool denied = false;
      try {
         refused->compile_llvm_jit();
      } catch (const vm::engine_load_budget::error& error) {
         denied = error.reason == (protect_failure ? vm::engine_load_budget::failure::protection
                                                   : vm::engine_load_budget::failure::allocation);
      }
      require(denied && !refused->llvm_jit_ready() && host.calls == before &&
                  wasm_runtime_get_native_code_policy(refused->get()) == WASM_NATIVE_CODE_UNCOMMITTED,
              "Failed native protection/mapping published executable module state");
      refused.reset();
      require(refused_owner->current_bytes() == refused_owner->root_bytes(),
              "Failed native publication leaked charged module storage");
   }
   std::thread unload{
       [source = std::move(source), import_only = std::move(import_only), no_body = std::move(no_body)]() mutable {
          no_body.reset();
          import_only.reset();
          source.reset();
       }};
   unload.join();
   require(owner->current_bytes() == owner->root_bytes(), "Native lifecycle retained charged module storage");
   std::cout << "Native policy lifecycle, nested restore, exit/cancel/error, reuse and zero-body policy PASS\n";
}

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
   bool thread_protected = false;
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
   thread_protected = os_thread_jit_write_protect_supported();
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
      const auto policy = aot_get_bounded_jit_native_code_policy(jit.get());
      require(policy != WASM_NATIVE_CODE_RX && policy != WASM_NATIVE_CODE_MAP_JIT,
              "Failed native finalization published executable mapping policy");
      const auto expected = test == scenario::cancelled    ? vm::engine_load_budget::failure::cancelled
                            : test == scenario::protection ? vm::engine_load_budget::failure::protection
                                                           : vm::engine_load_budget::failure::allocation;
      require(budget->status() == expected, "Native denial lost its typed owner reason");
      if (test == scenario::ceiling || test == scenario::aggregate_ceiling || test == scenario::mapping ||
          test == scenario::later_mapping || test == scenario::cancelled)
         require(observed.maps == 0, "Native map occurred after pre-admission denial");
   } else {
      checked(result);
      const auto policy = aot_get_bounded_jit_native_code_policy(jit.get());
      require(policy == (thread_protected ? WASM_NATIVE_CODE_MAP_JIT : WASM_NATIVE_CODE_RX),
              "Committed ORC policy differs from its actual mapping mechanism");
      std::cout << "Native map scenario=" << static_cast<unsigned>(test) << " maps=" << observed.maps << '\n';
      require(address != 0 && observed.maps >= 3, "Code/RO/RW were not separately mapped");
      permissions(observed, thread_protected);
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
      const bool previous = os_thread_jit_write_protect_state();
      if (thread_protected)
         os_thread_jit_write_protect_np(true);
#endif
      const auto answer = reinterpret_cast<std::int32_t (*)()>(static_cast<std::uintptr_t>(address));
      const auto first = answer();
      const auto second = answer();
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
      if (thread_protected)
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
      native_lifecycle();
      std::cout << "Native map ownership, denial, cleanup and recovery PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
