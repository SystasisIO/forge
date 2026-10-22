#include "details/engine_instance.hxx"
#include <wasm_exec_env.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace vm = forge::vm::wasm::interpret::detail;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
template <typename Error, typename F> void throws(F&& f) {
   try { f(); } catch (const Error&) { return; }
   throw std::runtime_error("Expected typed failure");
}
struct sentinel {};
struct context {
   unsigned calls{};
   bool fail{};
   vm::engine_instance* nested{};
   std::atomic_bool entered{};
};
void visit(vm::engine_bindings::call& call) {
   auto& state = *static_cast<context*>(call.host());
   state.entered.store(true, std::memory_order_release);
   ++state.calls;
   if (state.fail) throw sentinel{};
   if (state.nested) {
      std::array<std::uint32_t, 1> cells{};
      require(state.nested->invoke("leaf", 0, cells) && cells[0] == 42, "Independent nested module failed");
   }
   call.result(std::uint32_t{42});
}
auto load(const char* path) {
   std::ifstream input(path, std::ios::binary);
   std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
   require(!bytes.empty(), "Empty depth fixture");
   return std::make_shared<vm::engine_module>(vm::engine_runtime::acquire(), bytes,
      std::make_shared<vm::engine_bindings>(std::vector<vm::engine_bindings::function>{
         vm::engine_bindings::describe("depth", "visit", static_cast<std::uint32_t (*)()>(nullptr), visit)}));
}
void idle(vm::engine_instance& instance) {
   require(instance.env()->forge_call_depth == 0, "Logical call depth leaked across invocations");
}
void invoke(vm::engine_instance& instance, const char* name, unsigned n = 0, unsigned args = 1) {
   std::array<std::uint32_t, 1> cells{n};
   require(instance.invoke(name, args, cells) && cells[0] == 42, "Unexpected depth fixture result");
   idle(instance);
}
}

int main(int argc, char** argv) {
   try {
      if (argc != 3) return 1;
      auto source = load(argv[1]);
      context host;
      vm::engine_instance zero(source, 1024 * 1024, {}, &host, 8, 0);
      throws<vm::engine_instance::depth_exceeded>([&] { invoke(zero, "leaf", 0, 0); });
      throws<vm::engine_instance::depth_exceeded>([&] { invoke(zero, "native", 0, 0); });
      idle(zero);
      vm::engine_instance one(source, 1024 * 1024, {}, &host, 8, 1);
      invoke(one, "leaf", 0, 0);
      throws<vm::engine_instance::depth_exceeded>([&] { invoke(one, "host"); });
      require(host.calls == 0, "Host entered beyond logical depth limit");
      idle(one);
      invoke(one, "native", 0, 0);
      vm::engine_instance instance(source, 1024 * 1024, {}, &host, 8, 16);
      for (unsigned repeat = 0; repeat < 20; ++repeat) {
         for (auto name : {"direct", "indirect"}) {
            invoke(instance, name, 15);
            throws<vm::engine_instance::depth_exceeded>([&] { invoke(instance, name, 16); });
            idle(instance);
            invoke(instance, name, 15);
         }
         invoke(instance, "host", 14);
         const auto before = host.calls;
         throws<vm::engine_instance::depth_exceeded>([&] { invoke(instance, "host", 15); });
         require(host.calls == before, "Depth-denied intrinsic ran");
         idle(instance);
      }
      std::array<std::uint32_t, 1> cells{};
      require(!instance.invoke("trap", 0, cells), "Unreachable did not trap");
      idle(instance);
      invoke(instance, "direct", 15);
      host.fail = true;
      throws<sentinel>([&] { invoke(instance, "host", 14); });
      host.fail = false;
      idle(instance);
      invoke(instance, "host", 14);
      host.nested = &one;
      invoke(instance, "host", 14);
      host.nested = nullptr;
      std::stop_source stop;
      host.entered.store(false, std::memory_order_release);
      std::atomic_bool completed{};
      std::exception_ptr failure;
      std::jthread worker([&] {
         try { instance.invoke("loop", 0, cells, stop.get_token()); }
         catch (const vm::engine_instance::canceled&) { completed = true; }
         catch (...) { failure = std::current_exception(); }
      });
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!host.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
         std::this_thread::yield();
      const bool entered = host.entered.load(std::memory_order_acquire);
      std::this_thread::sleep_for(20ms);
      stop.request_stop();
      worker.join();
      require(entered, "Guest never entered the invocation before cancellation");
      if (failure) std::rethrow_exception(failure);
      require(completed, "Active guest cancellation was not propagated");
      idle(instance);
      invoke(instance, "direct", 15);
      auto start = load(argv[2]);
      throws<vm::engine_instance::depth_exceeded>([&] {
         vm::engine_instance denied(start, 1024 * 1024, {}, &host, 8, 4);
      });
      vm::engine_instance accepted(start, 1024 * 1024, {}, &host, 8, 5);
      idle(accepted);
      std::cout << "PASS direct/indirect/host/start depth and restoration after trap, host failure and cancellation\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n'; return 1;
   }
}
