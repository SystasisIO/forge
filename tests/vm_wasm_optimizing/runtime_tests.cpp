#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.vm.wasm.optimizing.executor;
import forge.vm.wasm.optimizing.exceptions;

#include "operational_oracles.hxx"

namespace {
namespace oc = forge::vm::wasm::optimizing;
template <typename T>
concept raw_entry = requires(const T& value) { value.entry(0u); };
template <typename T>
concept entry_pointer = requires(const T& value) { value.entry; };
static_assert(!raw_entry<oc::artifact> && !entry_pointer<oc::native_function>);
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const char* path) {
   auto input = std::ifstream(path, std::ios::binary);
   if (!input)
      throw std::runtime_error("Cannot read optimizing fixture");
   return {std::istreambuf_iterator<char>(input), {}};
}
struct host_failure {
   const void* identity;
};
struct floating_point_environment {
   std::fenv_t original{};
   int rounding = FE_UPWARD;
   int flags = 0;
   floating_point_environment() {
      require(std::fegetenv(&original) == 0, "Cannot save caller floating-point environment");
      require(std::fesetround(rounding) == 0 && std::feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  std::feraiseexcept(FE_INVALID | FE_DIVBYZERO) == 0,
              "Cannot stage non-default caller floating-point environment");
      flags = std::fetestexcept(FE_ALL_EXCEPT);
   }
   ~floating_point_environment() {
      std::fesetenv(&original);
   }
   void check() const {
      require(std::fegetround() == rounding && std::fetestexcept(FE_ALL_EXCEPT) == flags,
              "AOT changed caller floating-point environment");
   }
};
struct host {
   std::uint64_t random = 0x7a461cb5832d910fULL;
   std::uint64_t issued_sum = 0, consumed_sum = 0, issued = 0, consumed = 0;
   std::uint32_t last_depth_argument = UINT32_MAX;
   std::uint64_t marked = 0;
   std::stop_source* stop = nullptr;
   bool fail = false;
   std::uint64_t next() {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      issued_sum += random;
      ++issued;
      return random;
   }
   void consume(std::uint64_t value) {
      consumed_sum += value;
      ++consumed;
   }
   void mark(std::uint32_t depth) {
      last_depth_argument = depth;
      ++marked;
      if (stop)
         stop->request_stop();
      if (fail)
         throw host_failure{this};
   }
};
using registered = vm::registered_host_functions<host>;

void run(const std::string& test, oc::artifact code, unsigned budget) {
   if (test.starts_with("oracle_")) {
      operational_oracles::run(test, std::move(code));
      return;
   }
   registered::add<&host::next>("env", "next");
   registered::add<&host::consume>("env", "consume");
   registered::add<&host::mark>("env", "mark");
   code.prepare();
   require(code.is_prepared(), "AOT preparation did not publish");
   if (test == "runtime_pin") {
      require(code.is_unique(), "Runtime pin fixture unexpectedly shares its artifact");
      {
         const auto module = code.runtime_module();
         require(module && !code.is_unique(), "Runtime module pin did not retain its artifact owner");
         require(module->native_code().data() == code.native_code().data(), "Runtime pin changed the native mapping");
      }
      require(code.is_unique(), "Released runtime pin still retains its artifact owner");
      auto owner = std::optional<oc::artifact>{std::move(code)};
      const auto native = owner->native_code();
      auto module = owner->runtime_module();
      const auto weak = std::weak_ptr{module};
      require(!owner->is_unique(), "Exported runtime pin allowed artifact eviction");
      owner.reset();
      require(!weak.expired() && module->native_code().data() == native.data() &&
                  module->native_code().size() == native.size(),
              "Dropping artifact invalidated an owning runtime pin");
      module.reset();
      require(weak.expired(), "Final runtime pin did not retire its artifact owner");
      std::cout << "PASS runtime module outlives artifact and retains its eviction pin until final owner\n";
      return;
   }
   const auto binding = oc::executor::bind<registered>(code);
   const auto heavy = std::ranges::find(code.exports(), "heavy", &oc::compiled_export::name);
   const auto spin = std::ranges::find(code.exports(), "spin", &oc::compiled_export::name);
   const auto imported = std::ranges::find(code.exports(), "imported_next", &oc::compiled_export::name);
   require(heavy != code.exports().end() && spin != code.exports().end() && imported != code.exports().end(),
           "Missing fixture export");
   const auto locals = (budget - 16) / 8;
   const auto native_frame = code.functions()[heavy->function_index].stack_bytes;
   require(native_frame + 256 >= locals * 8, "LLVM removed the supposedly live native frame");
   require(code.image().configuration.execution.max_call_depth == 1024 &&
               code.image().configuration.execution.max_func_local_bytes == 32768,
           "Heavy AOT fixture silently reduced the admitted capacity");

   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   const auto physical = test == "physical" ? 8ULL * 1024 * 1024 : 64ULL * 1024 * 1024;
   std::cout << "AOT source_budget=" << budget << " live_native_frame=" << native_frame
             << " physical_stack=" << physical << std::endl;
   auto executor = oc::executor{scheduler, {physical, 64 * 1024}};
   auto failure = std::exception_ptr{};
   forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 5s));
   executor.prepare_thread();
   try {
      auto memory = vm::wasm_allocator{};
      auto state = host{};
      const auto shallow = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{0}};
      const auto deep = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{1022}};
      const auto excess_depth = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{1023}};
      const auto invoke = [&](std::uint32_t index, std::span<const vm::operand_stack_elem> arguments,
                              std::chrono::steady_clock::time_point deadline, std::stop_token stop = {}) {
         const auto environment = floating_point_environment{};
         try {
            const auto result = executor.invoke(binding, &state, index, arguments, memory, deadline, stop);
            environment.check();
            return result;
         } catch (...) {
            environment.check();
            throw;
         }
      };
      if (test == "heavy") {
         const auto before = std::fegetround();
         const auto result = [&] {
            try {
               return invoke(heavy->function_index, deep, std::chrono::steady_clock::now() + 120s);
            } catch (...) {
               std::cout << "AOT first_failure marks=" << state.marked
                         << " last_depth_argument=" << state.last_depth_argument << " issued=" << state.issued
                         << " consumed=" << state.consumed << std::endl;
               throw;
            }
         }();
         require(result.trap == oc::trap_reason::none && !result.value, "Heavy native invocation failed");
         require(state.issued == std::uint64_t(locals) * 1023 && state.consumed == state.issued &&
                     state.consumed_sum == state.issued_sum,
                 "Live values changed across native recursion");
         require(state.marked == 1023 && state.last_depth_argument == 0,
                 "Heavy AOT did not reach 1023 guest frames plus one host frame");
         require(std::fegetround() == before, "AOT changed caller floating-point environment");
      } else if (test == "depth") {
         const auto result = invoke(heavy->function_index, excess_depth, std::chrono::steady_clock::now() + 120s);
         require(result.trap == oc::trap_reason::stack_overflow && !result.host_error,
                 "Logical heavy-frame depth overflow lost its canonical classification");
         require(state.marked == 1023 && state.last_depth_argument == 1 && state.consumed == 0,
                 "Logical depth overflow did not reject the first excess host call");
      } else if (test == "physical") {
         auto unavailable = false;
         try {
            static_cast<void>(invoke(heavy->function_index, deep, std::chrono::steady_clock::now() + 120s));
         } catch (const vm::exceptions::execution_unavailable&) {
            unavailable = true;
         }
         require(unavailable, "Physical stack shortfall did not remain operational");
         require(state.marked > 0 && state.marked < 1023 && state.consumed == 0,
                 "Physical stack fixture did not fail during deep native recursion");
      } else if (test == "deadline") {
         const auto result = invoke(spin->function_index, {}, std::chrono::steady_clock::now() + 25ms);
         require(result.trap == oc::trap_reason::deadline, "Infinite AOT loop ignored absolute deadline");
      } else if (test == "cancel" || test == "host_error") {
         auto stop = std::stop_source{};
         state.stop = &stop;
         state.fail = test == "host_error";
         const auto result =
             invoke(heavy->function_index, shallow, std::chrono::steady_clock::now() + 5s, stop.get_token());
         if (state.fail) {
            require(result.trap == oc::trap_reason::host_exception && result.host_error,
                    "Racing cancellation replaced host exception");
            try {
               std::rethrow_exception(result.host_error);
            } catch (const host_failure& error) {
               require(error.identity == &state, "Typed host exception identity changed");
            }
         } else
            require(result.trap == oc::trap_reason::cancelled, "AOT cancellation ignored");
         state.stop = nullptr;
         state.fail = false;
      } else if (test == "exported_import") {
         const auto result = invoke(imported->function_index, {}, std::chrono::steady_clock::now() + 5s);
         require(result.trap == oc::trap_reason::none && result.value && result.value->to_ui64() == state.random,
                 "Exported import lost its raw numeric ABI");
      } else
         throw std::runtime_error("Unknown optimizing test");
      state = host{};
      const auto reused = invoke(heavy->function_index, shallow, std::chrono::steady_clock::now() + 5s);
      require(reused.trap == oc::trap_reason::none && state.consumed == locals &&
                  state.consumed_sum == state.issued_sum,
              "AOT executor was not reusable after its prior invocation");
      std::cout << "PASS " << test << " source_budget=" << budget << " live_native_frame=" << native_frame
                << " logical_depth_limit=1024 fenv=preserved reuse=passed\n";
   } catch (...) {
      failure = std::current_exception();
   }
   executor.release_thread();
   forge::asio::blocking::run(runtime, executor.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   if (failure)
      std::rethrow_exception(failure);
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 5 && argc != 7)
         return 2;
      const auto request = oc::compile_request::decode(read(argv[3]), 16 * 1024 * 1024);
      const auto bytes = read(argv[2]);
      const auto identity = oc::identify(request);
      const auto code = oc::artifact::decode(bytes, identity, 32 * 1024 * 1024);
      if (std::string(argv[1]) == "identity") {
         auto corrupt = bytes;
         corrupt.front() ^= 0x80;
         auto rejected = false;
         try {
            static_cast<void>(oc::artifact::decode(corrupt, identity, 32 * 1024 * 1024));
         } catch (const oc::exceptions::invalid_artifact&) {
            rejected = true;
         }
         require(rejected, "Foreign artifact magic accepted");
         for (const auto version : {3u, 4u}) {
            auto foreign = request;
            foreign.configuration.target.codegen_version = version;
            rejected = false;
            try {
               static_cast<void>(oc::identify(foreign));
            } catch (const oc::exceptions::unsupported_target&) {
               rejected = true;
            }
            require(rejected, "Legacy codegen identity accepted");
         }
         auto foreign = request;
         foreign.configuration.target.engine += "-foreign";
         rejected = false;
         try {
            static_cast<void>(oc::identify(foreign));
         } catch (const oc::exceptions::unsupported_target&) {
            rejected = true;
         }
         require(rejected, "Foreign engine/capacity identity accepted");
         foreign = request;
         --foreign.configuration.target.host_abi_version;
         rejected = false;
         try {
            static_cast<void>(oc::identify(foreign));
         } catch (const oc::exceptions::unsupported_target&) {
            rejected = true;
         }
         require(rejected, "Removed OC machine ABI identity accepted");
         rejected = false;
         try {
            code.prepare();
         } catch (const oc::exceptions::invalid_artifact&) {
            rejected = true;
         }
         require(rejected, "Serialization improperly retained executable provenance");
         if (argc == 7) {
            for (const auto* substituted : {argv[5], argv[6]}) {
               const auto encoded = read(substituted);
               auto payload = code.image();
               // The donor artifact has its own source hash. Decode only to
               // obtain same-shape native bytes; no provenance is asserted.
               const auto original_request =
                   oc::compile_request::decode(read((std::string(substituted) + ".request").c_str()), 16 * 1024 * 1024);
               const auto donor = oc::artifact::decode(encoded, oc::identify(original_request), 32 * 1024 * 1024);
               require(payload.aot != donor.image().aot, "Substitution fixture did not change AOT bytes");
               payload.aot = donor.image().aot;
               const auto rewritten = oc::artifact::from_payload(std::move(payload)).encode();
               const auto attacker = oc::artifact::decode(rewritten, identity, 32 * 1024 * 1024);
               rejected = false;
               try {
                  attacker.prepare();
               } catch (const oc::exceptions::invalid_artifact&) {
                  rejected = true;
               }
               require(rejected, "Same-shape foreign AOT gained execution authority by recomputing its checksum");
            }
         }
         std::cout << "PASS artifact identity, legacy OC and foreign engine rejection\n";
      } else
         run(argv[1], oc::artifact::decode(bytes, identity, 32 * 1024 * 1024, oc::artifact::origin::isolated_compiler),
             static_cast<unsigned>(std::stoul(argv[4])));
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed error\n";
      return 1;
   }
}
