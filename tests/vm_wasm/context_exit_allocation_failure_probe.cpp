#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <vector>

import forge.vm.wasm.interpret.backend;

namespace {
thread_local unsigned fail_after = 0;
thread_local unsigned faults = 0;
} // namespace

void* operator new(std::size_t size) {
   if (fail_after && --fail_after == 0) {
      ++faults;
      throw std::bad_alloc{};
   }
   if (auto* pointer = std::malloc(size ? size : 1))
      return pointer;
   throw std::bad_alloc{};
}
void operator delete(void* pointer) noexcept {
   std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
   std::free(pointer);
}

namespace {
namespace wasm = forge::vm::wasm::interpret;
struct host;
using functions = wasm::registered_host_functions<host>;
using backend = wasm::backend<functions, wasm::interpreter>;
struct first_host_error {};
enum class phase { start, function, caught_reset, first_error };

struct host {
   backend* engine;
   phase selected;
   unsigned calls = 0;
   std::exception_ptr construction_failure;
   std::exception_ptr first_failure;

   void stop() {
      ++calls;
      if (selected == phase::first_error) {
         if (calls == 2) {
            try {
               throw first_host_error{};
            } catch (...) {
               first_failure = std::current_exception();
               throw;
            }
         }
         try {
            engine->call_with_return(*this, "", "run");
         } catch (const first_host_error&) {
         }
      }
      // A single allocation fault, armed only inside the real host callback.
      // Refuse the exception base's first allocation, not a guest allocation.
      fail_after = faults == 0 ? 1 : 0;
      try {
         engine->get_context().exit();
      } catch (const std::bad_alloc&) {
         fail_after = 0;
         construction_failure = std::current_exception();
         throw;
      } catch (...) {
         fail_after = 0;
         throw;
      }
   }
};

std::vector<std::uint8_t> fixture(bool start) {
   auto code = std::vector<std::uint8_t>{0,  0x61, 0x73, 0x6d, 1, 0,   0,   0,   1,   4, 1,   0x60, 0,   0, 2,
                                         10, 1,    1,    'h',  4, 's', 't', 'o', 'p', 0, 0,   3,    2,   1, 0,
                                         5,  4,    1,    1,    1, 1,   7,   7,   1,   3, 'r', 'u',  'n', 0, 1};
   if (start)
      code.insert(code.end(), {8, 1, 1});
   code.insert(code.end(), {10, 7, 1, 5, 0, 0x10, 0, 0, 0x0b});
   return code;
}

bool check(phase selected) {
   auto code = fixture(selected == phase::start);
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto engine = backend{pointer, code.size(), nullptr, wasm::default_options{}, true, false};
   auto receiver = host{&engine, selected};
   auto observed = std::exception_ptr{};
   bool outer_bad_alloc = false;
   bool first_host_error_preserved = false;
   bool body_caught = false;
   bool body_reset = false;
   bool returned = false;
   bool poisoned = false;
   faults = 0;
   try {
      engine.with_invocation(&receiver, {}, std::chrono::seconds{5}, {}, [&] {
         if (selected != phase::start) {
            try {
               engine.call_with_return(receiver, "", "run");
            } catch (const std::bad_alloc&) {
               if (selected != phase::caught_reset)
                  throw;
               body_caught = true;
               engine.get_context().reset();
               body_reset = true;
            }
         }
      });
      returned = true;
   } catch (const std::bad_alloc&) {
      outer_bad_alloc = true;
      observed = std::current_exception();
   } catch (const first_host_error&) {
      first_host_error_preserved = true;
      observed = std::current_exception();
   } catch (...) {
      fail_after = 0;
   }
   fail_after = 0;
   try {
      engine.with_invocation(&receiver, {}, std::chrono::seconds{5}, {}, [] {});
   } catch (const wasm::exceptions::unusable&) {
      poisoned = true;
   } catch (...) {
   }
   const bool first = selected == phase::first_error;
   const bool exact = observed && observed == (first ? receiver.first_failure : receiver.construction_failure);
   const bool pass = faults == 1 && receiver.construction_failure && !returned && poisoned && exact &&
                     (first ? first_host_error_preserved && !outer_bad_alloc : outer_bad_alloc) &&
                     receiver.calls == (first ? 2U : 1U) &&
                     (selected != phase::caught_reset || (body_caught && body_reset));
   std::cout << (pass ? "PASS" : "FAIL") << " case=" << static_cast<unsigned>(selected)
             << " construction_bad_alloc=" << bool(receiver.construction_failure) << " faults=" << faults
             << " false_success=" << returned << " outer_bad_alloc=" << outer_bad_alloc
             << " first_host_error=" << first_host_error_preserved << " exact_failure=" << exact
             << " poisoned=" << poisoned << " body_caught=" << body_caught << " body_reset=" << body_reset
             << " host_calls=" << receiver.calls << '\n';
   return pass;
}
} // namespace

int main() {
   functions::add<&host::stop>("h", "stop");
   unsigned failures = 0;
   for (const auto selected : {phase::start, phase::function, phase::caught_reset, phase::first_error})
      failures += !check(selected);
   std::cout << "cases=4 failures=" << failures << '\n';
   return failures ? 1 : 0;
}
