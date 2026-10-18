#include "details/engine_instance.hxx"
#include "../vm_wasm_execution/fixtures.hxx"
#include <forge/contract/intrinsics.hpp>
#include <forge/contract/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

namespace vm = forge::vm::wasm::interpret::detail;
using bindings = vm::engine_bindings;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}
template <typename Error, typename F> void throws(F&& body) {
   try { body(); }
   catch (const Error&) { return; }
   throw std::runtime_error("Expected typed failure was not raised");
}
struct sentinel { int code; };
struct host {
   std::uint32_t value = 17;
   std::uint32_t mode = 0;
   std::atomic_bool entered{};
   vm::engine_instance* nested{};
   std::stop_source* cancel_before_throw{};
   std::vector<char> input{'o', 'k'};
   std::vector<char> output;
};
void scalar(bindings::call& call) {
   require(call.argument<std::uint32_t>(0) == 7 && call.argument<double>(1) == 2.5 &&
      call.argument<std::uint64_t>(2) == UINT64_C(0x100000003) && call.argument<float>(3) == 1.25f,
      "Mixed scalar arguments changed");
   std::fesetround(FE_UPWARD);
   call.result(9.5);
}
void visit(bindings::call& call) {
   auto& state = *static_cast<host*>(call.host());
   state.entered.store(true, std::memory_order_release);
   switch (state.mode) {
      case 1:
         std::fesetround(FE_UPWARD);
         if (state.cancel_before_throw) state.cancel_before_throw->request_stop();
         throw sentinel{93};
      case 2:
         while (!call.stop().stop_requested()) std::this_thread::yield();
         call.check_stop();
         break;
      case 3: {
         auto memory = call.memory(call.argument<std::uint32_t>(0), call.argument<std::uint32_t>(1));
         if (!memory.empty()) memory.back() = 42;
         break;
      }
      case 4: {
         std::array<std::uint32_t, 1> result{};
         require(state.nested && state.nested->invoke("run", 0, result), "Nested module failed");
         call.result(result[0] + state.value);
         return;
      }
      case 5: (void)call.string(call.argument<std::uint32_t>(0), call.argument<std::uint32_t>(1)); break;
      case 6: return; // Missing typed return must be rejected.
      case 7: (void)call.argument<double>(0); break;
      case 8: throw std::runtime_error("application error text is not a trap classifier");
      case 9: call.result(1.5); return;
      default: break;
   }
   call.result(state.value);
}
std::vector<bindings::function> functions() {
   return {
      bindings::describe("binding", "mix", static_cast<double (*)(std::uint32_t, double, std::uint64_t, float)>(nullptr), scalar),
      bindings::describe("binding", "visit", static_cast<std::uint32_t (*)(std::uint32_t, std::uint32_t)>(nullptr), visit)
   };
}
std::uint32_t invoke(vm::engine_instance& instance, const char* name = "run",
                     std::span<const std::uint32_t> parameters = {}, std::stop_token stop = {}) {
   std::array<std::uint32_t, 8> cells{};
   std::copy(parameters.begin(), parameters.end(), cells.begin());
   require(instance.invoke(name, parameters.size(), cells, stop), "Unexpected guest trap");
   return cells[0];
}
void sdk_action_data_size(bindings::call& c) {
   auto& h = *static_cast<host*>(c.host());
   h.entered.store(true, std::memory_order_release);
   if (h.mode == 1) {
      if (h.cancel_before_throw) h.cancel_before_throw->request_stop();
      throw sentinel{93};
   }
   if (h.mode == 2) {
      while (!c.stop().stop_requested()) std::this_thread::yield();
      c.check_stop();
   }
   c.result(static_cast<std::uint32_t>(h.input.size()));
}
void sdk_read_action_data(bindings::call& c) {
   auto& h = *static_cast<host*>(c.host());
   auto view = c.memory(c.argument<std::uint32_t>(0), c.argument<std::uint32_t>(1));
   auto n = std::min(view.size(), h.input.size());
   if (n) std::memcpy(view.data(), h.input.data(), n);
   c.result(static_cast<std::uint32_t>(n));
}
void sdk_set_action_return_value(bindings::call& c) {
   auto view = c.memory(c.argument<std::uint32_t>(0), c.argument<std::uint32_t>(1));
   static_cast<host*>(c.host())->output.assign(view.begin(), view.end());
}
void sdk_eosio_assert_message(bindings::call& c) {
   if (!c.argument<std::uint32_t>(0)) throw sentinel{12};
}
void sdk_prints_l(bindings::call& c) { (void)c.memory(c.argument<std::uint32_t>(0), c.argument<std::uint32_t>(1)); }
template <typename T> auto sdk_functions() {
   std::vector<bindings::function> result;
   using forge_contract_size_t = std::uint32_t;
   // Instantiate every canonical ABI signature, including pointer and bool
   // entries. Only the execution-profile five have test host implementations.
#define DESCRIBE(version, capability, header, feature, identifier, module_name, import_name, ret, arguments) \
   result.push_back(bindings::describe(#module_name, #import_name, static_cast<ret (*) arguments>(nullptr), \
      +[](bindings::call&) { throw sentinel{99}; }));
   FORGE_CONTRACT_INTRINSICS(DESCRIBE)
#undef DESCRIBE
   for (auto& f : result) {
      if (f.name == "action_data_size") f.invoke = sdk_action_data_size;
      else if (f.name == "read_action_data") f.invoke = sdk_read_action_data;
      else if (f.name == "set_action_return_value") f.invoke = sdk_set_action_return_value;
      else if (f.name == "eosio_assert_message") f.invoke = sdk_eosio_assert_message;
      else if (f.name == "prints_l") f.invoke = sdk_prints_l;
   }
   return result;
}
}

int main(int argc, char** argv) {
   try {
      require(argc == 3, "case and fixture required");
      const auto name = std::string_view(argv[1]);
      std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
      require(input.good() && input.tellg() > 0 && input.tellg() < 4 * 1024 * 1024, "Invalid fixture");
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input.tellg()));
      input.seekg(0);
      require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "Cannot read fixture");
      auto runtime = vm::engine_runtime::acquire();
      auto make_module = [&](auto f) {
         return std::make_shared<vm::engine_module>(runtime, bytes, std::make_shared<bindings>(std::move(f)));
      };
      if (name == "admission") {
         auto f = functions();
         f.pop_back();
         throws<bindings::link_error>([&] { (void)make_module(f); });
         f = functions(); f[0].parameters[1] = WASM_I64;
         throws<bindings::link_error>([&] { (void)make_module(f); });
         f = functions(); f[1].result = WASM_F32;
         throws<bindings::link_error>([&] { (void)make_module(f); });
         f = functions(); f.push_back(f[0]);
         throws<bindings::link_error>([&] { (void)make_module(f); });
         f = functions(); f[0].name = "_mix";
         throws<bindings::link_error>([&] { (void)make_module(f); });
      } else if (name == "sdk" || name == "start") {
         auto f = sdk_functions<void>();
         require(f.size() > 100, "Canonical registry did not expand");
         auto profile = std::make_shared<bindings>(std::move(f));
         host h;
         auto m = std::make_shared<vm::engine_module>(runtime, bytes, profile);
         require(!h.entered, "Module admission executed guest code");
         if (name == "sdk") {
            vm::engine_instance instance(m, 65536, {}, &h);
            std::array<std::uint32_t, 6> cells{};
            require(instance.invoke("apply", 6, cells), "Canonical SDK echo failed");
            require(h.input == h.output, "Canonical SDK roundtrip changed bytes");
            throws<bindings::link_error>([&] {
               (void)std::make_shared<vm::engine_module>(runtime, fixtures::module({}, false, "action_data_size", true), profile);
            });
            throws<std::runtime_error>([&] {
               const auto name = std::string{"action_data_size"} + '\0' + "not-the-same-import";
               (void)std::make_shared<vm::engine_module>(runtime, fixtures::module({}, false, name), profile);
            });
         } else {
            h.mode = 1;
            try { vm::engine_instance fail(m, 65536, {}, &h); throw std::runtime_error("Missing start exception"); }
            catch (const sentinel& s) { require(s.code == 93, "Start lost exception payload"); }
            std::stop_source racing_stop;
            h.cancel_before_throw = &racing_stop;
            try {
               vm::engine_instance fail(m, 65536, racing_stop.get_token(), &h);
               throw std::runtime_error("Missing start exception during cancellation");
            } catch (const sentinel& s) { require(s.code == 93, "Start cancellation masked host failure"); }
            require(racing_stop.stop_requested(), "Start did not request cancellation before throwing");
            h.cancel_before_throw = nullptr;
            h.mode = 2; h.entered = false;
            std::stop_source stop;
            std::jthread cancel([&](std::stop_token done) {
               while (!h.entered.load(std::memory_order_acquire) && !done.stop_requested()) std::this_thread::yield();
               if (!done.stop_requested()) stop.request_stop();
            });
            throws<bindings::canceled>([&] { vm::engine_instance fail(m, 65536, stop.get_token(), &h); });
            cancel.join(); h.entered = false;
            throws<bindings::canceled>([&] { vm::engine_instance fail(m, 65536, stop.get_token(), &h); });
            require(!h.entered, "Pre-canceled start entered host function");
            h.mode = 0;
            vm::engine_instance recovered(m, 65536, {}, &h);
            require(h.entered, "Fresh start did not recover");
         }
      } else {
         auto m = make_module(functions());
         host h;
         vm::engine_instance instance(m, 65536, {}, &h, 3);
         if (name == "scalar") {
            std::array<std::uint32_t, 2> cells{};
            std::fesetround(FE_DOWNWARD);
            require(instance.invoke("scalar_call", 0, cells), "Scalar invocation failed");
            require(std::bit_cast<double>(std::uint64_t(cells[0]) | (std::uint64_t(cells[1]) << 32)) == 9.5,
               "Scalar result changed");
            require(std::fegetround() == FE_DOWNWARD, "Host callback leaked FP state");
         } else if (name == "exceptions") {
            h.mode = 1;
            std::fesetround(FE_DOWNWARD);
            try { invoke(instance); throw std::runtime_error("Missing host exception"); }
            catch (const sentinel& value) { require(value.code == 93, "Host exception payload changed"); }
            require(std::fegetround() == FE_DOWNWARD, "Host exception leaked FP state");
            std::stop_source racing_stop;
            h.cancel_before_throw = &racing_stop;
            try {
               invoke(instance, "run", {}, racing_stop.get_token());
               throw std::runtime_error("Missing host exception during cancellation");
            } catch (const sentinel& value) { require(value.code == 93, "Cancellation masked host failure"); }
            require(racing_stop.stop_requested(), "Host did not request cancellation before throwing");
            require(std::fegetround() == FE_DOWNWARD, "Host cancellation/failure leaked FP state");
            h.cancel_before_throw = nullptr;
            for (auto mode : {6, 7, 9}) {
               h.mode = mode;
               throws<std::logic_error>([&] { invoke(instance); });
            }
            h.mode = 8;
            throws<std::runtime_error>([&] { invoke(instance); });
            h.mode = 0;
            require(invoke(instance) == h.value, "Host failure poisoned the next invocation");
         } else if (name == "memory" || name == "static_memory") {
            throws<std::length_error>([&] { vm::engine_instance too_small(m, 65536, {}, &h, 1); });
            // Zero is a valid page budget, but this module requires two pages.
            throws<std::length_error>([&] { vm::engine_instance zero(m, 65536, {}, &h, 0); });
            h.mode = 3;
            const auto range = [&](std::uint32_t offset, std::uint32_t count) {
               std::array args{offset, count}; return invoke(instance, "range_call", args);
            };
            require(range(131072, 0) == h.value, "Empty end-of-memory range rejected");
            throws<bindings::memory_error>([&] { range(131073, 0); });
            throws<bindings::memory_error>([&] { range(UINT32_MAX, 2); });
            throws<bindings::memory_error>([&] { range(131071, 2); });
            range(131071, 1);
            std::array last{std::uint32_t(131071)};
            require(invoke(instance, "load8", last) == 42, "Host memory write lost");
            h.mode = 5;
            throws<bindings::memory_error>([&] { range(131071, 1); });
            require(range(0, 1) == h.value, "NUL string rejected");
            if (name == "memory") {
               h.mode = 3;
               std::array page{std::uint32_t(1)};
               require(invoke(instance, "grow", page) == 2, "First growth rejected");
               range(196607, 1);
               require(invoke(instance, "grow", page) == UINT32_MAX && invoke(instance, "pages") == 3,
                  "Caller page cap was ignored");
            }
         } else if (name == "cancel" || name == "busy") {
            h.mode = 2;
            std::stop_source stop;
            std::jthread cancel([&](std::stop_token done) {
               while (!h.entered.load(std::memory_order_acquire) && !done.stop_requested()) std::this_thread::yield();
               if (!done.stop_requested()) {
                  if (name == "busy") throws<std::logic_error>([&] { invoke(instance); });
                  stop.request_stop();
               }
            });
            throws<bindings::canceled>([&] { invoke(instance, "run", {}, stop.get_token()); });
            h.mode = 0;
            require(invoke(instance) == h.value, "Cancellation poisoned the next invocation");
            h.entered.store(false);
            throws<bindings::canceled>([&] { invoke(instance, "run", {}, stop.get_token()); });
            require(!h.entered, "Canceled invocation entered a host function");
         } else if (name == "isolation") {
            host other; other.value = 31;
            auto alternate = functions();
            alternate[1].invoke = +[](bindings::call& c) { c.result(static_cast<host*>(c.host())->value + 1); };
            auto second = make_module(std::move(alternate));
            vm::engine_instance nested(second, 65536, {}, &other);
            h.nested = &nested; h.mode = 4;
            require(invoke(instance) == 49, "Nested module lost its binding or invocation context");
            h.mode = 0;
            std::atomic_bool okay{true};
            std::jthread a([&] { for (int i = 0; i < 100; ++i) if (invoke(instance) != 17) okay = false; });
            std::jthread b([&] { for (int i = 0; i < 100; ++i) if (invoke(nested) != 32) okay = false; });
            a.join(); b.join(); require(okay, "Concurrent module context leakage");
         } else throw std::runtime_error("Unknown case");
      }
      std::cout << "PASS " << name << '\n';
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n'; return 1;
   } catch (...) {
      std::cerr << "Unexpected non-standard exception\n"; return 1;
   }
}
