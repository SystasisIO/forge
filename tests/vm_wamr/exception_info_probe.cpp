#include "details/engine_instance.hxx"
#include "details/engine_memory.hxx"
#include "details/engine_thread_scope.hxx"
#include <aot_runtime.h>
#include <thread_manager.h>
#include <wasm_exec_env.h>

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

namespace vm = forge::vm::wasm::interpret::detail;
static_assert(WASM_EXCEPTION_NONE == 0 && WASM_EXCEPTION_UNKNOWN == 1 && WASM_EXCEPTION_KNOWN == 2);
static_assert(std::is_same_v<decltype(&wasm_runtime_get_exception_info), WASMExceptionInfo (*)(wasm_module_inst_t)>);
static_assert(EXCE_UNREACHABLE == 0 && EXCE_OUT_OF_MEMORY == 1 && EXCE_NATIVE_STACK_OVERFLOW == 11 &&
   EXCE_OPERAND_STACK_OVERFLOW == 16 && EXCE_ALREADY_THROWN == 33 && EXCE_NUM == 34);

namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void expect(wasm_module_inst_t instance, WASMExceptionStatus status, unsigned id = 0) {
   const auto info = wasm_runtime_get_exception_info(instance);
   require(info.status == status, "Unexpected exception status");
   if (status == WASM_EXCEPTION_KNOWN) require(info.id == id, "Actual engine exception ID was lost");
}
void message(wasm_module_inst_t instance, const char* text) {
   std::array<char, EXCEPTION_BUF_LEN> buffer{};
   require(wasm_copy_exception(reinterpret_cast<WASMModuleInstance*>(instance), buffer.data()), "Expected exception message");
   require(std::strcmp(buffer.data(), text) == 0, "Exception diagnostic changed");
}
void known(wasm_module_inst_t instance, unsigned id) {
   wasm_set_exception_with_id(reinterpret_cast<WASMModuleInstance*>(instance), id);
}
struct host_error { unsigned value; };
enum class behavior { normal, failure, cancel, failure_cancel };
struct context { behavior mode{}; unsigned calls{}; std::stop_source stop; };
void host(vm::engine_bindings::call& call) {
   auto& state = *static_cast<context*>(call.host());
   ++state.calls;
   if (state.mode == behavior::cancel || state.mode == behavior::failure_cancel) state.stop.request_stop();
   if (state.mode == behavior::failure || state.mode == behavior::failure_cancel) throw host_error{73};
   call.check_stop();
}
auto load(const char* path) {
   std::ifstream input(path, std::ios::binary);
   std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
   require(!bytes.empty(), "Empty exception fixture");
   return std::make_shared<vm::engine_module>(vm::engine_runtime::acquire(), bytes,
      std::make_shared<vm::engine_bindings>(std::vector<vm::engine_bindings::function>{
         vm::engine_bindings::describe("info", "host", static_cast<void (*)()>(nullptr), host)}));
}

class instance {
 public:
   explicit instance(const std::shared_ptr<vm::engine_module>& source, unsigned stack = 1024 * 1024,
      bool check_owner = false) {
      InstantiationArgs2* raw{};
      require(wasm_runtime_instantiation_args_create(&raw), "Cannot allocate instance arguments");
      const std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)> args(
         raw, wasm_runtime_instantiation_args_destroy);
      wasm_runtime_instantiation_args_set_default_stack_size(args.get(), stack);
      wasm_runtime_instantiation_args_set_defer_start(args.get(), true);
      std::array<char, 512> error{};
      if (check_owner) {
         auto* unowned = wasm_runtime_instantiate_ex2(source->get(), args.get(), error.data(), error.size());
         if (unowned) {
            const vm::engine_memory::scope cleanup_scope(_memory);
            wasm_runtime_deinstantiate(unowned);
            throw std::runtime_error("Raw instantiation unexpectedly accepted an absent memory owner");
         }
         error.fill(0);
      }
      const vm::engine_memory::scope memory_scope(_memory);
      _inst = wasm_runtime_instantiate_ex2(source->get(), args.get(), error.data(), error.size());
      if (!_inst) throw std::runtime_error(std::string("Exception fixture instantiation failed: ") + error.data());
      _memory.bind_instance(_inst);
      const auto destroy_instance = [this](WASMModuleInstanceCommon* value) {
         wasm_runtime_deinstantiate(value);
         _memory.bind_instance(nullptr);
      };
      std::unique_ptr<WASMModuleInstanceCommon, decltype(destroy_instance)> instance_guard(_inst, destroy_instance);
      _env = wasm_runtime_create_exec_env(_inst, stack);
      if (!_env) throw std::runtime_error("Cannot create environment");
      std::unique_ptr<WASMExecEnv, decltype(&wasm_runtime_destroy_exec_env)> env_guard(_env, wasm_runtime_destroy_exec_env);
      _env->forge_max_call_depth = 16;
      state.host = &host;
      state.stop = host.stop.get_token();
      wasm_runtime_set_custom_data(_inst, &state);
      expect(_inst, WASM_EXCEPTION_NONE);
      env_guard.release();
      instance_guard.release();
   }
   ~instance() {
      const vm::engine_memory::scope memory_scope(_memory);
      wasm_runtime_destroy_exec_env(_env);
      wasm_runtime_deinstantiate(_inst);
      _memory.bind_instance(nullptr);
   }
   bool initialize() {
      const vm::engine_memory::scope memory_scope(_memory);
      const std::stop_callback cancel(host.stop.get_token(), [&] { wasm_runtime_terminate(_inst); });
      return wasm_runtime_initialize_with_exec_env(_inst, _env);
   }
   bool call(const char* name, unsigned argument = 0, unsigned argc = 0) {
      const vm::engine_memory::scope memory_scope(_memory);
      wasm_runtime_clear_exception(_inst);
      _env->forge_depth_exceeded = 0;
      std::array<std::uint32_t, 1> cells{argument};
      const auto function = wasm_runtime_lookup_function(_inst, name);
      require(function != nullptr, "Missing exception fixture export");
      const bool result = wasm_runtime_call_wasm(_env, function, argc, cells.data());
      require(_env->forge_call_depth == 0, "Trap leaked logical depth");
      return result;
   }
   wasm_module_inst_t get() const { return _inst; }
   wasm_exec_env_t env() const { return _env; }
   context host;
   vm::engine_bindings::state state;
 private:
   vm::engine_memory _memory = vm::engine_memory::system();
   wasm_module_inst_t _inst{};
   wasm_exec_env_t _env{};
};

void api(const std::shared_ptr<vm::engine_module>& source) {
   instance value(source, 1024 * 1024, true);
   require(value.initialize(), "Control initialization failed");
   for (unsigned id = 0; id < EXCE_NUM; ++id) {
      if (id == EXCE_ALREADY_THROWN) continue;
      known(value.get(), id);
      expect(value.get(), WASM_EXCEPTION_KNOWN, id);
   }
   known(value.get(), EXCE_UNREACHABLE);
   message(value.get(), "Exception: unreachable");
   known(value.get(), EXCE_ALREADY_THROWN);
   expect(value.get(), WASM_EXCEPTION_KNOWN, EXCE_UNREACHABLE);
   if (source->get()->module_type == Wasm_Module_AoT) {
      aot_set_exception_with_id(reinterpret_cast<AOTModuleInstance*>(value.get()), EXCE_ALREADY_THROWN);
      expect(value.get(), WASM_EXCEPTION_KNOWN, EXCE_UNREACHABLE);
   }
   wasm_runtime_set_exception(value.get(), "unreachable");
   expect(value.get(), WASM_EXCEPTION_UNKNOWN);
   message(value.get(), "Exception: unreachable");
   known(value.get(), EXCE_ALREADY_THROWN);
   expect(value.get(), WASM_EXCEPTION_UNKNOWN);
   wasm_runtime_set_exception(value.get(), "");
   expect(value.get(), WASM_EXCEPTION_UNKNOWN);
   message(value.get(), "Exception: ");
   known(value.get(), UINT32_MAX);
   expect(value.get(), WASM_EXCEPTION_UNKNOWN);
   message(value.get(), "Exception: unknown exception");
   wasm_set_exception_with_id_and_message(reinterpret_cast<WASMModuleInstance*>(value.get()),
      EXCE_OUT_OF_MEMORY, "out of memory");
   expect(value.get(), WASM_EXCEPTION_KNOWN, EXCE_OUT_OF_MEMORY);
   message(value.get(), "Exception: out of memory");
   wasm_runtime_clear_exception(value.get());
   expect(value.get(), WASM_EXCEPTION_NONE);
   require(!wasm_runtime_get_exception(value.get()), "Clear retained a message");
   known(value.get(), EXCE_INTEGER_OVERFLOW);
   wasm_runtime_set_exception(value.get(), nullptr);
   expect(value.get(), WASM_EXCEPTION_NONE);
   require(value.call("answer"), "Instance did not recover after clear");
   expect(value.get(), WASM_EXCEPTION_NONE);
}

void guest(const std::shared_ptr<vm::engine_module>& source) {
   instance value(source);
   require(value.initialize(), "Control initialization failed");
   struct test { const char* name; unsigned id; const char* text; unsigned argument; unsigned argc; };
   const std::array tests{
      test{"unreachable", EXCE_UNREACHABLE, "Exception: unreachable", 0, 0},
      test{"divide", EXCE_INTEGER_DIVIDE_BY_ZERO, "Exception: integer divide by zero", 0, 1},
      test{"overflow", EXCE_INTEGER_OVERFLOW, "Exception: integer overflow", 0, 0},
      test{"conversion", EXCE_INVALID_CONVERSION_TO_INTEGER, "Exception: invalid conversion to integer", 0, 0},
      test{"bounds", EXCE_OUT_OF_BOUNDS_MEMORY_ACCESS, "Exception: out of bounds memory access", 65536, 1},
      test{"indirect", EXCE_INVALID_FUNCTION_TYPE_INDEX, "Exception: indirect call type mismatch", 1, 1},
      test{"indirect", EXCE_UNINITIALIZED_ELEMENT, "Exception: uninitialized element", 2, 1},
      test{"indirect", EXCE_UNDEFINED_ELEMENT, "Exception: undefined element", 3, 1}
   };
   for (const auto& item : tests) {
      require(!value.call(item.name, item.argument, item.argc), "Expected guest trap");
      expect(value.get(), WASM_EXCEPTION_KNOWN, item.id);
      message(value.get(), item.text);
      require(value.call("answer"), "Healthy reuse failed after a trap");
      expect(value.get(), WASM_EXCEPTION_NONE);
   }
}

void forwarding(const std::shared_ptr<vm::engine_module>& source) {
   instance parent(source);
   require(parent.initialize(), "Control initialization failed");
   auto child_memory = vm::engine_memory::system();
   const auto raw_child = [&] {
      const vm::engine_memory::scope memory_scope(child_memory);
      return wasm_cluster_spawn_exec_env(parent.env());
   }();
   require(raw_child != nullptr, "Cannot spawn cluster peer");
   const auto destroy_child = [&child_memory](WASMExecEnv* value) {
      const vm::engine_memory::scope memory_scope(child_memory);
      wasm_cluster_destroy_spawned_exec_env(value);
      child_memory.bind_instance(nullptr);
   };
   const std::unique_ptr<WASMExecEnv, decltype(destroy_child)> child(raw_child, destroy_child);
   const auto peer = wasm_runtime_get_module_inst(child.get());
   child_memory.bind_instance(peer);
   known(parent.get(), EXCE_INTEGER_DIVIDE_BY_ZERO);
   expect(peer, WASM_EXCEPTION_KNOWN, EXCE_INTEGER_DIVIDE_BY_ZERO);
   message(peer, "Exception: integer divide by zero");
   wasm_runtime_set_exception(parent.get(), "unreachable");
   expect(peer, WASM_EXCEPTION_UNKNOWN);
   wasm_runtime_clear_exception(parent.get());
   expect(peer, WASM_EXCEPTION_NONE);
   // Restore copies an already formatted message; it must neither lose the ID
   // nor add another Exception: prefix.
   wasm_exec_env_set_module_inst(parent.env(), peer);
   known(peer, EXCE_INTEGER_OVERFLOW);
   wasm_exec_env_restore_module_inst(parent.env(), parent.get());
   expect(parent.get(), WASM_EXCEPTION_KNOWN, EXCE_INTEGER_OVERFLOW);
   message(parent.get(), "Exception: integer overflow");
   wasm_runtime_clear_exception(parent.get());
   wasm_exec_env_set_module_inst(parent.env(), peer);
   wasm_runtime_set_exception(peer, "unreachable");
   wasm_exec_env_restore_module_inst(parent.env(), parent.get());
   expect(parent.get(), WASM_EXCEPTION_UNKNOWN);
   wasm_runtime_clear_exception(parent.get());
}

void initialization(const std::shared_ptr<vm::engine_module>& source, bool trap) {
   for (auto mode : {behavior::failure, behavior::cancel, behavior::failure_cancel}) {
      WASMExceptionInfo captured{};
      std::exception_ptr failure;
      {
         instance value(source);
         value.host.mode = mode;
         require(!value.initialize(), "Expected initialization failure");
         captured = wasm_runtime_get_exception_info(value.get());
         failure = value.state.failure;
         require(value.env()->forge_call_depth == 0, "Initialization leaked depth");
         if (trap) message(value.get(), "Exception: unreachable");
      }
      // The snapshot is copied before the failed instance is destroyed.
      require(captured.status == (trap ? WASM_EXCEPTION_KNOWN : WASM_EXCEPTION_UNKNOWN), "Lost initialization status");
      if (trap) require(captured.id == EXCE_UNREACHABLE && !failure, "Lost initialization trap ID");
      else {
         require(failure != nullptr, "Lost typed initialization host failure");
         try { std::rethrow_exception(failure); }
         catch (const host_error& error) { require(mode != behavior::cancel && error.value == 73, "Host error changed"); }
         catch (const vm::engine_instance::canceled&) { require(mode == behavior::cancel, "Cancel replaced host error"); }
      }
      context host_state;
      host_state.mode = mode;
      try {
         vm::engine_instance value(source, 1024 * 1024, host_state.stop.get_token(), &host_state, 2, 16);
         throw std::runtime_error("Expected constructor initialization failure");
      } catch (const vm::engine_instance::initialization_trap& error) {
         require(trap && error.reason == vm::engine_exception::reason::unreachable,
            "Constructor did not capture the structured trap before destruction");
      } catch (const host_error& error) {
         require(!trap && mode != behavior::cancel && error.value == 73, "Constructor lost host error priority");
      } catch (const vm::engine_instance::canceled&) {
         require(!trap && mode == behavior::cancel, "Constructor cancellation replaced host error");
      }
   }
}

void priority(const std::shared_ptr<vm::engine_module>& source) {
   context host_state;
   vm::engine_instance value(source, 1024 * 1024, {}, &host_state, 2, 16);
   for (auto mode : {behavior::failure, behavior::cancel, behavior::failure_cancel}) {
      host_state.mode = mode;
      host_state.stop = std::stop_source{};
      std::array<std::uint32_t, 1> cells{};
      try {
         value.invoke("host", 0, cells, host_state.stop.get_token());
         throw std::runtime_error("Expected typed invocation failure");
      } catch (const host_error& error) {
         require(mode != behavior::cancel && error.value == 73, "Typed host failure lost priority");
      } catch (const vm::engine_instance::canceled&) {
         require(mode == behavior::cancel, "Cancel replaced typed host failure");
      }
      expect(value.get(), WASM_EXCEPTION_UNKNOWN);
      host_state.mode = behavior::normal;
      require(value.invoke("answer", 0, cells) && cells[0] == 42, "Typed failure poisoned reuse");
      expect(value.get(), WASM_EXCEPTION_NONE);
   }
}

void resources(const std::shared_ptr<vm::engine_module>& source) {
   instance value(source, 8192);
   require(value.initialize(), "Control initialization failed");
   if (source->get()->module_type == Wasm_Module_Bytecode) {
      value.env()->forge_max_call_depth = UINT32_MAX;
      require(!value.call("recurse"), "Expected operand stack exhaustion");
      expect(value.get(), WASM_EXCEPTION_KNOWN, EXCE_OPERAND_STACK_OVERFLOW);
   } else {
      require(!value.call("recurse"), "Expected logical depth denial");
      require(value.env()->forge_depth_exceeded != 0, "Logical depth flag was lost");
      // Preserve the actual ID emitted by the existing AOT depth instrumenter.
      expect(value.get(), WASM_EXCEPTION_KNOWN, EXCE_NATIVE_STACK_OVERFLOW);
   }
   require(value.call("answer"), "Resource failure poisoned reuse");
   expect(value.get(), WASM_EXCEPTION_NONE);
}

void concurrent(const std::shared_ptr<vm::engine_module>& source) {
   instance value(source);
   require(value.initialize(), "Control initialization failed");
   std::atomic_bool done{};
   std::jthread writer([&] {
      const vm::engine_thread_scope thread;
      for (unsigned i = 0; i < 1000; ++i) {
         known(value.get(), EXCE_UNREACHABLE);
         wasm_runtime_set_exception(value.get(), "custom");
         known(value.get(), EXCE_INTEGER_DIVIDE_BY_ZERO);
         wasm_runtime_clear_exception(value.get());
      }
      done.store(true, std::memory_order_release);
   });
   do {
      const auto info = wasm_runtime_get_exception_info(value.get());
      require(info.status == WASM_EXCEPTION_NONE || info.status == WASM_EXCEPTION_UNKNOWN ||
         (info.status == WASM_EXCEPTION_KNOWN && (info.id == EXCE_UNREACHABLE || info.id == EXCE_INTEGER_DIVIDE_BY_ZERO)),
         "Getter returned a torn exception snapshot");
      auto* inst = reinterpret_cast<WASMModuleInstance*>(value.get());
      exception_lock(inst);
      const auto locked = GetModuleInstanceExtraCommon(inst)->exception_info;
      const auto text = inst->cur_exception;
      const bool coherent = locked.status == WASM_EXCEPTION_NONE ? text[0] == 0 :
         locked.status == WASM_EXCEPTION_UNKNOWN ? std::strcmp(text, "Exception: custom") == 0 :
         std::strcmp(text, locked.id == EXCE_UNREACHABLE ? "Exception: unreachable" : "Exception: integer divide by zero") == 0;
      exception_unlock(inst);
      require(coherent, "Message and exception info were not published atomically");
   } while (!done.load(std::memory_order_acquire));
   writer.join();
   expect(value.get(), WASM_EXCEPTION_NONE);
}
}
int main(int argc, char** argv) {
   try {
      if (argc != 3) return 1;
      const auto runtime = vm::engine_runtime::acquire();
      const vm::engine_thread_scope thread;
      auto source = load(argv[2]);
      const std::string_view mode(argv[1]);
      if (mode == "api") api(source);
      else if (mode == "guest") guest(source);
      else if (mode == "forwarding") forwarding(source);
      else if (mode == "init_trap") initialization(source, true);
      else if (mode == "init_host") initialization(source, false);
      else if (mode == "priority") priority(source);
      else if (mode == "resources") resources(source);
      else if (mode == "concurrent") concurrent(source);
      else throw std::runtime_error("Unknown exception info case");
      std::cout << "PASS exception info " << mode << '\n';
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n'; return 1;
   } catch (...) {
      std::cerr << "Unexpected typed exception\n"; return 1;
   }
}
