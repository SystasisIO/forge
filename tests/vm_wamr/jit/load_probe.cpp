#include "../../vm_wasm_execution/fixtures.hxx"
#include "details/engine_memory.hxx"
#include "details/engine_instance.hxx"
#include "details/engine_limits.hxx"
#include "details/engine_runtime.hxx"
#include "details/engine_thread_scope.hxx"

#include <wasm_exec_env.h>
#include <wasm_loader.h>
#include <wasm_runtime.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

import forge.vm.wasm.interpret.options;
import forge.vm.wasm.interpret.capacity_profile;
import forge.vm.wasm.interpret.prepared_module;
import forge.vm.wasm.interpret.exceptions;

namespace vm = forge::vm::wasm::interpret::detail;
namespace api = forge::vm::wasm::interpret;

namespace {
constexpr std::uint32_t function_count = 2 * WASM_ORC_JIT_BACKEND_THREAD_NUM * WASM_ORC_JIT_COMPILE_THREAD_NUM + 3;
static_assert(function_count >= 4 && function_count < 65536);

void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}

fixtures::bytes fixture() {
   fixtures::bytes result{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(result, 1, {3, 0x60, 0, 1, 0x7f, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 2, 0x7f, 0x7f, 1, 0x7f});
   fixtures::bytes functions;
   fixtures::size(functions, function_count);
   functions.insert(functions.end(), {0, 1, 0, 2});
   functions.resize(functions.size() + function_count - 4, 0);
   fixtures::section(result, 3, functions);
   fixtures::bytes exports;
   fixtures::size(exports, function_count);
   const std::array names{"answer", "recurse", "trap", "add32"};
   for (std::uint8_t index = 0; index != names.size(); ++index) {
      fixtures::text(exports, names[index]);
      exports.insert(exports.end(), {0, index});
   }
   for (std::uint32_t index = names.size(); index != function_count; ++index) {
      fixtures::text(exports, "materialized_" + std::to_string(index));
      exports.push_back(0);
      fixtures::size(exports, index);
   }
   fixtures::section(result, 7, exports);
   fixtures::bytes code;
   fixtures::size(code, function_count);
   const auto append = [&](const fixtures::bytes& body) {
      fixtures::size(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
   };
   append({0, 0x41, 42, 0x0b});
   // n is live across the recursive call, so this is not tail recursion.
   append({0, 0x20, 0, 0x45, 0x04, 0x7f, 0x41, 42, 0x05, 0x20, 0, 0x20, 0, 0x41, 1, 0x6b, 0x10, 1, 0x6a, 0x0b, 0x0b});
   append({0, 0x00, 0x0b});
   append({0, 0x20, 0, 0xbe, 0x20, 1, 0xbe, 0x92, 0xbc, 0x0b});
   // Cross upstream group and partition boundaries, including a partial tail.
   for (std::uint32_t index = names.size(); index != function_count; ++index)
      append({0, 0x41, static_cast<std::uint8_t>(index % 32), 0x0b});
   fixtures::section(result, 10, code);
   return result;
}

class loaded_module {
 public:
   explicit loaded_module(bool defer) : bytes(fixture()) {
      const vm::engine_load_budget::scope allocation{_budget};
      LoadArgs args{};
      args.block_type_mode = WASM_BLOCK_TYPES_NUMERIC;
      args.defer_jit = defer;
      std::array<char, 512> error{};
      _module = wasm_runtime_load_ex(bytes.data(), static_cast<std::uint32_t>(bytes.size()), &args, error.data(),
                                     error.size());
      if (!_module)
         throw std::runtime_error(std::string("JIT fixture load failed: ") + error.data());
      require(_module->module_type == Wasm_Module_Bytecode, "JIT test loaded an AOT artifact");
   }
   ~loaded_module() {
      const vm::engine_load_budget::scope allocation{_budget};
      wasm_runtime_unload(_module);
      if (!_budget->retry_cleanup())
         _runtime->retain_cleanup(_budget);
   }
   loaded_module(const loaded_module&) = delete;
   loaded_module& operator=(const loaded_module&) = delete;
   WASMModule* get() const {
      return reinterpret_cast<WASMModule*>(_module);
   }
   WASMLoadBudget native_budget() const {
      using owner_type = const std::shared_ptr<vm::engine_load_budget>;
      return {
          const_cast<std::shared_ptr<vm::engine_load_budget>*>(&_budget),
          [](void* p, std::uint64_t retained, std::uint64_t peak) {
             return vm::engine_load_budget::begin_mapping(*static_cast<owner_type*>(p), retained, peak,
                                                          &os_munmap_checked);
          },
          [](void* p, void* token, void* address) { (*static_cast<owner_type*>(p))->complete_mapping(token, address); },
          [](void* p, void* address, std::uint64_t retained) {
             return (*static_cast<owner_type*>(p))->unmap(address, retained);
          },
          [](void* p) { return (*static_cast<owner_type*>(p))->active(); },
          [](void* p) { return (*static_cast<owner_type*>(p))->allow_protection(); },
          [](void* p) { (*static_cast<owner_type*>(p))->fail(vm::engine_load_budget::failure::allocation); },
          [](void* p) { (*static_cast<owner_type*>(p))->fail(vm::engine_load_budget::failure::protection); }};
   }
   bool compile(WASMModule* module, char* error, std::uint32_t size) const {
#if defined(WASM_LOADER_HAS_BOUNDED_LLVM_JIT)
      const vm::engine_load_budget::scope allocation{_budget};
      const auto hooks = native_budget();
      return wasm_loader_compile_llvm_jit_ex(module, &hooks, _budget->limit_bytes(), error, size);
#else
      return wasm_loader_compile_llvm_jit(module, error, size);
#endif
   }
   fixtures::bytes bytes;

 private:
   wasm_module_t _module{};
   std::shared_ptr<vm::engine_runtime> _runtime = vm::engine_runtime::acquire();
   std::shared_ptr<vm::engine_load_budget> _budget = vm::engine_load_budget::create(4 * 1024 * 1024);
};

void uncompiled(const loaded_module& source) {
   const auto* module = source.get();
   require(module->function_count == function_count, "Deferred load lost bytecode metadata");
#if WASM_ENABLE_JIT != 0
   require(!module->comp_ctx && !module->comp_data && !module->func_ptrs && !module->func_ptrs_compiled,
           "Deferred load created LLVM compiler state");
#endif
#if WASM_ENABLE_FAST_JIT != 0
   require(!module->fast_jit_func_ptrs, "Deferred load initialized fast JIT");
#endif
#if WASM_ENABLE_JIT != 0 || WASM_ENABLE_FAST_JIT != 0
   for (const auto thread : module->orcjit_threads)
      require(!thread, "Deferred load created a JIT worker");
#endif
}

class instance {
 public:
   instance(const loaded_module& source, RunningMode mode) {
      const vm::engine_memory::scope scope(_memory);
      InstantiationArgs2* raw{};
      require(wasm_runtime_instantiation_args_create(&raw), "Cannot create instance arguments");
      const std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)> args(
          raw, wasm_runtime_instantiation_args_destroy);
      wasm_runtime_instantiation_args_set_default_stack_size(args.get(), 1024 * 1024);
      wasm_runtime_instantiation_args_set_defer_start(args.get(), true);
      std::array<char, 512> error{};
      _instance = wasm_runtime_instantiate_ex2(reinterpret_cast<wasm_module_t>(source.get()), args.get(), error.data(),
                                               error.size());
      if (!_instance)
         throw std::runtime_error(std::string("JIT fixture instantiation failed: ") + error.data());
      const auto cleanup = [this](void*) { destroy(); };
      std::unique_ptr<void, decltype(cleanup)> guard(this, cleanup);
      _memory.bind_instance(_instance);
      require(wasm_runtime_set_running_mode(_instance, mode), "Requested execution mode was rejected");
      require(wasm_runtime_get_running_mode(_instance) == mode, "Engine substituted another execution mode");
      _env = wasm_runtime_create_exec_env(_instance, 1024 * 1024);
      require(_env != nullptr, "Cannot create execution environment");
      _env->forge_max_call_depth = 8;
      require(wasm_runtime_initialize_with_exec_env(_instance, _env), "Fixture initialization failed");
      guard.release();
   }
   ~instance() {
      destroy();
   }
   instance(const instance&) = delete;
   instance& operator=(const instance&) = delete;
   bool call(const char* name, std::uint32_t argc, std::array<std::uint32_t, 2>& cells) {
      const vm::engine_memory::scope scope(_memory);
      wasm_runtime_clear_exception(_instance);
      _env->forge_depth_exceeded = 0;
      auto* function = wasm_runtime_lookup_function(_instance, name);
      require(function != nullptr, "JIT fixture export is missing");
      const bool success = wasm_runtime_call_wasm(_env, function, argc, cells.data());
      require(_env->forge_call_depth == 0, "JIT call leaked logical depth");
      return success;
   }
   wasm_exec_env_t env() const {
      return _env;
   }
   wasm_module_inst_t get() const {
      return _instance;
   }

 private:
   void destroy() {
      const vm::engine_memory::scope scope(_memory);
      if (_env)
         wasm_runtime_destroy_exec_env(_env);
      if (_instance)
         wasm_runtime_deinstantiate(_instance);
      _memory.bind_instance(nullptr);
   }
   vm::engine_memory _memory = vm::engine_memory::system();
   wasm_module_inst_t _instance{};
   wasm_exec_env_t _env{};
};

void execution(const loaded_module& source, RunningMode mode) {
   instance value(source, mode);
   std::array<std::uint32_t, 2> cells{};
   require(value.call("answer", 0, cells) && cells[0] == 42, "Control execution failed");
   cells[0] = 7;
   require(value.call("recurse", 1, cells) && cells[0] == 70, "Admitted recursive execution failed");
   cells[0] = 8;
   require(!value.call("recurse", 1, cells) && value.env()->forge_depth_exceeded,
           "Logical depth was not enforced by the selected execution mode");
   const auto depth = wasm_runtime_get_exception_info(value.get());
   if (mode == Mode_Interp)
      require(depth.status == WASM_EXCEPTION_UNKNOWN,
              "Interpreter logical-depth message was falsely classified as a native stack trap");
   else
      require(depth.status == WASM_EXCEPTION_KNOWN && depth.id == EXCE_NATIVE_STACK_OVERFLOW,
              "Generated logical-depth trap lost its structured exception ID");
   require(value.call("answer", 0, cells) && cells[0] == 42, "Reuse after depth failure failed");
   require(!value.call("trap", 0, cells), "Unreachable did not trap");
   const auto trap = wasm_runtime_get_exception_info(value.get());
   require(trap.status == WASM_EXCEPTION_KNOWN && trap.id == EXCE_UNREACHABLE,
           "JIT trap lost its structured exception ID");
   cells = {0xffc12345u, 0x3f800000u};
   require(value.call("add32", 2, cells) && cells[0] == 0x7fc00000u, "Scalar NaN was not canonicalized");
   cells = {0x80000000u, 0x80000000u};
   require(value.call("add32", 2, cells) && cells[0] == 0x80000000u, "Scalar negative zero was not preserved");
   for (std::uint32_t index = 4; index != function_count; ++index) {
      const auto name = "materialized_" + std::to_string(index);
      require(value.call(name.c_str(), 0, cells) && cells[0] == index % 32,
              "A function beyond the first materialization group failed");
   }
}

void deferred() {
   {
      const auto code = fixture();
      api::prepared_module source(code, api::options{});
      api::prepared_module foreign(code, api::options{});
      api::runtime_context inferred(source, 8);
      api::runtime_context explicit_interpreter(source, 8, api::execution_mode::interpreter);
      static_assert(noexcept(inferred.matches(source, api::execution_mode::interpreter)));
      require(inferred.matches(source, api::execution_mode::interpreter) &&
                  explicit_interpreter.matches(source, api::execution_mode::interpreter),
              "Uncompiled bytecode context did not select interpreter mode");
      require(!inferred.matches(foreign, api::execution_mode::interpreter) &&
                  !inferred.matches(source, api::execution_mode::jit),
              "Context identity or fixed-mode mismatch was accepted");
      bool refused = false;
      try {
         api::runtime_context unprepared(source, 8, api::execution_mode::jit);
      } catch (const api::exceptions::execution_unavailable&) {
         refused = true;
      }
      require(refused, "Unprepared explicit JIT context was accepted");
   }
   for (unsigned repeat = 0; repeat != 16; ++repeat) {
      loaded_module source(true);
      uncompiled(source);
      execution(source, Mode_Interp);
      instance guarded(source, Mode_Interp);
      for (const auto native : {Mode_Fast_JIT, Mode_LLVM_JIT, Mode_Multi_Tier_JIT}) {
         require(!wasm_runtime_set_running_mode(guarded.get(), native), "Unprepared native mode was accepted");
         require(wasm_runtime_get_running_mode(guarded.get()) == Mode_Interp,
                 "Native refusal changed the current mode");
      }
      std::array<std::uint32_t, 2> cells{};
      require(guarded.call("answer", 0, cells) && cells[0] == 42,
              "Interpreter could not recover after native-mode refusal");
      uncompiled(source);
   }
   auto invalid = fixture();
   invalid.back() = 0x00; // No terminating end opcode for the final function.
   LoadArgs args{};
   args.block_type_mode = WASM_BLOCK_TYPES_NUMERIC;
   args.defer_jit = true;
   std::array<char, 512> error{};
   auto* rejected = wasm_runtime_load_ex(invalid.data(), static_cast<std::uint32_t>(invalid.size()), &args,
                                         error.data(), error.size());
   if (rejected) {
      wasm_runtime_unload(rejected);
      throw std::runtime_error("Deferred load accepted malformed bytecode");
   }
   require(error[0] != 0, "Failed deferred load did not provide a diagnostic");
   loaded_module recovery(true);
   uncompiled(recovery);
   execution(recovery, Mode_Interp);
}

void legacy() {
   loaded_module source(false);
#if WASM_ENABLE_JIT != 0
   require(source.get()->comp_ctx && source.get()->comp_data && source.get()->func_ptrs,
           "Default load no longer preserves upstream automatic LLVM preparation");
#if WASM_ENABLE_LAZY_JIT == 0
   for (std::uint32_t index = 0; index != source.get()->function_count; ++index)
      require(source.get()->func_ptrs_compiled[index], "Default eager JIT load did not materialize a function");
#endif
   const auto* context = source.get()->comp_ctx;
   std::array<char, 512> error{};
   require(!source.compile(source.get(), error.data(), error.size()),
           "Explicit preparation accepted an upstream compiler context");
   require(context == source.get()->comp_ctx, "Refusal destroyed the upstream compiler context");
#endif
   execution(source, Mode_Interp);
}

void compile() {
   loaded_module source(true);
   uncompiled(source);
   std::array<char, 512> error{};
   require(!source.compile(nullptr, error.data(), error.size()), "Null module was accepted");
   error.fill(0);
   const bool compiled = source.compile(source.get(), error.data(), error.size());
#if WASM_ENABLE_JIT == 0 || WASM_ENABLE_FAST_JIT != 0 || WASM_ENABLE_LAZY_JIT != 0
   require(!compiled, "Unsupported JIT profile was accepted");
   uncompiled(source);
#else
   if (!compiled)
      throw std::runtime_error(std::string("Explicit LLVM JIT compilation failed: ") + error.data());
   require(source.get()->comp_ctx && source.get()->comp_data && source.get()->func_ptrs,
           "Explicit JIT compilation did not create native state");
   for (std::uint32_t index = 0; index != source.get()->function_count; ++index)
      require(source.get()->func_ptrs[index] && source.get()->func_ptrs_compiled[index],
              "Explicit JIT compilation left an unmaterialized function");
   for (const auto thread : source.get()->orcjit_threads)
      require(!thread, "Explicit JIT compilation created an upstream loader worker");
   const auto* context = source.get()->comp_ctx;
   require(!source.compile(source.get(), error.data(), error.size()),
           "Repeated preparation accepted existing compiler state");
   require(context == source.get()->comp_ctx, "Repeated preparation compiled the module twice");
   execution(source, Mode_LLVM_JIT);
   execution(source, Mode_Interp);
#endif
}

void bridge() {
#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT == 0 && WASM_ENABLE_LAZY_JIT == 0
   const auto* compiler = wasm_runtime_get_llvm_jit_options();
   require(compiler->opt_level == 3 && compiler->size_level == 3 && !compiler->segue_flags &&
               !compiler->quick_invoke_c_api_import,
           "Shared runtime JIT options differ from the qualified compile profile");
   const auto default_mode = wasm_runtime_get_default_running_mode();
   require(default_mode == Mode_Interp, "Shared runtime default must not select JIT");
   // This ceiling covers WAMR-owned storage and native maps. LLVM compiler
   // heap is explicitly excluded from this preparation contract.
   auto budget = vm::engine_load_budget::create(4 * 1024 * 1024);
   const auto bytes = fixture();
   auto source = std::make_shared<vm::engine_module>(
       vm::engine_runtime::acquire(), bytes, std::shared_ptr<const vm::engine_bindings>{}, false, false, budget);
   auto* original = source->get();
   require(!source->llvm_jit_ready(), "Source load implicitly selected native compilation");
   forge::vm::wasm::interpret::options policy{};
   vm::engine_limits admission(source->get());
   admission.check_profile<forge::vm::wasm::interpret::capacity_profile>(policy);
   admission.check_metadata(policy);
   admission.check_instructions(policy);
   admission.validate_instantiation();
   const auto before = budget->current_bytes();
   bool denied = false;
   try {
      vm::engine_instance unprepared(source, 1024 * 1024, {}, nullptr, 8, 8, nullptr, nullptr, nullptr, Mode_LLVM_JIT);
   } catch (const vm::engine_instance::unavailable&) {
      denied = true;
   }
   require(denied && budget->current_bytes() == before,
           "Unprepared JIT mode was not rejected before instance allocation");
   source->compile_llvm_jit();
   require(source->get() == original && source->llvm_jit_ready(),
           "Private JIT preparation replaced the admitted source module");
   require(source->jit_native_stack_bytes().size() == function_count,
           "Private JIT preparation did not retain complete stack metadata");
   source->compile_llvm_jit();
   require(source->get() == original, "Private bridge reloaded source during repeated preparation");
   for (const auto mode : {Mode_LLVM_JIT, Mode_Interp}) {
      vm::engine_instance value(source, admission.interpreter_stack_bytes(8), {}, nullptr, 8, 8, nullptr, nullptr,
                                nullptr, mode);
      require(wasm_runtime_get_running_mode(value.get()) == mode, "Private bridge substituted an execution mode");
      std::array<std::uint32_t, 2> cells{};
      require(value.invoke("answer", 0, cells) && cells[0] == 42, "Private bridge control call failed");
      cells[0] = 7;
      require(value.invoke("recurse", 1, cells) && cells[0] == 70, "Private bridge admitted recursion failed");
      cells[0] = 8;
      bool depth = false;
      try {
         value.invoke("recurse", 1, cells);
      } catch (const vm::engine_instance::depth_exceeded&) {
         depth = true;
      }
      require(depth && value.env()->forge_call_depth == 0, "Private bridge lost logical depth or unwind");
      require(value.invoke("answer", 0, cells) && cells[0] == 42, "Private bridge reuse after depth failure failed");
      require(!value.invoke("trap", 0, cells), "Private bridge unreachable did not trap");
      const auto trap = wasm_runtime_get_exception_info(value.get());
      require(trap.status == WASM_EXCEPTION_KNOWN && trap.id == EXCE_UNREACHABLE,
              "Private bridge lost the structured guest trap");
      cells = {0xffc12345u, 0x3f800000u};
      require(value.invoke("add32", 2, cells) && cells[0] == 0x7fc00000u,
              "Private bridge scalar NaN was not canonicalized");
      for (std::uint32_t index = 4; index != function_count; ++index) {
         const auto name = "materialized_" + std::to_string(index);
         require(value.invoke(name.c_str(), 0, cells) && cells[0] == index % 32,
                 "Private bridge did not execute every materialization group");
      }
   }
   require(wasm_runtime_get_default_running_mode() == default_mode,
           "Private JIT bridge changed process-wide execution mode");
   budget->check();
   {
      api::prepared_module public_source(bytes, api::options{}, std::vector<api::host_binding>{},
                                         api::prepared_module::purpose::execution, api::execution_mode::jit);
      api::runtime_context inferred(public_source, 8);
      api::runtime_context explicit_interpreter(public_source, 8, api::execution_mode::interpreter);
      require(inferred.matches(public_source, api::execution_mode::jit) &&
                  !inferred.matches(public_source, api::execution_mode::interpreter),
              "External default context silently selected interpreter for prepared JIT");
      require(explicit_interpreter.matches(public_source, api::execution_mode::interpreter),
              "Explicit interpreter mode did not remain selectable");
      inferred.initialize(nullptr);
      const auto result = inferred.call(nullptr, "answer", {});
      require(result && result->to_ui32() == 42, "Inferred public JIT context did not execute");
      inferred.reset();
      require(inferred.matches(public_source, api::execution_mode::jit),
              "Reset changed the context's fixed execution mode");
   }
#else
   throw std::runtime_error("Positive private JIT qualification requires the explicit LLVM-only eager profile");
#endif
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 2)
         throw std::invalid_argument("Expected deferred, compile, legacy or bridge case");
      const auto runtime = vm::engine_runtime::acquire();
      const vm::engine_thread_scope thread;
      const std::string_view name(argv[1]);
      if (name == "deferred")
         deferred();
      else if (name == "compile")
         compile();
      else if (name == "legacy")
         legacy();
      else if (name == "bridge")
         bridge();
      else
         throw std::invalid_argument("Unknown JIT load case");
      std::cout << "PASS " << name << '\n';
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
