#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.vm.wasm.optimizing.executor;

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const char* path) {
   auto input = std::ifstream(path, std::ios::binary);
   require(bool(input), "Cannot read guest-trap fixture");
   return {std::istreambuf_iterator<char>(input), {}};
}
struct host {
   std::uint64_t value = 0x71ab32ef442011abULL;
   std::uint64_t next() {
      return ++value;
   }
   void consume(std::uint64_t) {}
   void mark(std::uint32_t) {}
};
using registered = vm::registered_host_functions<host>;
struct test_case {
   std::string name;
   std::uint32_t index;
   oc::trap_reason reason;
};
const auto cases = std::array{test_case{"unreachable", 3, oc::trap_reason::unreachable},
                              test_case{"divide_zero", 4, oc::trap_reason::integer_divide_by_zero},
                              test_case{"overflow", 5, oc::trap_reason::integer_overflow},
                              test_case{"conversion", 6, oc::trap_reason::invalid_conversion},
                              test_case{"table_bounds", 7, oc::trap_reason::table_bounds},
                              test_case{"indirect_type", 8, oc::trap_reason::indirect_type},
                              test_case{"depth", 9, oc::trap_reason::stack_overflow},
                              test_case{"private_index", 10, oc::trap_reason::none},
                              test_case{"unexported_import", 0, oc::trap_reason::none},
                              test_case{"indirect_import", 12, oc::trap_reason::none},
                              test_case{"table_null", 13, oc::trap_reason::table_bounds},
                              test_case{"memory_bounds", 14, oc::trap_reason::memory_bounds}};
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 4)
         return 2;
      const auto selected =
          std::find_if(cases.begin(), cases.end(), [&](const auto& value) { return value.name == argv[1]; });
      require(selected != cases.end(), "Unknown guest-trap case");
      const auto request = oc::compile_request::decode(read(argv[3]), 4 * 1024 * 1024);
      const auto code = oc::artifact::decode(read(argv[2]), oc::identify(request), 16 * 1024 * 1024,
                                             oc::artifact::origin::isolated_compiler);
      registered::add<&host::next>("env", "next");
      registered::add<&host::consume>("env", "consume");
      registered::add<&host::mark>("env", "mark");
      code.prepare();
      const auto binding = oc::executor::bind<registered>(code);
      auto runtime = forge::asio::runtime{};
      auto scheduler = forge::asio::task::scheduler{runtime};
      auto executor = oc::executor{scheduler, {8 * 1024 * 1024, 64 * 1024}};
      auto failure = std::exception_ptr{};
      forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 5s));
      executor.prepare_thread();
      try {
         auto state = host{};
         auto memory = vm::wasm_allocator{};
         const auto deep = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{1025}};
         const auto arguments = selected->name == "depth" ? std::span<const vm::operand_stack_elem>{deep}
                                                          : std::span<const vm::operand_stack_elem>{};
         const auto result = executor.invoke(binding, &state, selected->index, arguments, memory,
                                             std::chrono::steady_clock::now() + 5s);
         require(result.trap == selected->reason && !result.host_error, "Native guest trap lost its canonical kind");
         if (selected->name == "private_index")
            require(result.value && result.value->to_ui32() == 7, "Private indexed function lost its result");
         else if (selected->name == "unexported_import" || selected->name == "indirect_import")
            require(result.value && result.value->to_ui64() == state.value, "Unexported host import was not invoked");
         const auto reused = executor.invoke(binding, &state, 11, {}, memory, std::chrono::steady_clock::now() + 5s);
         require(reused.trap == oc::trap_reason::none && reused.value && reused.value->to_ui32() == 7,
                 "AOT executor did not recover after a genuine guest trap");
      } catch (...) {
         failure = std::current_exception();
      }
      executor.release_thread();
      forge::asio::blocking::run(runtime, executor.shutdown());
      forge::asio::blocking::run(runtime, scheduler.shutdown());
      if (failure)
         std::rethrow_exception(failure);
      std::cout << "PASS AOT " << selected->name << " and shallow reuse\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed error\n";
      return 1;
   }
}
