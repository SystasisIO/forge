#include "details/engine_instance.hxx"
#include "details/engine_thread_scope.hxx"
#include <wasm_exec_env.h>

#include <algorithm>
#include <array>
#include <cfenv>
#include <fstream>
#include <iostream>
#include <string_view>

namespace vm = forge::vm::wasm::interpret::detail;

namespace {
void require(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}
struct host_error { unsigned value; };
enum class behavior { normal, failure, cancel, failure_cancel };
struct context {
   std::vector<unsigned> calls;
   behavior mode{};
   std::stop_source stop;
};

void visit(vm::engine_bindings::call& call, unsigned id) {
   require(call.host() != nullptr, "Host was not installed before initialization");
   auto& host = *static_cast<context*>(call.host());
   if (host.calls.empty())
      require(std::fegetround() == FE_TONEAREST, "Guest initialization inherited host rounding");
   host.calls.push_back(id);
   // Deliberately poison host FP state inside the import. The outer invocation
   // must restore its caller's complete environment on success and failure.
   require(std::fesetround(FE_UPWARD) == 0, "Cannot change callback rounding");
   std::feraiseexcept(FE_INVALID);
   if (host.mode == behavior::cancel || host.mode == behavior::failure_cancel) {
      require(call.stop().stop_possible(), "Cancellation was not installed before initialization");
      host.stop.request_stop();
   }
   if (host.mode == behavior::failure || host.mode == behavior::failure_cancel)
      throw host_error{73};
   call.check_stop();
}
void start(vm::engine_bindings::call& call) { visit(call, 0); }
void post(vm::engine_bindings::call& call) { visit(call, 1); }
void post_result(vm::engine_bindings::call& call) { visit(call, 1); call.result(std::uint32_t{42}); }

class fp_environment {
 public:
   fp_environment() {
      require(std::fegetenv(&_saved) == 0, "Cannot save caller FP environment");
      require(std::fesetround(FE_DOWNWARD) == 0, "Cannot set caller rounding");
      std::feclearexcept(FE_ALL_EXCEPT);
      std::feraiseexcept(FE_DIVBYZERO);
      _flags = std::fetestexcept(FE_ALL_EXCEPT);
   }
   ~fp_environment() { std::fesetenv(&_saved); }
   void check() const {
      require(std::fegetround() == FE_DOWNWARD, "Initialization leaked FP rounding");
      require(std::fetestexcept(FE_ALL_EXCEPT) == _flags, "Initialization leaked FP flags");
   }
 private:
   std::fenv_t _saved{};
   int _flags{};
};

auto load(const char* path, std::string_view mode) {
   std::ifstream input(path, std::ios::binary);
   std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
   require(!bytes.empty(), "Empty imported initialization fixture");
   std::vector<vm::engine_bindings::function> functions{
      vm::engine_bindings::describe("init", "start", static_cast<void (*)()>(nullptr), start)};
   if (mode == "signature_result")
      functions.push_back(vm::engine_bindings::describe("init", "post",
         static_cast<std::uint32_t (*)()>(nullptr), post_result));
   else if (mode == "signature_parameter")
      functions.push_back(vm::engine_bindings::describe("init", "post",
         static_cast<void (*)(std::uint32_t)>(nullptr), post));
   else
      functions.push_back(vm::engine_bindings::describe("init", "post", static_cast<void (*)()>(nullptr), post));
   return std::make_shared<vm::engine_module>(vm::engine_runtime::acquire(), bytes,
      std::make_shared<vm::engine_bindings>(std::move(functions)));
}

void idle(wasm_exec_env_t env) {
   require(env->forge_call_depth == 0, "Initialization leaked logical call depth");
}
void answer(vm::engine_instance& instance) {
   std::array<std::uint32_t, 1> cells{};
   require(instance.invoke("answer", 0, cells) && cells[0] == 42, "Healthy export failed after initialization");
   idle(instance.env());
}

// This lower-level test observes deferred state and depth after failed
// initialization, before the normal C++ constructor tears the instance down.
void deferred(const std::shared_ptr<vm::engine_module>& source, std::string_view mode) {
   const vm::engine_thread_scope thread;
   context host;
   if (mode == "deferred_failure") host.mode = behavior::failure;
   if (mode == "deferred_cancel") host.mode = behavior::cancel;
   vm::engine_bindings::state state{&host, host.stop.get_token(), {}};
   InstantiationArgs2* raw_args{};
   require(wasm_runtime_instantiation_args_create(&raw_args), "Cannot allocate deferred arguments");
   const std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)> args(
      raw_args, wasm_runtime_instantiation_args_destroy);
   wasm_runtime_instantiation_args_set_default_stack_size(args.get(), 1024 * 1024);
   wasm_runtime_instantiation_args_set_defer_start(args.get(), true);
   std::array<char, 512> error{};
   const auto raw_instance = wasm_runtime_instantiate_ex2(source->get(), args.get(), error.data(), error.size());
   require(raw_instance != nullptr, "Deferred instantiation failed");
   const std::unique_ptr<WASMModuleInstanceCommon, decltype(&wasm_runtime_deinstantiate)> instance(
      raw_instance, wasm_runtime_deinstantiate);
   require(host.calls.empty(), "Import ran before deferred initialization");
   const auto raw_env = wasm_runtime_create_exec_env(instance.get(), 1024 * 1024);
   require(raw_env != nullptr, "Cannot create deferred execution environment");
   const std::unique_ptr<WASMExecEnv, decltype(&wasm_runtime_destroy_exec_env)> env(
      raw_env, wasm_runtime_destroy_exec_env);
   env->forge_max_call_depth = mode == "deferred_depth" ? 0 : 1;
   wasm_runtime_set_custom_data(instance.get(), &state);
   const std::stop_callback cancel(host.stop.get_token(), [&] { wasm_runtime_terminate(instance.get()); });
   const fp_environment fp;
   const bool initialized = wasm_runtime_initialize_with_exec_env(instance.get(), env.get());
   fp.check();
   idle(env.get());
   if (mode == "deferred_depth") {
      require(!initialized && env->forge_depth_exceeded, "Imported initializer bypassed logical depth");
      require(host.calls.empty(), "Depth-denied imported initializer executed");
   } else {
      require(host.calls.size() == 1, "Deferred imported initializer was skipped or repeated");
      require(initialized == (mode == "deferred"), "Unexpected deferred initialization outcome");
      if (mode != "deferred") {
         require(state.failure != nullptr, "Imported initialization lost typed host failure");
         try { std::rethrow_exception(state.failure); }
         catch (const host_error& value) {
            require(mode == "deferred_failure" && value.value == 73, "Wrong deferred host error");
         }
         catch (const vm::engine_instance::canceled&) {
            require(mode == "deferred_cancel", "Unexpected deferred cancellation");
         }
      }
   }
   const auto calls = host.calls.size();
   wasm_runtime_clear_exception(instance.get());
   require(!wasm_runtime_initialize_with_exec_env(instance.get(), env.get()), "Initialization was replayed");
   require(host.calls.size() == calls, "Second initialization executed an import");
   fp.check();
   idle(env.get());
}

void run(const std::shared_ptr<vm::engine_module>& source, std::string_view mode) {
   if (mode.starts_with("deferred")) { deferred(source, mode); return; }
   context host;
   if (mode == "failure") host.mode = behavior::failure;
   if (mode == "cancel") host.mode = behavior::cancel;
   if (mode == "failure_cancel") host.mode = behavior::failure_cancel;
   if (mode == "precancel") host.stop.request_stop();
   const bool signature = mode.starts_with("signature_");
   const bool failed = mode == "failure" || mode == "failure_cancel";
   const bool canceled = mode == "cancel" || mode == "precancel";
   const bool denied = mode == "depth";
   std::exception_ptr captured;
   const fp_environment fp;
   try {
      vm::engine_instance instance(source, 1024 * 1024, host.stop.get_token(), &host, 8,
         denied ? 0 : 1, &captured);
      require(!failed && !canceled && !denied, "Expected imported initialization failure was lost");
      fp.check();
      const auto count = mode == "order" ? 2U : signature ? 0U : 1U;
      require(host.calls.size() == count, "Imported initializer was skipped or repeated");
      if (mode == "order") require(host.calls == std::vector<unsigned>{0, 1}, "Initialization order changed");
      idle(instance.env());
      answer(instance);
      answer(instance);
      require(host.calls.size() == count, "Ordinary invocation repeated initialization");
   } catch (const host_error& value) {
      require(failed && value.value == 73 && captured != nullptr, "Typed host failure was not preserved");
   } catch (const vm::engine_instance::canceled&) {
      require(canceled && !failed, "Cancellation replaced a typed host failure");
   } catch (const vm::engine_instance::depth_exceeded&) {
      require(denied, "Unexpected imported initialization depth error");
   }
   fp.check();
   if (failed || mode == "cancel") require(host.calls.size() == 1, "Failing initializer was skipped or repeated");
   if (denied || mode == "precancel") require(host.calls.empty(), "Denied initializer executed");
   // A failed instance must not poison another instance of the same module.
   if (failed || canceled || denied) {
      context healthy;
      vm::engine_instance instance(source, 1024 * 1024, {}, &healthy, 8, 1);
      require(healthy.calls.size() == 1, "Module did not recover after initialization failure");
      answer(instance);
      fp.check();
   }
}
}

int main(int argc, char** argv) {
   try {
      if (argc != 3) return 1;
      const std::string_view mode(argv[1]);
      const std::array modes{"success", "order", "failure", "cancel", "failure_cancel", "precancel", "depth",
         "signature_result", "signature_parameter", "deferred", "deferred_failure", "deferred_cancel", "deferred_depth"};
      require(std::find(modes.begin(), modes.end(), mode) != modes.end(), "Unknown imported initialization case");
      run(load(argv[2], mode), mode);
      std::cout << "PASS imported initialization " << mode << '\n';
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "Unexpected non-standard initialization exception\n";
      return 1;
   }
}
