#include "test_prelude.hpp"
#include "optimizing_fixture_export.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <cerrno>
#include <fcntl.h>
#include <boost/scope/scope_exit.hpp>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if (defined(__linux__) && defined(__x86_64__)) || (defined(__APPLE__) && defined(__aarch64__))
#include <sys/wait.h>
#include <unistd.h>
#endif

#if (defined(__linux__) && defined(__x86_64__)) || (defined(__APPLE__) && defined(__aarch64__))
#include "fresh_process.hpp"
#endif

import forge.tooling.wasm_compiler.compiler;
import forge.raw.raw;
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.exceptions;

namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
namespace tooling = forge::tooling::wasm_compiler;

namespace {
void write_fixture_file(std::string_view name, std::string_view suffix, std::span<const std::uint8_t> bytes) {
   const auto* configured = std::getenv("FORGE_OC_FIXTURE_EXPORT_DIRECTORY");
   BOOST_REQUIRE_MESSAGE(configured != nullptr, "explicit fixture output directory is required");
   const auto directory = std::filesystem::path{configured};
   BOOST_REQUIRE(directory.is_absolute());
   BOOST_REQUIRE(std::filesystem::is_directory(directory));
   BOOST_REQUIRE(name.find('/') == name.npos);
   const auto path = directory / (std::string{name} + std::string{suffix});
   // Never overwrite immutable fixture evidence on a second invocation.
   const auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   BOOST_REQUIRE_MESSAGE(fd >= 0, "fixture evidence already exists or cannot be created: " << path);
   const auto close_file = boost::scope::scope_exit{[fd] { ::close(fd); }};
   auto offset = std::size_t{};
   while (offset < bytes.size()) {
      const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR)
         continue;
      BOOST_REQUIRE(count > 0);
      offset += static_cast<std::size_t>(count);
   }
   BOOST_REQUIRE(::fsync(fd) == 0);
}
} // namespace

void optimizing_fixture_export::write(std::string_view name, std::span<const std::uint8_t> request,
                                      std::span<const std::uint8_t> artifact) {
   write_fixture_file(name, ".request", request);
   write_fixture_file(name, ".foc", artifact);
}

void optimizing_fixture_export::write_diagnostic(std::string_view name, std::string_view extension,
                                                 std::span<const std::uint8_t> bytes) {
   BOOST_REQUIRE(extension == "ll" || extension == "o");
   write_fixture_file(name, "." + std::string{extension}, bytes);
}

namespace {
using bytes = std::vector<std::uint8_t>;
void append_u32(bytes& output, std::uint32_t value) {
   do {
      const auto part = static_cast<std::uint8_t>(value & 127);
      value >>= 7;
      output.push_back(part | (value ? 128 : 0));
   } while (value);
}
void append_section(bytes& output, std::uint8_t id, const bytes& body) {
   output.push_back(id);
   append_u32(output, body.size());
   output.insert(output.end(), body.begin(), body.end());
}
oc::compile_request integer_request(std::uint8_t operation = 0x6a) {
   auto request = oc::compile_request{};
   request.configuration.execution = vm::resolve_options(vm::default_options{});
   request.configuration.limits = {1024 * 1024, 16384};
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   append_section(wasm, 1, {3, 0x60, 2, 0x7f, 0x7f, 1, 0x7f, 0x60, 2, 0x7e, 0x7e, 1, 0x7e, 0x60, 0, 0});
   append_section(wasm, 3, {4, 0, 0, 1, 2});
   auto exports = bytes{4};
   const auto names = std::array<std::string_view, 4>{"add", "relay", "wide", "empty"};
   for (std::uint32_t i = 0; i < names.size(); ++i) {
      append_u32(exports, names[i].size());
      exports.insert(exports.end(), names[i].begin(), names[i].end());
      exports.push_back(0);
      append_u32(exports, i);
   }
   append_section(wasm, 7, exports);
   append_section(wasm, 8, {3});
   auto code = bytes{4};
   for (const auto& body : std::vector<bytes>{{0, 0x20, 0, 0x20, 1, operation, 0x0b},
                                              {0, 0x20, 0, 0x20, 1, 0x10, 0, 0x0b},
                                              {0, 0x20, 0, 0x20, 1, 0x7c, 0x0b},
                                              {0, 0x0b}}) {
      append_u32(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
   }
   append_section(wasm, 10, code);
   return request;
}

oc::compile_request functions_request(const bytes& parameters, std::uint8_t result, const std::vector<bytes>& bodies,
                                      bool memory = false, const bytes& globals = {}, const bytes& data = {},
                                      std::uint32_t initial_pages = 1, std::uint32_t maximum_pages = 3) {
   auto request = integer_request();
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   auto types = bytes{1, 0x60};
   append_u32(types, parameters.size());
   types.insert(types.end(), parameters.begin(), parameters.end());
   types.push_back(result ? 1 : 0);
   if (result)
      types.push_back(result);
   append_section(wasm, 1, types);
   auto functions = bytes{};
   append_u32(functions, bodies.size());
   functions.resize(functions.size() + bodies.size(), 0);
   append_section(wasm, 3, functions);
   if (memory) {
      auto limits = bytes{1, 1};
      append_u32(limits, initial_pages);
      append_u32(limits, maximum_pages);
      append_section(wasm, 5, limits);
   }
   if (!globals.empty())
      append_section(wasm, 6, globals);
   auto exports = bytes{};
   auto code = bytes{};
   append_u32(exports, bodies.size());
   append_u32(code, bodies.size());
   for (std::uint32_t i = 0; i < bodies.size(); ++i) {
      const auto name = "f" + std::to_string(i);
      append_u32(exports, name.size());
      exports.insert(exports.end(), name.begin(), name.end());
      exports.push_back(0);
      append_u32(exports, i);
      append_u32(code, bodies[i].size());
      code.insert(code.end(), bodies[i].begin(), bodies[i].end());
   }
   append_section(wasm, 7, exports);
   append_section(wasm, 10, code);
   if (!data.empty())
      append_section(wasm, 11, data);
   return request;
}

oc::compile_request finite_stack_request(std::uint32_t parameters = 1024) {
   BOOST_REQUIRE(parameters > 0 && parameters <= 2032);
   // One SAME function signature for the three finite entries. Reversed live
   // arguments prevent a memcpy/tail-call shortcut from hiding actual frames.
   auto forward = bytes{0};
   for (auto index = parameters; index > 0; --index) {
      forward.push_back(0x20);
      append_u32(forward, index - 1);
   }
   auto first = forward;
   first.insert(first.end(), {0x10, 1, 0x41, 1, 0x6a, 0x0b});
   auto second = forward;
   second.insert(second.end(), {0x10, 2, 0x41, 1, 0x6a, 0x0b});
   auto last = bytes{0};
   for (std::uint32_t index = 0; index < parameters; ++index) {
      last.push_back(0x20);
      append_u32(last, index);
   }
   // Existing shared numeric callback is an opaque spill site only. The live
   // i64 values were loaded BEFORE it and must survive it. No LLVM FP math or
   // test arithmetic helper; the conversion result is deliberately discarded.
   last.insert(last.end(), {0x41, 1, static_cast<std::uint8_t>(vm::f32_convert_s_i32), 0x1a});
   for (std::uint32_t index = 1; index < parameters; ++index)
      last.push_back(0x7c); // i64.add reduction of the preserved operand stack
   last.insert(last.end(), {0xa7, 0x41, 40, 0x6a, 0x0b}); // wrap +40; two callers add1
   return functions_request(bytes(parameters, 0x7e), 0x7f, {first, second, last});
}

oc::compile_request recursive_request() {
   // Genuine finite recursion through SAME function builder. One immutable
   // module/configuration: f0(250) returns42 at251 active frames; f0(251)
   // reaches the NEXT real entry with canonical depth251. No artifact/context
   // mutation and no compiler-side native execution of this fixture.
   auto request = functions_request({0x7f}, 0x7f,
       {{0, 0x20, 0, 0x45, 0x04, 0x7f, 0x41, 42, 0x05,
         0x20, 0, 0x41, 1, 0x6b, 0x10, 0, 0x0b, 0x0b}});
   request.configuration.execution.max_call_depth = 251;
   return request;
}

enum class benchmark_workload { integer_mix, memory_sweep };
constexpr auto benchmark_arguments =
    std::array<std::uint64_t, 3>{0x345c850000000000ULL, 0x345c850000000000ULL, 1}; // receiver/code "alice", raw action1
constexpr std::uint64_t benchmark_multiplier = 0x9e3779b185ebca87ULL;

void append_signed_constant(bytes& output, std::uint8_t operation, std::uint64_t bits) {
   output.push_back(operation);
   auto value = std::bit_cast<std::int64_t>(bits);
   for (;;) {
      auto part = static_cast<std::uint8_t>(value) & 127U;
      const auto sign = (part & 64U) != 0;
      value >>= 7;
      if ((value == 0 && !sign) || (value == -1 && sign)) {
         output.push_back(part);
         return;
      }
      output.push_back(part | 128U);
   }
}

oc::compile_request benchmark_request(benchmark_workload selected) {
   // One owning byte factory. Other engines consume these exact exported bytes;
   // no CDT contract, second parser, target-specific body or host implementation.
   auto request = integer_request();
   request.configuration.imports = {{"env", "printui", {{vm::i64}, {}}}};
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   append_section(wasm, 1, {2, 0x60, 1, 0x7e, 0, 0x60, 3, 0x7e, 0x7e, 0x7e, 0});
   append_section(wasm, 2, {1, 3, 'e', 'n', 'v', 7, 'p', 'r', 'i', 'n', 't', 'u', 'i', 0, 0});
   append_section(wasm, 3, {1, 1});
   if (selected == benchmark_workload::memory_sweep)
      append_section(wasm, 5, {1, 1, 1, 1}); // exactly one64KiB page, no growth
   append_section(wasm, 7, {1, 5, 'a', 'p', 'p', 'l', 'y', 0, 1});
   // Parameters0..2; local3 index,4 pass,5 checksum,6 next memory word.
   auto body = bytes{2, 2, 0x7f, 2, 0x7e, 0x20, 0, 0x20, 1};
   const auto i32 = [&](std::uint32_t value) { append_signed_constant(body, 0x41, value); };
   const auto i64 = [&](std::uint64_t value) { append_signed_constant(body, 0x42, value); };
   const auto begin_loop = [&](std::uint8_t index, std::uint32_t bound) {
      body.insert(body.end(), {0x02, 0x40, 0x03, 0x40, 0x20, index});
      i32(bound);
      body.insert(body.end(), {0x4f, 0x0d, 1}); // unsigned >= bound: leave block
   };
   const auto end_loop = [&](std::uint8_t index) {
      body.insert(body.end(), {0x20, index, 0x41, 1, 0x6a, 0x21, index, 0x0c, 0, 0x0b, 0x0b});
   };
   i64(17);
   body.insert(body.end(), {0x89, 0x85, 0x20, 2, 0x85, 0x21, 5});
   if (selected == benchmark_workload::integer_mix) {
      begin_loop(3, 4096);
      body.insert(body.end(), {0x20, 5, 0x20, 3, 0xad});
      i64(1);
      body.insert(body.end(), {0x7c, 0x7c});
      i64(13);
      body.push_back(0x89);
      i64(benchmark_multiplier);
      body.insert(body.end(), {0x7e, 0x22, 5, 0x20, 5});
      i64(7);
      body.insert(body.end(), {0x88, 0x85, 0x21, 5});
      end_loop(3);
   } else {
      begin_loop(3, 8192);
      body.insert(body.end(), {0x20, 3, 0x41, 3, 0x74, 0x20, 5, 0x20, 3, 0xad, 0x85, 0x37, 3, 0});
      end_loop(3);
      begin_loop(4, 8);
      body.insert(body.end(), {0x41, 0, 0x21, 3});
      begin_loop(3, 8192);
      // Preserve store address, read actual earlier-pass memory, then update.
      body.insert(body.end(),
                  {0x20, 3, 0x41, 3, 0x74, 0x20, 3, 0x41, 3, 0x74, 0x29, 3, 0, 0x20, 5, 0x7c, 0x20, 3, 0xad, 0x7c});
      i64(13);
      body.insert(body.end(), {0x89, 0x20, 4, 0xad});
      i64(1);
      body.insert(body.end(), {0x7c, 0x85, 0x22, 6, 0x37, 3, 0, 0x20, 5, 0x20, 6, 0x85, 0x21, 5});
      end_loop(3);
      end_loop(4);
   }
   body.insert(body.end(), {0x20, 5, 0x10, 0, 0x0b}); // the ONLY host effect
   auto code = bytes{1};
   append_u32(code, body.size());
   code.insert(code.end(), body.begin(), body.end());
   append_section(wasm, 10, code);
   return request;
}

std::uint64_t benchmark_reference(benchmark_workload selected, const std::array<std::uint64_t, 3>& arguments) {
   auto checksum = arguments[0] ^ std::rotl(arguments[1], 17) ^ arguments[2];
   if (selected == benchmark_workload::integer_mix) {
      for (std::uint64_t index = 0; index < 4096; ++index) {
         checksum = std::rotl(checksum + index + 1, 13) * benchmark_multiplier;
         checksum ^= checksum >> 7;
      }
   } else {
      auto memory = std::array<std::uint64_t, 8192>{};
      for (std::size_t index = 0; index < memory.size(); ++index)
         memory[index] = checksum ^ index;
      for (std::uint64_t pass = 0; pass < 8; ++pass) {
         for (std::size_t index = 0; index < memory.size(); ++index) {
            memory[index] = std::rotl(memory[index] + checksum + index, 13) ^ (pass + 1);
            checksum ^= memory[index];
         }
      }
   }
   return checksum;
}

struct benchmark_host {
   std::uint64_t checksum = 0;
   unsigned calls = 0;
   void printui(std::uint64_t value) {
      checksum = value;
      ++calls;
   }
};

void require_benchmark_checksum(const benchmark_host& host, std::uint64_t expected) {
   if (host.calls != 1 || host.checksum != expected)
      throw std::runtime_error{"benchmark requires exactly one expected checksum effect"};
}

benchmark_host benchmark_baseline(const oc::compile_request& request, const std::array<std::uint64_t, 3>& arguments) {
   using registry = vm::registered_host_functions<benchmark_host>;
   static const auto registered = [] {
      registry::add<&benchmark_host::printui>("env", "printui");
      return true;
   }();
   static_cast<void>(registered);
   auto input = request.wasm;
   auto memory = vm::wasm_allocator{};
   auto instance = vm::backend<registry, vm::interpreter, vm::options>{input, &memory, request.configuration.execution};
   auto host = benchmark_host{};
   BOOST_REQUIRE(instance.call(host, "env", "apply", arguments[0], arguments[1], arguments[2]));
   return host;
}

oc::compile_request dense_branch_table_request() {
   auto body = bytes{0};
   for (unsigned i = 0; i < 16; ++i)
      body.insert(body.end(), {0x02, 0x40});
   body.insert(body.end(), {0x20, 0, 0x0e, 16});
   for (std::uint8_t i = 0; i < 16; ++i)
      body.push_back(i);
   body.push_back(15);
   const auto operations = std::array{vm::i32_add, vm::i32_xor, vm::i32_mul, vm::i32_rem_u,
                                      vm::i32_shl, vm::i32_rotr, vm::i32_or, vm::i32_and};
   for (unsigned i = 0; i < 16; ++i)
      body.insert(body.end(), {0x0b, 0x20, 1, 0x41, static_cast<std::uint8_t>(i + 3),
                              static_cast<std::uint8_t>(operations[i % operations.size()]), 0x0f});
   body.push_back(0x0b);
   return functions_request({0x7f, 0x7f}, 0x7f, {body});
}

oc::compile_request epoch_import_loop_request() {
   auto request = integer_request();
   request.configuration.imports = {{"env", "tick", {{}, {}}}};
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   append_section(wasm, 1, {2, 0x60, 0, 0, 0x60, 1, 0x7f, 1, 0x7f});
   append_section(wasm, 2, {1, 3, 'e', 'n', 'v', 4, 't', 'i', 'c', 'k', 0, 0});
   append_section(wasm, 3, {1, 1});
   append_section(wasm, 7, {1, 2, 'f', '0', 0, 1});
   const auto body = bytes{0, 0x02, 0x40, 0x03, 0x40, 0x20, 0, 0x45, 0x0d, 1, 0x10, 0,
                            0x20, 0, 0x41, 1, 0x6b, 0x21, 0, 0x0c, 0, 0x0b, 0x0b, 0x20, 0, 0x0b};
   auto code = bytes{1};
   append_u32(code, body.size());
   code.insert(code.end(), body.begin(), body.end());
   append_section(wasm, 10, code);
   return request;
}

struct numeric_case {
   vm::opcodes operation;
   vm::value_type input;
   vm::value_type output;
   unsigned arity;
};

std::vector<numeric_case> numeric_cases() {
   auto cases = std::vector<numeric_case>{};
   for (const bool wide : {false, true}) {
      const auto type = static_cast<vm::value_type>(wide ? vm::f64 : vm::f32);
      for (auto operation = wide ? vm::f64_eq : vm::f32_eq; operation <= (wide ? vm::f64_ge : vm::f32_ge);
           operation = static_cast<vm::opcodes>(operation + 1))
         cases.push_back({operation, type, vm::i32, 2});
      for (auto operation = wide ? vm::f64_abs : vm::f32_abs;
           operation <= (wide ? vm::f64_copysign : vm::f32_copysign);
           operation = static_cast<vm::opcodes>(operation + 1))
         cases.push_back({operation, type, type, operation >= (wide ? vm::f64_add : vm::f32_add) ? 2U : 1U});
   }
   for (const auto& entry : std::to_array<numeric_case>({
          numeric_case{vm::i32_trunc_s_f32, vm::f32, vm::i32, 1}, {vm::i32_trunc_u_f32, vm::f32, vm::i32, 1},
          {vm::i32_trunc_s_f64, vm::f64, vm::i32, 1}, {vm::i32_trunc_u_f64, vm::f64, vm::i32, 1},
          {vm::i64_trunc_s_f32, vm::f32, vm::i64, 1}, {vm::i64_trunc_u_f32, vm::f32, vm::i64, 1},
          {vm::i64_trunc_s_f64, vm::f64, vm::i64, 1}, {vm::i64_trunc_u_f64, vm::f64, vm::i64, 1},
          {vm::f32_convert_s_i32, vm::i32, vm::f32, 1}, {vm::f32_convert_u_i32, vm::i32, vm::f32, 1},
          {vm::f32_convert_s_i64, vm::i64, vm::f32, 1}, {vm::f32_convert_u_i64, vm::i64, vm::f32, 1},
          {vm::f64_convert_s_i32, vm::i32, vm::f64, 1}, {vm::f64_convert_u_i32, vm::i32, vm::f64, 1},
          {vm::f64_convert_s_i64, vm::i64, vm::f64, 1}, {vm::f64_convert_u_i64, vm::i64, vm::f64, 1},
          {vm::f32_demote_f64, vm::f64, vm::f32, 1}, {vm::f64_promote_f32, vm::f32, vm::f64, 1},
          {vm::i32_reinterpret_f32, vm::f32, vm::i32, 1}, {vm::i64_reinterpret_f64, vm::f64, vm::i64, 1},
          {vm::f32_reinterpret_i32, vm::i32, vm::f32, 1}, {vm::f64_reinterpret_i64, vm::i64, vm::f64, 1}}))
      cases.push_back(entry);
   return cases;
}

oc::compile_request numeric_request(const numeric_case& fixture) {
   const auto input = static_cast<std::uint8_t>(fixture.input);
   auto parameters = bytes(fixture.arity, input);
   // A typed float/integer local verifies the raw transport path, not merely
   // direct raw arguments. Relay also exercises generated-to-generated calls.
   auto body = bytes{1, 1, input, 0x20, 0, 0x22, static_cast<std::uint8_t>(fixture.arity)};
   auto relay = bytes{0, 0x20, 0};
   if (fixture.arity == 2) {
      body.insert(body.end(), {0x20, 1});
      relay.insert(relay.end(), {0x20, 1});
   }
   body.insert(body.end(), {static_cast<std::uint8_t>(fixture.operation), 0x0b});
   relay.insert(relay.end(), {0x10, 0, 0x0b});
   return functions_request(parameters, static_cast<std::uint8_t>(fixture.output), {body, relay});
}

oc::compile_request import_indirect_request(std::uint8_t expected_type = 1, std::uint8_t reserved = 0) {
   auto request = integer_request();
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   // Types 0/1 are structurally identical but separately declared. Type 3
   // deliberately differs and supplies a wrong-signature table destination.
   append_section(wasm, 1, {4, 0x60, 2, 0x7f, 0x7f, 1, 0x7f,
                              0x60, 2, 0x7f, 0x7f, 1, 0x7f,
                              0x60, 3, 0x7f, 0x7f, 0x7f, 1, 0x7f, 0x60, 0, 0});
   // Module order z,a differs from the canonical configuration ordinal a,z.
   append_section(wasm, 2, {2, 3, 'e', 'n', 'v', 1, 'z', 0, 1,
                              3, 'e', 'n', 'v', 1, 'a', 0, 0});
   append_section(wasm, 3, {5, 0, 2, 1, 0, 3});
   append_section(wasm, 4, {1, 0x70, 0, 5});
   auto exports = bytes{7};
   for (std::uint32_t i = 0; i < 7; ++i) {
      const auto name = "f" + std::to_string(i);
      append_u32(exports, name.size());
      exports.insert(exports.end(), name.begin(), name.end());
      exports.insert(exports.end(), {0, static_cast<std::uint8_t>(i)});
   }
   append_section(wasm, 7, exports);
   // Three mixed destinations, a null slot, then a different signature.
   append_section(wasm, 9, {2, 0, 0x41, 0, 0x0b, 3, 0, 1, 2,
                              0, 0x41, 4, 0x0b, 1, 6});
   auto code = bytes{5};
   for (const auto& body : std::vector<bytes>{
          {0, 0x20, 0, 0x20, 1, 0x6b, 0x0b},
          {0, 0x20, 0, 0x20, 1, 0x20, 2, 0x11, expected_type, reserved, 0x0b},
          {0, 0x20, 0, 0x20, 1, 0x10, 0, 0x0b},
          {0, 0x20, 0, 0x20, 1, 0x10, 1, 0x0b}, {0, 0x0b}}) {
      append_u32(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
   }
   append_section(wasm, 10, code);
   const auto signature = vm::host_function{{vm::i32, vm::i32}, {vm::i32}};
   request.configuration.imports = {{"env", "z", signature}, {"env", "a", signature}};
   return request;
}

oc::compile_request imported_entry_request(const bytes& parameters, std::uint8_t result, bool start = false) {
   auto request = integer_request();
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   auto type = bytes{1, 0x60};
   append_u32(type, parameters.size());
   type.insert(type.end(), parameters.begin(), parameters.end());
   type.push_back(result ? 1 : 0);
   if (result)
      type.push_back(result);
   append_section(wasm, 1, type);
   append_section(wasm, 2, {1, 3, 'e', 'n', 'v', 1, 'a', 0, 0});
   append_section(wasm, 7, {1, 2, 'f', '0', 0, 0});
   if (start)
      append_section(wasm, 8, {0});
   auto signature = vm::host_function{};
   signature.params.assign(parameters.begin(), parameters.end());
   if (result)
      signature.ret.push_back(result);
   request.configuration.imports = {{"env", "a", std::move(signature)}};
   return request;
}

bytes memory_data() {
   // These bytes distinguish offsets, little endian and signed extension.
   const auto content = bytes{0xa5, 0x39, 0x12, 0x80, 0xff, 0x45, 0x23, 0x81, 0xfe, 0xdc, 0xba, 0x98};
   auto data = bytes{1, 0, 0x41, 0, 0x0b};
   append_u32(data, content.size());
   data.insert(data.end(), content.begin(), content.end());
   return data;
}

struct memory_load_case {
   vm::opcodes operation;
   std::uint32_t width;
   bool wide;
   bool sign;
};
const auto memory_loads =
    std::array{memory_load_case{vm::i32_load, 4, false, false},    memory_load_case{vm::i64_load, 8, true, false},
               memory_load_case{vm::i32_load8_s, 1, false, true},  memory_load_case{vm::i32_load8_u, 1, false, false},
               memory_load_case{vm::i32_load16_s, 2, false, true}, memory_load_case{vm::i32_load16_u, 2, false, false},
               memory_load_case{vm::i64_load8_s, 1, true, true},   memory_load_case{vm::i64_load8_u, 1, true, false},
               memory_load_case{vm::i64_load16_s, 2, true, true},  memory_load_case{vm::i64_load16_u, 2, true, false},
               memory_load_case{vm::i64_load32_s, 4, true, true},  memory_load_case{vm::i64_load32_u, 4, true, false}};

oc::compile_request load_request(const memory_load_case& load, std::uint32_t offset = 3, std::uint32_t alignment = 0,
                                 std::uint32_t initial_pages = 1) {
   auto body = bytes{0, 0x20, 0, static_cast<std::uint8_t>(load.operation)};
   append_u32(body, alignment);
   append_u32(body, offset);
   body.push_back(0x0b);
   auto request = functions_request({0x7f}, load.wide ? 0x7e : 0x7f, {body}, true, {},
                                    initial_pages ? memory_data() : bytes{}, initial_pages);
   // Overflow fixtures deliberately resolve the existing offset option to
   // its uint32 maximum. Production/default acceptance is not widened.
   request.configuration.execution.max_memory_offset =
       std::max(request.configuration.execution.max_memory_offset, offset);
   return request;
}

std::uint64_t loaded_bits(const memory_load_case& load) {
   const auto data = memory_data();
   auto bits = std::uint64_t{0};
   for (std::uint32_t i = 0; i < load.width; ++i)
      bits |= std::uint64_t{data[data.size() - 12 + 3 + i]} << (8 * i);
   if (load.sign && (bits & (std::uint64_t{1} << (load.width * 8 - 1))))
      bits |= UINT64_MAX << (load.width * 8);
   return load.wide ? bits : static_cast<std::uint32_t>(bits);
}

struct memory_store_case {
   vm::opcodes operation;
   vm::opcodes load;
   std::uint32_t width;
   bool wide;
};
const auto memory_stores = std::array{memory_store_case{vm::i32_store, vm::i32_load, 4, false},
                                      memory_store_case{vm::i64_store, vm::i64_load, 8, true},
                                      memory_store_case{vm::i32_store8, vm::i32_load8_u, 1, false},
                                      memory_store_case{vm::i32_store16, vm::i32_load16_u, 2, false},
                                      memory_store_case{vm::i64_store8, vm::i64_load8_u, 1, true},
                                      memory_store_case{vm::i64_store16, vm::i64_load16_u, 2, true},
                                      memory_store_case{vm::i64_store32, vm::i64_load32_u, 4, true}};

oc::compile_request store_request(const memory_store_case& store) {
   return functions_request({0x7f, static_cast<std::uint8_t>(store.wide ? 0x7e : 0x7f)}, store.wide ? 0x7e : 0x7f,
                            {{0, 0x20, 0, 0x20, 1, static_cast<std::uint8_t>(store.operation), 0, 3, 0x20, 0,
                              static_cast<std::uint8_t>(store.load), 0, 3, 0x0b}},
                            true);
}

oc::compile_request float_bits_request(bool wide) {
   const auto load = static_cast<std::uint8_t>(wide ? vm::f64_load : vm::f32_load);
   const auto store = static_cast<std::uint8_t>(wide ? vm::f64_store : vm::f32_store);
   const auto integer_load = static_cast<std::uint8_t>(wide ? vm::i64_load : vm::i32_load);
   auto globals =
       bytes{1, static_cast<std::uint8_t>(wide ? 0x7c : 0x7d), 1, static_cast<std::uint8_t>(wide ? 0x44 : 0x43)};
   // A quiet-NaN payload is preserved, not normalized by native FP arithmetic.
   for (const auto byte : wide ? bytes{0x45, 0x23, 1, 0, 0, 0, 0xf8, 0x7f} : bytes{0x45, 0x23, 0xc1, 0x7f})
      globals.push_back(byte);
   globals.push_back(0x0b);
   return functions_request(
       {0x7f, 0x7f}, wide ? 0x7e : 0x7f,
       {{0, 0x20, 1, 0x20, 0, load, 0, 3, store, 0, 0, 0x20, 1, integer_load, 0, 0, 0x0b},
        {0, 0x20, 0, load, 0, 3, 0x24, 0, 0x20, 1, 0x23, 0, store, 0, 0, 0x20, 1, integer_load, 0, 0, 0x0b},
        {0, 0x20, 1, 0x23, 0, store, 0, 0, 0x20, 1, integer_load, 0, 0, 0x0b}},
       true, globals, memory_data());
}

oc::compile_request globals_request(bool wide, bool mutable_global = true) {
   auto globals = bytes{1,
                        static_cast<std::uint8_t>(wide ? 0x7e : 0x7f),
                        static_cast<std::uint8_t>(mutable_global),
                        static_cast<std::uint8_t>(wide ? 0x42 : 0x41),
                        7,
                        0x0b};
   return functions_request({static_cast<std::uint8_t>(wide ? 0x7e : 0x7f)}, wide ? 0x7e : 0x7f,
                            {{0, 0x23, 0, 0x0b}, {0, 0x20, 0, 0x24, 0, 0x23, 0, 0x0b}}, false, globals);
}

oc::compile_request growth_request(std::uint32_t initial_pages = 1, std::uint32_t maximum_pages = 3) {
   return functions_request({0x7f}, 0x7f,
                            {{0, 0x3f, 0, 0x0b},
                             {0, 0x20, 0, 0x40, 0, 0x0b},
                             // Growth returns before this load: a relocated base must be reloaded.
                             {0, 0x20, 0, 0x40, 0, 0x1a, 0x41, 0, 0x28, 2, 3, 0x0b},
                             // A base observed before a generated-to-generated call must not survive it.
                             {0, 0x41, 0, 0x28, 2, 3, 0x1a, 0x20, 0, 0x10, 1, 0x1a, 0x41, 0, 0x28, 2, 3, 0x0b}},
                            true, {}, initial_pages ? memory_data() : bytes{}, initial_pages, maximum_pages);
}

oc::compile_request start_initialization_request() {
   auto request = integer_request();
   auto& wasm = request.wasm;
   wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   append_section(wasm, 1, {2, 0x60, 0, 0, 0x60, 0, 1, 0x7f});
   append_section(wasm, 3, {2, 0, 1});
   append_section(wasm, 5, {1, 1, 1, 3});
   append_section(wasm, 6, {1, 0x7f, 1, 0x41, 7, 0x0b});
   append_section(wasm, 7, {1, 2, 'f', '0', 0, 1});
   append_section(wasm, 8, {0});
   auto code = bytes{2};
   for (const auto& body : std::vector<bytes>{{0, 0x41, 11, 0x24, 0, 0x41, 0, 0x41, 11, 0x36, 0, 0, 0x0b},
                                              {0, 0x23, 0, 0x41, 0, 0x28, 0, 0, 0x6a, 0x0b}}) {
      append_u32(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
   }
   append_section(wasm, 10, code);
   append_section(wasm, 11, memory_data());
   return request;
}

template <typename T>
std::uint64_t baseline_one(const oc::compile_request& request, std::string_view name, T argument) {
   auto input = request.wasm;
   auto memory = vm::wasm_allocator{};
   auto instance =
       vm::backend<std::nullptr_t, vm::interpreter, vm::options>{input, &memory, request.configuration.execution};
   const auto result = instance.call_with_return("env", name, argument);
   return result->template is_a<vm::i32_const_t>() ? result->to_ui32() : result->to_ui64();
}

oc::compile_request control_flow_request() {
   return functions_request(
       {0x7f, 0x7f}, 0x7f,
       {
           {0, 0x20, 0, 0x04, 0x7f, 0x20, 1, 0x05, 0x41, 9, 0x0b, 0x0b},
           // The preserved result must move over one discarded temporary.
           {0, 0x02, 0x7f, 0x41, 0x63, 0x41, 12, 0x20, 0, 0x0d, 0, 0x1a, 0x0b, 0x0b},
           // br_table has two distinct result-carrying destinations + default.
           {0,    0x02, 0x7f, 0x02, 0x7f, 0x41, 0x63, 0x41, 12,   0x20, 0,
            0x0e, 2,    0,    1,    1,    0x0b, 0x41, 1,    0x6a, 0x0b, 0x0b},
           // Sum down to zero; loop labels have no preserved result.
           {1,    1, 0x7f, 0x02, 0x40, 0x03, 0x40, 0x20, 0, 0x45, 0x0d, 1,    0x20, 2,    0x20, 0,   0x6a,
            0x21, 2, 0x20, 0,    0x41, 1,    0x6b, 0x21, 0, 0x0c, 0,    0x0b, 0x0b, 0x20, 2,    0x0b},
           {0, 0x41, 0x63, 0x20, 1, 0x0f, 0x0b},
           {0, 0x20, 0, 0x20, 1, 0x20, 0, 0x1b, 0x0b},
           // Empty table: the in-place payload still includes its default.
           {0, 0x02, 0x7f, 0x41, 12, 0x20, 0, 0x0e, 0, 0, 0x0b, 0x0b},
       });
}

template <typename T>
std::uint64_t baseline(const oc::compile_request& request, std::string_view name, T left, T right) {
   auto input = request.wasm;
   auto memory = vm::wasm_allocator{};
   auto instance =
       vm::backend<std::nullptr_t, vm::interpreter, vm::options>{input, &memory, request.configuration.execution};
   const auto result = instance.call_with_return("env", name, left, right);
   return result->template is_a<vm::i32_const_t>() ? result->to_ui32() : result->to_ui64();
}
} // namespace

std::vector<std::uint8_t> optimizing_fixture_export::finite_request(std::uint32_t parameters, bool darwin) {
   auto request = finite_stack_request(parameters);
   if (darwin) {
      request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
      request.configuration.target.cpu = "generic";
      request.configuration.target.features = "+fp-armv8,+neon";
   }
   return request.encode();
}

BOOST_AUTO_TEST_SUITE(optimizing_compiler)

BOOST_AUTO_TEST_CASE(common_benchmark_corpus_checks_runtime_operands_and_one_host_effect) {
   for (const auto selected : {benchmark_workload::integer_mix, benchmark_workload::memory_sweep}) {
      const auto request = benchmark_request(selected);
      const auto expected = benchmark_reference(selected, benchmark_arguments);
      const auto golden =
          selected == benchmark_workload::integer_mix ? UINT64_C(15065454813286283065) : UINT64_C(11713105455123543422);
      BOOST_TEST(expected == golden);
      const auto observed = benchmark_baseline(request, benchmark_arguments);
      BOOST_CHECK_NO_THROW(require_benchmark_checksum(observed, expected));
      BOOST_CHECK_THROW(require_benchmark_checksum(observed, expected ^ 1U), std::runtime_error);
      auto changed = benchmark_arguments;
      changed[2] ^= 1U;
      const auto changed_expected = benchmark_reference(selected, changed);
      BOOST_TEST(expected != changed_expected);
      BOOST_CHECK_NO_THROW(require_benchmark_checksum(benchmark_baseline(request, changed), changed_expected));
      for (const auto darwin : {false, true}) {
         auto profile_request = request;
         if (darwin) {
            profile_request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
            profile_request.configuration.target.cpu = "generic";
            profile_request.configuration.target.features = "+fp-armv8,+neon";
         }
         const auto code = tooling::compiler{}.compile(profile_request);
         BOOST_REQUIRE(code.image().functions.size() == 2U);
         BOOST_REQUIRE(code.image().exports.size() == 1U);
         BOOST_TEST(code.image().exports[0].name == "apply");
         BOOST_TEST(code.image().exports[0].function_index == 1U);
         BOOST_TEST(code.image().memory.defined == (selected == benchmark_workload::memory_sweep));
         BOOST_TEST(profile_request.wasm == request.wasm);
         const auto encoded = code.encode();
         const auto checked = oc::artifact::decode(encoded, oc::identify(profile_request), encoded.size());
         BOOST_TEST(checked.encode() == encoded);
         BOOST_TEST(!checked.is_prepared()); // No native timing/execution acceptance in this owner.
      }
   }
}

BOOST_AUTO_TEST_CASE(export_checked_benchmark_corpus, *boost::unit_test::disabled()) {
   for (const auto selected : {benchmark_workload::integer_mix, benchmark_workload::memory_sweep}) {
      const auto name =
          std::string{selected == benchmark_workload::integer_mix ? "benchmark-integer" : "benchmark-memory"};
      const auto request = benchmark_request(selected);
      const auto expected = benchmark_reference(selected, benchmark_arguments);
      require_benchmark_checksum(benchmark_baseline(request, benchmark_arguments), expected);
      // Existing generic Raw std::array codec; fixed receiver/code/action/checksum,
      // no bespoke parser, JSON builder or production benchmark protocol.
      const auto inputs = std::array{benchmark_arguments[0], benchmark_arguments[1], benchmark_arguments[2], expected};
      const auto encoded_inputs = forge::raw::pack(inputs);
      BOOST_REQUIRE(encoded_inputs.size() == 32U);
      BOOST_CHECK((forge::raw::unpack_exact<std::array<std::uint64_t, 4>>(encoded_inputs) == inputs));
      write_fixture_file(name, ".wasm", request.wasm);
      write_fixture_file(name, ".inputs", encoded_inputs);
      for (const auto darwin : {false, true}) {
         auto profile_request = request;
         if (darwin) {
            profile_request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
            profile_request.configuration.target.cpu = "generic";
            profile_request.configuration.target.features = "+fp-armv8,+neon";
         }
         const auto code = tooling::compiler{}.compile(profile_request);
         const auto encoded_request = profile_request.encode();
         const auto encoded_artifact = code.encode();
         const auto checked =
             oc::artifact::decode(encoded_artifact, oc::identify(profile_request), encoded_artifact.size());
         BOOST_TEST(checked.encode() == encoded_artifact);
         BOOST_TEST(!checked.is_prepared());
         const auto profile_name = name + (darwin ? "-darwin-arm64" : "-linux-x86-64");
         optimizing_fixture_export::write(profile_name, encoded_request, encoded_artifact);
         BOOST_TEST_MESSAGE("benchmark=" << profile_name << " identity=" << checked.identity().str()
                                         << " wasm=" << checked.image().wasm.str() << " checksum=" << expected
                                         << " receiver=" << inputs[0] << " code=" << inputs[1]
                                         << " action=" << inputs[2] << " host_effects=1 performance=NOT_RUN");
      }
   }
}

BOOST_AUTO_TEST_CASE(export_checked_runtime_fixtures, *boost::unit_test::disabled()) {
   // Existing fixture owners ONLY. Every output goes through the actual public
   // compiler, SAME request/artifact Raw codec and identity validation.
   for (const auto darwin : {false, true}) {
      auto fixtures = std::vector<std::pair<std::string, oc::compile_request>>{
          {"add", integer_request()}, {"imports-indirect", import_indirect_request()},
          {"grow", growth_request()}, {"integer-traps", integer_request(0x6d)},
          {"memory-load", load_request(memory_loads[0])}, {"import-loop", epoch_import_loop_request()},
          {"finite", finite_stack_request()}, {"recursive", recursive_request()}};
      for (auto& [name, request] : fixtures) {
         if (darwin) {
            request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
            request.configuration.target.cpu = "generic";
            request.configuration.target.features = "+fp-armv8,+neon";
         }
         const auto code = tooling::compiler{}.compile(request);
         const auto encoded_request = request.encode();
         const auto encoded_artifact = code.encode();
         const auto checked = oc::artifact::decode(encoded_artifact, oc::identify(request), encoded_artifact.size());
         BOOST_TEST(checked.image().configuration.target.host_abi_version == 2U);
         BOOST_TEST(checked.image().configuration.target.codegen_version == 3U);
         BOOST_TEST(checked.encode() == encoded_artifact);
         BOOST_CHECK(checked.image().configuration == oc::canonicalize(request.configuration));
         BOOST_TEST(!checked.is_prepared()); // pointer-free export, not executor acceptance
         name += darwin ? "-darwin-arm64" : "-linux-x86-64";
         optimizing_fixture_export::write(name, encoded_request, encoded_artifact);
         BOOST_TEST_MESSAGE("fixture=" << name << " identity=" << checked.identity().str()
                            << " wasm=" << checked.image().wasm.str()
                            << " request_bytes=" << encoded_request.size()
                            << " artifact_bytes=" << encoded_artifact.size()
                            << " triple=" << checked.image().configuration.target.triple
                            << " ABI=" << checked.image().configuration.target.host_abi_version
                            << " codegen=" << checked.image().configuration.target.codegen_version);
      }
   }
}

BOOST_AUTO_TEST_CASE(genuine_recursive_request_preserves_canonical_depth_for_runtime_handoff) {
   const auto original = recursive_request();
   const auto wasm = original.wasm;
   BOOST_TEST(original.configuration.execution.max_call_depth == 251U);
   for (const auto remaining : {0U, 1U, 7U, 250U})
      BOOST_TEST(baseline_one(original, "f0", remaining) == 42U);
   // Existing interpreter reference capacity category only. The native
   // executor must independently observe raw stack_overflow8 with enough
   // physical/logical allowance and unchanged canonical configuration.
   BOOST_CHECK_THROW(baseline_one(original, "f0", 251U), vm::exceptions::vector_out_of_bounds);
   for (const auto darwin : {false, true}) {
      auto request = original;
      if (darwin) {
         request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
         request.configuration.target.cpu = "generic";
         request.configuration.target.features = "+fp-armv8,+neon";
      }
      const auto code = tooling::compiler{}.compile(request);
      BOOST_REQUIRE(code.image().functions.size() == 1U);
      BOOST_REQUIRE(code.image().types.size() == 1U);
      BOOST_TEST(code.image().functions[0].type_index == 0U);
      BOOST_TEST(code.image().types[0].params.size() == 1U);
      BOOST_TEST(code.image().types[0].params[0] == vm::i32);
      BOOST_TEST(code.image().configuration.execution.max_call_depth == 251U);
      BOOST_TEST(code.image().functions[0].stack_bytes > 0U);
      BOOST_TEST(code.image().functions[0].stack_bytes <= 16384U);
      BOOST_TEST(request.wasm == wasm);
      const auto encoded = code.encode();
      const auto checked = oc::artifact::decode(encoded, oc::identify(request), encoded.size());
      BOOST_CHECK(checked.image().configuration == oc::canonicalize(request.configuration));
      BOOST_TEST(checked.encode() == encoded);
      BOOST_TEST(!checked.is_prepared()); // No RX/native recursive execution here.
      BOOST_TEST_MESSAGE("recursive profile=" << request.configuration.target.triple
                         << " frame=" << checked.image().functions[0].stack_bytes
                         << " canonical_depth=" << checked.image().configuration.execution.max_call_depth
                         << " expected_arguments=250-success42,251-canonical-trap8");
   }
}

BOOST_AUTO_TEST_CASE(real_parser_llvm_object_artifact_and_raw_roundtrip) {
   const auto request = integer_request();
   const auto artifact = tooling::compiler{}.compile(request);
   BOOST_TEST(artifact.identity() == oc::identify(request));
   BOOST_TEST(!artifact.is_prepared());
   const auto& payload = artifact.image();
   BOOST_TEST(payload.functions.size() == 4U);
   BOOST_TEST(payload.types.size() == 3U);
   BOOST_TEST(payload.exports.size() == 4U);
   BOOST_TEST(payload.start == 3U);
   BOOST_REQUIRE(!payload.sections.empty());
   BOOST_TEST(!payload.relocations.empty()); // Actual generated-to-generated call.
   for (const auto& function : payload.functions) {
      BOOST_TEST(function.stack_bytes >= 8U);
      BOOST_TEST(function.stack_bytes <= request.configuration.limits.max_function_stack_bytes);
      BOOST_TEST(function.entry.offset < payload.sections[function.entry.section].bytes.size());
   }
   const auto encoded = artifact.encode();
   const auto decoded = oc::artifact::decode(encoded, artifact.identity(), encoded.size());
   BOOST_TEST(decoded.encode() == encoded);
   BOOST_TEST(tooling::compiler{}.compile(request).encode() == encoded);

   // Optional raw fixture handoff for the real Linux executor owner. Never
   // print payload/source bytes; normal tests have no file-write side effect.
   if (const auto* path = std::getenv("FORGE_VM_WASM_OPTIMIZING_FIXTURE_PATH")) {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
      output.close();
      BOOST_REQUIRE(output.good());
      auto wasm_output = std::ofstream{std::string{path} + ".wasm", std::ios::binary | std::ios::trunc};
      wasm_output.write(reinterpret_cast<const char*>(request.wasm.data()), request.wasm.size());
      wasm_output.close();
      BOOST_REQUIRE(wasm_output.good());
      const auto encoded_request = request.encode();
      auto request_output = std::ofstream{std::string{path} + ".request", std::ios::binary | std::ios::trunc};
      request_output.write(reinterpret_cast<const char*>(encoded_request.data()), encoded_request.size());
      request_output.close();
      BOOST_REQUIRE(request_output.good());
   }

   // Existing interpreter is only the semantic oracle, never the OC engine.
   auto input = request.wasm;
   auto memory = vm::wasm_allocator{};
   auto baseline =
       vm::backend<std::nullptr_t, vm::interpreter, vm::options>{input, &memory, request.configuration.execution};
   BOOST_TEST(baseline.call_with_return("env", "relay", std::uint32_t{UINT32_MAX}, std::uint32_t{2})->to_ui32() == 1U);
   BOOST_TEST(baseline.call_with_return("env", "wide", std::uint64_t{UINT64_MAX}, std::uint64_t{2})->to_ui64() == 1U);
}

BOOST_AUTO_TEST_CASE(canonical_input_and_output_budgets_are_enforced) {
   auto request = integer_request();
   request.wasm[0] = 1;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::invalid_module);
   for (const auto length : {std::size_t{0}, std::size_t{3}, std::size_t{10}, integer_request().wasm.size() - 1}) {
      request = integer_request();
      request.wasm.resize(length);
      BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::invalid_module);
   }
   request = integer_request();
   request.configuration.execution.max_code_bytes = 1;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::invalid_module);
   request = integer_request();
   request.configuration.limits.max_generated_bytes = 8;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::compile_limit);
   request = integer_request();
   request.configuration.limits.max_function_stack_bytes = 1;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::compile_limit);
}

BOOST_AUTO_TEST_CASE(unbound_valid_import_is_local_unavailability_not_invalid_wasm) {
   auto request = integer_request();
   request.wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   append_section(request.wasm, 1, {1, 0x60, 0, 0});
   append_section(request.wasm, 2, {1, 3, 'e', 'n', 'v', 6, 'u', 'n', 'u', 's', 'e', 'd', 0, 0});
   append_section(request.wasm, 3, {1, 0});
   append_section(request.wasm, 10, {1, 2, 0, 0x0b}); // Valid unused import still needs an exact binding.
   BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::unsupported_import);
   auto target = integer_request();
   target.configuration.target.features = "+avx";
   BOOST_CHECK_THROW(tooling::compiler{}.compile(target), oc::exceptions::unsupported_target);
}

BOOST_AUTO_TEST_CASE(import_bindings_and_indirect_types_reuse_canonical_frontend_equivalence) {
   for (const bool darwin : {false, true}) {
      auto request = import_indirect_request();
      if (darwin) {
         request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
         request.configuration.target.cpu = "generic";
         request.configuration.target.features = "+fp-armv8,+neon";
      }
      const auto artifact = tooling::compiler{}.compile(request);
      const auto& payload = artifact.image();
      BOOST_REQUIRE(payload.functions.size() == 7U);
      BOOST_REQUIRE(payload.types.size() == 3U); // Duplicate declaration was not given a new identity.
      BOOST_TEST(payload.configuration.imports[0].name == "a");
      BOOST_TEST(payload.configuration.imports[1].name == "z");
      BOOST_TEST(payload.functions[0].import_index == 1U);
      BOOST_TEST(payload.functions[1].import_index == 0U);
      for (const auto index : {0U, 1U, 2U, 4U, 5U})
         BOOST_TEST(payload.functions[index].type_index == 0U);
      for (const auto index : {2U, 3U, 4U, 5U, 6U})
         BOOST_TEST(payload.functions[index].import_index == UINT32_MAX);
      BOOST_TEST(payload.table == (std::vector<std::uint32_t>{0, 1, 2, UINT32_MAX, 6}));
      const auto encoded = artifact.encode();
      BOOST_TEST(oc::artifact::decode(encoded, artifact.identity(), encoded.size()).encode() == encoded);
      std::reverse(request.configuration.imports.begin(), request.configuration.imports.end());
      BOOST_TEST(tooling::compiler{}.compile(request).encode() == encoded);
      request.configuration.imports.pop_back();
      BOOST_CHECK_THROW(tooling::compiler{}.compile(request), oc::exceptions::unsupported_import);
   }
   auto mismatch = import_indirect_request();
   mismatch.configuration.imports[0].signature.params[0] = vm::i64;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(mismatch), oc::exceptions::unsupported_import);
   mismatch = import_indirect_request();
   mismatch.configuration.imports[0].signature.ret[0] = vm::f32;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(mismatch), oc::exceptions::unsupported_import);
   for (const auto& request : {imported_entry_request({}, 0, true),
                               imported_entry_request({}, vm::i32),
                               imported_entry_request({vm::i32, vm::f64}, vm::f32)}) {
      const auto artifact = tooling::compiler{}.compile(request);
      BOOST_TEST(artifact.image().functions.size() == 1U);
      BOOST_TEST(artifact.image().functions[0].import_index == 0U);
   }
   const auto absent_table = functions_request({0x7f, 0x7f}, 0x7f,
       {{0, 0x20, 0, 0x20, 1, 0x41, 0, 0x11, 0, 0, 0x0b}});
   BOOST_CHECK_THROW(tooling::compiler{}.compile(absent_table), oc::exceptions::invalid_module);
   BOOST_CHECK_THROW(tooling::compiler{}.compile(import_indirect_request(4)), oc::exceptions::invalid_module);
   BOOST_CHECK_THROW(tooling::compiler{}.compile(import_indirect_request(1, 1)), oc::exceptions::invalid_module);
}

BOOST_AUTO_TEST_CASE(canonical_control_flow_result_merges_module_pcs_and_branch_table_payloads) {
   const auto request = control_flow_request();
   const auto artifact = tooling::compiler{}.compile(request);
   BOOST_TEST(artifact.image().functions.size() == 7U);
   BOOST_TEST(artifact.encode() == tooling::compiler{}.compile(request).encode());
   const auto inputs = std::array<std::uint32_t, 4>{0, 1, 7, UINT32_MAX};
   for (const auto input : inputs) {
      BOOST_TEST(baseline(request, "f0", input, std::uint32_t{27}) == (input ? 27U : 9U));
      BOOST_TEST(baseline(request, "f1", input, std::uint32_t{27}) == (input ? 12U : std::uint32_t{0xffffffe3}));
      BOOST_TEST(baseline(request, "f2", input, std::uint32_t{27}) == (input == 0 ? 13U : 12U));
      BOOST_TEST(baseline(request, "f4", input, std::uint32_t{27}) == 27U);
      BOOST_TEST(baseline(request, "f5", input, std::uint32_t{27}) == (input ? input : 27U));
      BOOST_TEST(baseline(request, "f6", input, std::uint32_t{27}) == 12U);
   }
   BOOST_TEST(baseline(request, "f3", std::uint32_t{10}, std::uint32_t{27}) == 55U);
   auto bounded = request;
   bounded.configuration.limits.max_function_stack_bytes = 1;
   BOOST_CHECK_THROW(tooling::compiler{}.compile(bounded), oc::exceptions::compile_limit);
}

BOOST_AUTO_TEST_CASE(integer_families_emit_verified_native_code_with_defined_wasm_boundaries) {
   // Compile every MVP integer comparison, numeric op and integer conversion
   // through the canonical parser and real ELF/frame normalizer. On arm64 this
   // is codegen, not execution; the interpreter only checks fixture semantics.
   for (const bool wide : {false, true}) {
      const auto type = static_cast<std::uint8_t>(wide ? 0x7e : 0x7f);
      for (auto operation = wide ? vm::i64_eq : vm::i32_eq; operation <= (wide ? vm::i64_ge_u : vm::i32_ge_u);
           operation = static_cast<vm::opcodes>(operation + 1)) {
         const auto request =
             functions_request({type, type}, 0x7f, {{0, 0x20, 0, 0x20, 1, static_cast<std::uint8_t>(operation), 0x0b}});
         BOOST_TEST(tooling::compiler{}.compile(request).image().functions.size() == 1U);
      }
      for (auto operation = wide ? vm::i64_clz : vm::i32_clz; operation <= (wide ? vm::i64_rotr : vm::i32_rotr);
           operation = static_cast<vm::opcodes>(operation + 1)) {
         const auto unary = operation <= (wide ? vm::i64_popcnt : vm::i32_popcnt);
         auto body = bytes{0, 0x20, 0};
         if (!unary)
            body.insert(body.end(), {0x20, 1});
         body.insert(body.end(), {static_cast<std::uint8_t>(operation), 0x0b});
         const auto request = functions_request(unary ? bytes{type} : bytes{type, type}, type, {body});
         BOOST_TEST(tooling::compiler{}.compile(request).image().functions.size() == 1U);
      }
   }
   for (const auto operation :
        {vm::i32_eqz, vm::i64_eqz, vm::i32_wrap_i64, vm::i64_extend_s_i32, vm::i64_extend_u_i32}) {
      const auto input =
          static_cast<std::uint8_t>(operation == vm::i64_eqz || operation == vm::i32_wrap_i64 ? 0x7e : 0x7f);
      const auto output = static_cast<std::uint8_t>(
          operation == vm::i64_extend_s_i32 || operation == vm::i64_extend_u_i32 ? 0x7e : 0x7f);
      const auto request =
          functions_request({input}, output, {{0, 0x20, 0, static_cast<std::uint8_t>(operation), 0x0b}});
      BOOST_TEST(tooling::compiler{}.compile(request).image().functions.size() == 1U);
   }
   const auto remainder = integer_request(0x6f);
   BOOST_TEST(baseline(remainder, "add", std::uint32_t{0x80000000}, std::uint32_t{UINT32_MAX}) == 0U);
   const auto shift = integer_request(0x74);
   BOOST_TEST(baseline(shift, "add", std::uint32_t{1}, std::uint32_t{32}) == 1U);
   BOOST_TEST(baseline(shift, "add", std::uint32_t{1}, std::uint32_t{UINT32_MAX}) == 0x80000000U);
}

BOOST_AUTO_TEST_CASE(memory_widths_signs_unaligned_offsets_and_raw_float_bits_emit_real_artifacts) {
   for (const auto& load : memory_loads) {
      const auto request = load_request(load);
      const auto artifact = tooling::compiler{}.compile(request);
      BOOST_TEST(artifact.image().memory.initial_pages == 1U);
      BOOST_TEST(artifact.image().memory.max_pages == 3U);
      BOOST_TEST(artifact.image().data.size() == 1U);
      BOOST_TEST(baseline_one(request, "f0", std::uint32_t{0}) == loaded_bits(load));
   }
   // Offset 3 differs from natural alignment 2 and is itself unaligned.
   const auto hinted = load_request(memory_loads[0], 3, 2);
   BOOST_TEST(tooling::compiler{}.compile(hinted).image().functions.size() == 1U);
   BOOST_TEST(baseline_one(hinted, "f0", std::uint32_t{0}) == loaded_bits(memory_loads[0]));
   for (const auto& store : memory_stores) {
      const auto request = store_request(store);
      BOOST_TEST(tooling::compiler{}.compile(request).image().functions.size() == 1U);
      auto input = request.wasm;
      auto memory = vm::wasm_allocator{};
      auto instance =
          vm::backend<std::nullptr_t, vm::interpreter, vm::options>{input, &memory, request.configuration.execution};
      auto* base = instance.get_context().linear_memory();
      base[2] = 0x35;
      base[3 + store.width] = 0x6a;
      const auto value = std::uint64_t{0xfedcba9876543210};
      const auto result =
          store.wide
              ? instance.call_with_return("env", "f0", std::uint32_t{0}, value)->to_ui64()
              : instance.call_with_return("env", "f0", std::uint32_t{0}, static_cast<std::uint32_t>(value))->to_ui32();
      const auto mask = store.width == 8 ? UINT64_MAX : (std::uint64_t{1} << (8 * store.width)) - 1;
      BOOST_TEST(result == (value & mask));
      BOOST_TEST(static_cast<std::uint8_t>(base[2]) == 0x35U);
      BOOST_TEST(static_cast<std::uint8_t>(base[3 + store.width]) == 0x6aU);
      for (std::uint32_t i = 0; i < store.width; ++i)
         BOOST_TEST(static_cast<std::uint8_t>(base[3 + i]) == ((value >> (8 * i)) & 0xff));
   }
   for (const bool wide : {false, true}) {
      const auto request = float_bits_request(wide);
      const auto artifact = tooling::compiler{}.compile(request);
      BOOST_TEST(artifact.image().functions.size() == 3U);
      BOOST_TEST(artifact.image().globals.size() == 1U);
      const auto nan = wide ? std::uint64_t{0x7ff8000000012345} : std::uint64_t{0x7fc12345};
      BOOST_TEST(artifact.image().globals[0].bits == nan);
      const auto expected = loaded_bits(memory_loads[wide ? 1 : 0]);
      BOOST_TEST(baseline(request, "f0", 0U, 32U) == expected);
      BOOST_TEST(baseline(request, "f1", 0U, 32U) == expected);
      BOOST_TEST(baseline(request, "f2", 0U, 32U) == nan);
   }
}

BOOST_AUTO_TEST_CASE(globals_and_memory_growth_use_the_existing_options_and_preparation) {
   for (const bool wide : {false, true}) {
      const auto request = globals_request(wide);
      const auto artifact = tooling::compiler{}.compile(request);
      BOOST_TEST(artifact.image().globals[0].bits == 7U);
      if (wide) {
         BOOST_TEST(baseline_one(request, "f0", std::uint64_t{0}) == 7U);
         BOOST_TEST(baseline_one(request, "f1", UINT64_MAX) == UINT64_MAX);
      } else {
         BOOST_TEST(baseline_one(request, "f0", std::uint32_t{0}) == 7U);
         BOOST_TEST(baseline_one(request, "f1", UINT32_MAX) == UINT32_MAX);
      }
      BOOST_CHECK_THROW(tooling::compiler{}.compile(globals_request(wide, false)), oc::exceptions::invalid_module);
   }
   auto request = growth_request();
   BOOST_TEST(tooling::compiler{}.compile(request).image().memory.max_pages == 3U);
   auto input = request.wasm;
   auto memory = vm::wasm_allocator{};
   auto instance =
       vm::backend<std::nullptr_t, vm::interpreter, vm::options>{input, &memory, request.configuration.execution};
   BOOST_TEST(instance.call_with_return("env", "f0", 0U)->to_ui32() == 1U);
   BOOST_TEST(instance.call_with_return("env", "f1", 0U)->to_ui32() == 1U);
   BOOST_TEST(instance.call_with_return("env", "f1", 1U)->to_ui32() == 1U);
   BOOST_TEST(instance.call_with_return("env", "f0", 0U)->to_ui32() == 2U);
   auto* base = instance.get_context().linear_memory();
   BOOST_TEST(static_cast<std::uint8_t>(base[3]) == 0x80U);
   for (std::size_t offset = vm::page_size; offset < 2U * vm::page_size; ++offset)
      BOOST_REQUIRE(base[offset] == 0);
   BOOST_TEST(instance.call_with_return("env", "f1", 2U)->to_ui32() == UINT32_MAX);
   BOOST_TEST(instance.call_with_return("env", "f1", UINT32_MAX)->to_ui32() == UINT32_MAX);
   BOOST_TEST(instance.call_with_return("env", "f0", 0U)->to_ui32() == 2U);
   BOOST_TEST(static_cast<std::uint8_t>(base[3]) == 0x80U);
   request.configuration.execution.max_pages = 1;
   BOOST_TEST(tooling::compiler{}.compile(request).image().configuration.execution.max_pages == 1U);
   BOOST_TEST(baseline_one(request, "f1", 1U) == UINT32_MAX);
   const auto empty = growth_request(0);
   BOOST_TEST(tooling::compiler{}.compile(empty).image().memory.initial_pages == 0U);
   BOOST_TEST(baseline_one(empty, "f0", 0U) == 0U);
   BOOST_TEST(baseline_one(empty, "f1", 1U) == 0U);
   const auto start = start_initialization_request();
   const auto initialized = tooling::compiler{}.compile(start);
   BOOST_TEST(initialized.image().start == 0U);
   BOOST_TEST(initialized.image().globals[0].bits == 7U);
   auto start_input = start.wasm;
   auto start_memory = vm::wasm_allocator{};
   auto start_instance = vm::backend<std::nullptr_t, vm::interpreter, vm::options>{start_input, &start_memory,
                                                                                   start.configuration.execution};
   BOOST_TEST(start_instance.call_with_return("env", "f0")->to_ui32() == 22U);
}

BOOST_AUTO_TEST_CASE(memory_admission_is_canonical_and_out_of_bounds_addresses_never_wrap) {
   const auto load = memory_loads[0];
   BOOST_CHECK_THROW(tooling::compiler{}.compile(load_request(load, 0, 3)), oc::exceptions::invalid_module);
   const auto absent = functions_request({0x7f}, 0x7f, {{0, 0x20, 0, 0x28, 0, 0, 0x0b}});
   BOOST_CHECK_THROW(tooling::compiler{}.compile(absent), oc::exceptions::invalid_module);
   const auto bad_reserved = functions_request({}, 0x7f, {{0, 0x3f, 1, 0x0b}}, true);
   BOOST_CHECK_THROW(tooling::compiler{}.compile(bad_reserved), oc::exceptions::invalid_module);
   const auto bad_offset =
       functions_request({0x7f}, 0x7f, {{0, 0x20, 0, 0x28, 0, 0x80, 0x80, 0x80, 0x80, 0x10, 0x0b}}, true);
   BOOST_CHECK_THROW(tooling::compiler{}.compile(bad_offset), oc::exceptions::invalid_module);
   // All are valid WASM; only the invocation access is out of bounds.
   const auto accesses =
       std::array{std::pair{load_request(load, UINT32_MAX), UINT32_MAX},
                  std::pair{load_request(load, 0), static_cast<std::uint32_t>(vm::page_size - 3)},
                  std::pair{load_request(load, 0), UINT32_MAX}, std::pair{load_request(load, 0, 0, 0), 0U}};
   for (const auto& [request, address] : accesses) {
      BOOST_TEST(tooling::compiler{}.compile(request).image().functions.size() == 1U);
      BOOST_CHECK_THROW(baseline_one(request, "f0", address), vm::exceptions::memory);
   }
}

BOOST_AUTO_TEST_CASE(dense_branch_table_emits_real_readonly_data_and_page_relocations) {
   for (const bool darwin : {false, true}) {
      auto request = dense_branch_table_request();
      if (darwin) {
         request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
         request.configuration.target.cpu = "generic";
         request.configuration.target.features = "+fp-armv8,+neon";
      }
      const auto artifact = tooling::compiler{}.compile(request);
      BOOST_TEST(std::any_of(artifact.image().sections.begin(), artifact.image().sections.end(), [](const auto& value) {
         return value.kind == oc::section_kind::readonly_data;
      }));
      if (darwin)
         BOOST_TEST(std::any_of(artifact.image().relocations.begin(), artifact.image().relocations.end(), [](const auto& value) {
            return value.kind == oc::relocation_kind::aarch64_page21;
         }));
      const auto encoded = artifact.encode();
      BOOST_TEST(oc::artifact::decode(encoded, artifact.identity(), encoded.size()).encode() == encoded);
   }
}

BOOST_AUTO_TEST_CASE(explicit_darwin_abi_elf_codegen_has_aarch64_calls_and_fixed_aligned_frames) {
   auto request = integer_request();
   const auto linux_identity = oc::identify(request);
   request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
   request.configuration.target.cpu = "generic";
   request.configuration.target.features = "+fp-armv8,+neon";
   const auto artifact = tooling::compiler{}.compile(request);
   BOOST_TEST(artifact.identity() != linux_identity);
   BOOST_TEST((artifact.image().configuration.target == request.configuration.target));
   BOOST_TEST(std::any_of(artifact.image().relocations.begin(), artifact.image().relocations.end(), [](const auto& value) {
      return value.kind == oc::relocation_kind::aarch64_call26;
   }));
   for (const auto& function : artifact.image().functions) {
      BOOST_TEST(function.entry.offset % 4 == 0U);
      BOOST_TEST(function.stack_bytes % 16 == 0U);
      BOOST_TEST(function.stack_bytes <= request.configuration.limits.max_function_stack_bytes);
   }
   const auto encoded = artifact.encode();
   BOOST_TEST(oc::artifact::decode(encoded, artifact.identity(), encoded.size()).encode() == encoded);
   BOOST_TEST(tooling::compiler{}.compile(request).encode() == encoded);
}

BOOST_AUTO_TEST_CASE(all_fp_numeric_signatures_and_locals_emit_verified_objects_for_both_profiles) {
   const auto fixtures = numeric_cases();
   BOOST_TEST(fixtures.size() == 62U);
   for (const bool darwin : {false, true})
      for (const auto& fixture : fixtures) {
         auto request = numeric_request(fixture);
         if (darwin) {
            request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
            request.configuration.target.cpu = "generic";
            request.configuration.target.features = "+fp-armv8,+neon";
         }
         const auto artifact = tooling::compiler{}.compile(request);
         BOOST_TEST(artifact.image().functions.size() == 2U);
         BOOST_TEST(artifact.image().types[0].params.size() == fixture.arity);
         const auto encoded = artifact.encode();
         BOOST_TEST(oc::artifact::decode(encoded, artifact.identity(), encoded.size()).encode() == encoded);
      }
}

#if (defined(__linux__) && defined(__x86_64__)) || (defined(__APPLE__) && defined(__aarch64__))
namespace {
oc::artifact compile_native(oc::compile_request request) {
#if defined(__APPLE__)
   request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
   request.configuration.target.cpu = "generic";
   request.configuration.target.features = "+fp-armv8,+neon";
#endif
   return tooling::compiler{}.compile(request);
}
oc::artifact decoded_copy(const oc::artifact& source) {
   const auto encoded = source.encode();
   auto result = oc::artifact::decode(encoded, source.identity(), encoded.size());
   BOOST_TEST(result.encode() == encoded);
   return result;
}

struct native_probe {
   oc::artifact original;
   oc::artifact artifact;
   oc::invocation_callbacks callbacks;
   oc::invocation_abi context;
   alignas(std::atomic_ref<std::uint64_t>::required_alignment) std::uint64_t epoch{};
   alignas(std::atomic_ref<std::uint32_t>::required_alignment) std::uint32_t sticky_reason{};
   std::uint32_t polls = 0;
   std::uint32_t numerics = 0;
   std::uint32_t host_effects = 0;
   std::uint32_t last_import = UINT32_MAX;
   std::vector<oc::table_entry> table;
   bytes memory;
   bytes relocated_memory;
   std::vector<vm::expr_value> globals;

   explicit native_probe(const oc::compile_request& request)
       : original(compile_native(request)), artifact(decoded_copy(original)) {
      original.prepare(); // Pin this mapping so the decoded image cannot reuse its address.
      artifact.prepare();
      BOOST_TEST(static_cast<bool>(original.entry(0) != artifact.entry(0)));
      callbacks.poll = +[](oc::invocation_abi* invocation) noexcept {
         auto& probe = *static_cast<native_probe*>(invocation->owner);
         ++probe.polls;
         const auto observed = std::atomic_ref{probe.epoch}.load(std::memory_order_acquire);
         const auto reason = std::atomic_ref{probe.sticky_reason}.load(std::memory_order_acquire);
         if (reason != 0)
            return static_cast<oc::trap_reason>(reason);
         invocation->epoch_deadline = observed == UINT64_MAX ? UINT64_MAX : observed + 1;
         return oc::trap_reason::none;
      };
      callbacks.trap = +[](oc::invocation_abi*, oc::trap_reason) noexcept { std::abort(); };
      callbacks.host_call = +[](oc::invocation_abi* context, std::uint32_t index,
                                const vm::expr_value* arguments, vm::expr_value* result) noexcept {
         auto& probe = *static_cast<native_probe*>(context->owner);
         if (index > 1)
            return oc::trap_reason::host_exception;
         ++probe.host_effects;
         probe.last_import = index;
         const auto left = static_cast<std::uint32_t>(arguments[0].i64);
         const auto right = static_cast<std::uint32_t>(arguments[1].i64);
         result->i64 = index ? left - 2U * right : left + 3U * right;
         return oc::trap_reason::none;
      };
      callbacks.numeric = +[](oc::invocation_abi* context, std::uint32_t operation, std::uint32_t count,
                              const vm::expr_value* arguments, vm::expr_value* result) noexcept {
         auto& probe = *static_cast<native_probe*>(context->owner);
         ++probe.numerics;
         // Primitive runs on the ordinary host stack without active recovery;
         // the real executor still owns guest/host bounce and whole-NULL entry.
         try {
            *result = vm::execute_numeric(static_cast<vm::opcodes>(operation), {arguments, count});
            return oc::trap_reason::none;
         } catch (const vm::exceptions::interpreter&) {
            return oc::trap_reason::invalid_conversion;
         } catch (...) {
            return oc::trap_reason::host_exception;
         }
      };
      context.owner = this;
      std::atomic_ref{epoch}.store(0, std::memory_order_relaxed);
      std::atomic_ref{sticky_reason}.store(0, std::memory_order_relaxed);
      context.epoch = &epoch;
      context.sticky_reason = &sticky_reason;
      context.epoch_deadline = 1;
      context.callbacks = &callbacks;
      context.functions = artifact.functions().data();
      context.function_count = artifact.functions().size();
      context.max_call_depth = 32;
      context.stack_remaining_bytes = 1024 * 1024;
      const auto& payload = artifact.image();
      table.reserve(payload.table.size());
      for (const auto index : payload.table)
         table.push_back({index, index == UINT32_MAX ? UINT32_MAX : payload.functions[index].type_index});
      context.table = table.data();
      context.table_count = table.size();
      globals.resize(payload.globals.size());
      for (std::size_t index = 0; index < globals.size(); ++index)
         globals[index].i64 = std::bit_cast<std::int64_t>(payload.globals[index].bits);
      context.globals = globals.data();
      context.global_count = globals.size();
      if (payload.memory.defined) {
         // Test-only fixed storage, admitted before native entry. This is not
         // the allocator/guarded-memory implementation of the real executor.
         memory.assign(3U * vm::page_size + 16, 0);
         relocated_memory.assign(memory.size(), 0);
         for (std::size_t index = 0; index < 8; ++index) {
            memory[index] = memory[memory.size() - 8 + index] = 0xa5;
            relocated_memory[index] = relocated_memory[relocated_memory.size() - 8 + index] = 0xa5;
         }
         context.memory_base = memory.data() + 8;
         context.current_pages = payload.memory.initial_pages;
         context.memory_bytes = std::uint64_t{context.current_pages} * vm::page_size;
         context.max_pages = std::min({payload.memory.max_pages, payload.configuration.execution.max_pages, 3U});
         for (const auto& segment : payload.data)
            std::memcpy(context.memory_base + segment.offset, segment.bytes.data(), segment.bytes.size());
         callbacks.grow_memory = +[](oc::invocation_abi* context, std::uint32_t delta) noexcept -> std::int32_t {
            if (context->current_pages > context->max_pages || delta > context->max_pages - context->current_pages)
               return -1;
            const auto old_pages = context->current_pages;
            if (!delta)
               return static_cast<std::int32_t>(old_pages);
            auto& probe = *static_cast<native_probe*>(context->owner);
            auto* previous = context->memory_base;
            auto* next =
                previous == probe.memory.data() + 8 ? probe.relocated_memory.data() + 8 : probe.memory.data() + 8;
            const auto added_bytes = std::uint64_t{delta} * vm::page_size;
            std::memcpy(next, previous, context->memory_bytes);
            std::memset(next + context->memory_bytes, 0, added_bytes);
            // Deliberately invalidate an old data byte: using a cached base
            // after callback return yields a different, observable result.
            if (context->memory_bytes > 3)
               previous[3] = 0x5e;
            context->memory_base = next;
            context->memory_bytes += added_bytes;
            context->current_pages += delta;
            return static_cast<std::int32_t>(old_pages);
         };
      }
   }

   void check_canaries() const {
      for (const auto* buffer : {&memory, &relocated_memory})
         if (!buffer->empty())
            for (std::size_t index = 0; index < 8; ++index) {
               BOOST_TEST((*buffer)[index] == 0xa5U);
               BOOST_TEST((*buffer)[buffer->size() - 8 + index] == 0xa5U);
            }
   }

   std::uint64_t call(std::uint32_t index, std::uint64_t left, std::uint64_t right, std::uint64_t third = 0) {
      auto arguments = std::array<vm::expr_value, 3>{};
      arguments[0].i64 = std::bit_cast<std::int64_t>(left);
      arguments[1].i64 = std::bit_cast<std::int64_t>(right);
      arguments[2].i64 = std::bit_cast<std::int64_t>(third);
      auto result = vm::expr_value{};
      artifact.entry(index)(&context, arguments.data(), &result);
      BOOST_TEST(context.call_depth == 0U);
      BOOST_TEST(context.stack_remaining_bytes == 1024U * 1024U);
      check_canaries();
      return std::bit_cast<std::uint64_t>(result.i64);
   }
};
} // namespace

BOOST_AUTO_TEST_CASE(native_integer_entry_primitive_smoke_not_executor_acceptance) {
   const auto artifact = compile_native(integer_request());
   artifact.prepare();
   auto callbacks = oc::invocation_callbacks{};
   callbacks.poll = +[](oc::invocation_abi*) noexcept { return oc::trap_reason::none; };
   callbacks.trap = +[](oc::invocation_abi*, oc::trap_reason) noexcept { std::abort(); };
   auto context = oc::invocation_abi{};
   alignas(std::atomic_ref<std::uint64_t>::required_alignment) std::uint64_t epoch{};
   alignas(std::atomic_ref<std::uint32_t>::required_alignment) std::uint32_t sticky_reason{};
   std::atomic_ref{epoch}.store(0, std::memory_order_relaxed);
   std::atomic_ref{sticky_reason}.store(0, std::memory_order_relaxed);
   context.epoch = &epoch;
   context.sticky_reason = &sticky_reason;
   context.epoch_deadline = 1;
   context.callbacks = &callbacks;
   context.functions = artifact.functions().data();
   context.function_count = artifact.functions().size();
   context.max_call_depth = 4;
   context.stack_remaining_bytes = 1024 * 1024;
   auto arguments = std::array<vm::expr_value, 2>{};
   arguments[0].i64 = UINT32_MAX;
   arguments[1].i64 = 2;
   auto result = vm::expr_value{};
   artifact.entry(1)(&context, arguments.data(), &result);
   BOOST_TEST(result.i64 == 1);
   BOOST_TEST(context.call_depth == 0U);
   BOOST_TEST(context.stack_remaining_bytes == 1024U * 1024U);
   arguments[0].i64 = -1;
   artifact.entry(2)(&context, arguments.data(), &result);
   BOOST_TEST(result.i64 == 1);
   BOOST_TEST(context.call_depth == 0U);
}

BOOST_AUTO_TEST_CASE(native_finite_callpath_distinguishes_local_stack_capacity_from_canonical_depth_primitive) {
   if (const auto* mode = fresh_process::mode()) {
      fresh_process::disable_core_dumps();
      const auto control = std::strtoul(mode, nullptr, 10);
      const auto request = functions_request({}, 0x7f,
          {{0, 0x10, 1, 0x41, 1, 0x6a, 0x0b},
           {0, 0x10, 2, 0x41, 1, 0x6a, 0x0b},
           {0, 0x41, 40, 0x0b}});
      BOOST_REQUIRE(request.configuration.execution.max_call_depth > 3U);
      auto probe = native_probe{request};
      auto total = std::uint64_t{};
      auto largest = std::uint64_t{};
      for (const auto& function : probe.artifact.functions()) {
         BOOST_REQUIRE(function.stack_bytes > 0U);
         BOOST_REQUIRE(function.stack_bytes <= UINT64_MAX - total);
         total += function.stack_bytes;
         largest = std::max(largest, function.stack_bytes);
      }
      BOOST_REQUIRE(total > largest);
      probe.context.max_call_depth = request.configuration.execution.max_call_depth;
      // Every individual frame fits this small LOCAL allowance, yet the
      // finite aggregate path (depth3<canonical maximum) cannot fit it.
      const auto budget = control == 0 ? largest : total;
      probe.context.stack_remaining_bytes = budget;
      if (control == 2)
         probe.context.max_call_depth = 2; // canonical guard at the third entry
      else if (control == 3) {
         probe.context.call_depth = probe.context.max_call_depth;
         probe.context.stack_remaining_bytes = 0; // depth guard must win first
      } else if (control > 3)
         std::_Exit(91);
      probe.callbacks.trap = +[](oc::invocation_abi* context, oc::trap_reason reason) noexcept {
         const auto& probe = *static_cast<native_probe*>(context->owner);
         std::_Exit(probe.host_effects ? 90 : 32 + static_cast<int>(reason));
      };
      BOOST_TEST_MESSAGE("finite path frames=" << probe.artifact.functions()[0].stack_bytes << ','
                         << probe.artifact.functions()[1].stack_bytes << ','
                         << probe.artifact.functions()[2].stack_bytes << " local=" << budget
                         << " canonical_depth=" << request.configuration.execution.max_call_depth);
      auto result = vm::expr_value{};
      probe.artifact.entry(0)(&probe.context, nullptr, &result);
      if (control != 1 || result.i64 != 42 || probe.context.call_depth != 0 ||
          probe.context.stack_remaining_bytes != budget || probe.host_effects != 0)
         std::_Exit(99);
      std::_Exit(0);
   }
   const auto expected = std::array{32 + static_cast<int>(oc::trap_reason::native_stack_exhausted), 0,
                                    32 + static_cast<int>(oc::trap_reason::stack_overflow),
                                    32 + static_cast<int>(oc::trap_reason::stack_overflow)};
   for (std::size_t control = 0; control < expected.size(); ++control) {
      const auto child = fresh_process::run(
          "optimizing_compiler/native_finite_callpath_distinguishes_local_stack_capacity_from_canonical_depth_primitive",
          std::to_string(control));
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status)); // no fault/recovery claim
      BOOST_TEST(WEXITSTATUS(child.status) == expected[control]);
   }
}

BOOST_AUTO_TEST_CASE(native_epoch_rearm_and_shared_bursts_do_not_cancel_other_invocation_primitive) {
   auto probe = native_probe{epoch_import_loop_request()};
   probe.callbacks.host_call = +[](oc::invocation_abi* context, std::uint32_t index,
                                  const vm::expr_value*, vm::expr_value*) noexcept {
      auto& probe = *static_cast<native_probe*>(context->owner);
      if (index)
         return oc::trap_reason::host_exception;
      ++probe.host_effects;
      const auto snapshot = std::atomic_ref{probe.epoch}.fetch_add(1, std::memory_order_acq_rel) + 1;
      context->epoch_deadline = snapshot + 1; // while this guest is suspended, as the bridge contract requires
      return oc::trap_reason::none;
   };
   BOOST_TEST(probe.call(1, 20, 0) == 0U);
   BOOST_TEST(probe.host_effects == 20U);
   BOOST_TEST(probe.polls == 0U); // Backedge reload observes each host rearm; no stale-threshold callback.
   auto publishers = std::array<std::thread, 4>{};
   for (auto& publisher : publishers)
      publisher = std::thread{[&] {
         for (unsigned i = 0; i < 100; ++i)
            std::atomic_ref{probe.epoch}.fetch_add(1, std::memory_order_acq_rel);
      }};
   for (auto& publisher : publishers)
      publisher.join();
   BOOST_TEST(std::atomic_ref{probe.epoch}.load(std::memory_order_acquire) == 420U);
   BOOST_TEST(probe.call(1, 1, 0) == 0U);
   BOOST_TEST(probe.polls == 1U); // A shared wake with no sticky reason merely rearms.
   BOOST_TEST(std::atomic_ref{probe.sticky_reason}.load(std::memory_order_acquire) == 0U);
   auto saturated = native_probe{control_flow_request()};
   std::atomic_ref{saturated.epoch}.store(UINT64_MAX, std::memory_order_release);
   saturated.context.epoch_deadline = UINT64_MAX;
   BOOST_TEST(saturated.call(3, 10, 0) == 55U);
   BOOST_TEST(saturated.polls >= 11U); // Saturation stays slow, never wraps/reopens a fast path.
}

BOOST_AUTO_TEST_CASE(native_epoch_and_abi2_software_guards_are_bounded_fresh_primitives_not_executor) {
   if (const auto mode = fresh_process::mode(); mode != nullptr) {
      fresh_process::disable_core_dumps();
      const auto control = static_cast<unsigned>(std::stoul(mode));
      auto probe = native_probe{import_indirect_request()};
      probe.callbacks.trap = +[](oc::invocation_abi* context, oc::trap_reason reason) noexcept {
         const auto& probe = *static_cast<native_probe*>(context->owner);
         std::_Exit(probe.host_effects ? 90 : 32 + static_cast<int>(reason));
      };
      switch (control) {
      case 0: probe.context.version = 1; break;
      case 1: probe.context.size = 144; break;
      case 2: probe.context.epoch = nullptr; break;
      case 3: probe.context.sticky_reason = nullptr; break;
      case 4:
      case 5: {
         const auto reason = control == 4 ? oc::trap_reason::cancelled : oc::trap_reason::deadline;
         auto expected = std::uint32_t{0};
         std::atomic_ref{probe.sticky_reason}.compare_exchange_strong(
             expected, static_cast<std::uint32_t>(reason), std::memory_order_release, std::memory_order_relaxed);
         std::atomic_ref{probe.epoch}.fetch_add(1, std::memory_order_acq_rel);
         break;
      }
      default: std::_Exit(91);
      }
      probe.call(0, 19, 5);
      std::_Exit(99);
   }
   const auto reasons = std::array{oc::trap_reason::host_exception, oc::trap_reason::host_exception,
                                    oc::trap_reason::host_exception, oc::trap_reason::host_exception,
                                    oc::trap_reason::cancelled, oc::trap_reason::deadline};
   for (std::size_t control = 0; control < reasons.size(); ++control) {
      const auto child = fresh_process::run(
          "optimizing_compiler/native_epoch_and_abi2_software_guards_are_bounded_fresh_primitives_not_executor",
          std::to_string(control));
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status));
      BOOST_TEST(WEXITSTATUS(child.status) == 32 + static_cast<int>(reasons[control]));
   }
}

BOOST_AUTO_TEST_CASE(native_cfg_results_and_integer_boundaries_primitive_not_guarded_executor) {
   const auto request = control_flow_request();
   auto probe = native_probe{request};
   for (const auto input : {0U, 1U, 7U, UINT32_MAX})
      for (const auto index : {0U, 1U, 2U, 4U, 5U, 6U})
         BOOST_TEST(probe.call(index, input, 27) == baseline(request, "f" + std::to_string(index), input, 27U));
   const auto before = probe.polls;
   BOOST_TEST(probe.call(3, 10, 0) == 55U);
   BOOST_TEST(probe.polls == before); // Stable epoch: no callback at entry/backedges.
   std::atomic_ref{probe.epoch}.fetch_add(1, std::memory_order_acq_rel);
   BOOST_TEST(probe.call(3, 10, 0) == 55U);
   BOOST_TEST(probe.polls == before + 1U); // A wake is not cancellation; slow rearm happens once.
   const auto dense_request = dense_branch_table_request();
   auto dense = native_probe{dense_request};
   for (const auto right : {0U, 27U, 0x80000000U, UINT32_MAX})
      for (std::uint32_t left = 0; left < 18; ++left)
         BOOST_TEST(dense.call(0, left, right) == baseline(dense_request, "f0", left, right));
   for (const auto operation : {vm::i32_rem_s, vm::i32_div_s, vm::i32_div_u, vm::i32_rem_u, vm::i32_shl, vm::i32_shr_s,
                                vm::i32_shr_u, vm::i32_rotl, vm::i32_rotr}) {
      auto binary = native_probe{integer_request(static_cast<std::uint8_t>(operation))};
      const auto input = operation == vm::i32_div_s ? 0x7fffffffU : 0x80000000U;
      BOOST_TEST(binary.call(0, input, UINT32_MAX) ==
                 baseline(integer_request(static_cast<std::uint8_t>(operation)), "add", input, UINT32_MAX));
   }
   for (const auto operation : {vm::i64_rem_s, vm::i64_div_s, vm::i64_div_u, vm::i64_rem_u, vm::i64_shl, vm::i64_shr_s,
                                vm::i64_shr_u, vm::i64_rotl, vm::i64_rotr}) {
      const auto wide =
          functions_request({0x7e, 0x7e}, 0x7e, {{0, 0x20, 0, 0x20, 1, static_cast<std::uint8_t>(operation), 0x0b}});
      auto binary = native_probe{wide};
      const auto input =
          operation == vm::i64_div_s ? std::uint64_t{0x7fffffffffffffff} : std::uint64_t{0x8000000000000000};
      BOOST_TEST(binary.call(0, input, UINT64_MAX) == baseline(wide, "f0", input, UINT64_MAX));
   }
}

BOOST_AUTO_TEST_CASE(native_memory_globals_and_forced_base_reload_primitive_not_executor_acceptance) {
   for (const auto& load : memory_loads) {
      auto probe = native_probe{load_request(load)};
      BOOST_TEST(probe.call(0, 0, 0) == loaded_bits(load));
      BOOST_TEST(probe.call(0, vm::page_size - 3 - load.width, 0) == 0U);
   }
   for (const auto& store : memory_stores) {
      auto probe = native_probe{store_request(store)};
      auto* base = probe.context.memory_base;
      base[2] = 0x35;
      base[3 + store.width] = 0x6a;
      const auto value = std::uint64_t{0xfedcba9876543210};
      const auto mask = store.width == 8 ? UINT64_MAX : (std::uint64_t{1} << (8 * store.width)) - 1;
      BOOST_TEST(probe.call(0, 0, value) == (value & mask));
      BOOST_TEST(base[2] == 0x35U);
      BOOST_TEST(base[3 + store.width] == 0x6aU);
      for (std::uint32_t i = 0; i < store.width; ++i)
         BOOST_TEST(base[3 + i] == ((value >> (8 * i)) & 0xff));
   }
   auto shared = native_probe{load_request(memory_loads[0])};
   auto second_context = shared.context;
   auto second_memory = bytes(vm::page_size + 16, 0);
   second_memory[0] = second_memory.back() = 0xa5;
   second_context.memory_base = second_memory.data() + 8;
   second_context.memory_base[3] = 19;
   auto second_arguments = std::array<vm::expr_value, 1>{};
   auto second_result = vm::expr_value{};
   shared.artifact.entry(0)(&second_context, second_arguments.data(), &second_result);
   BOOST_TEST(second_result.i64 == 19);
   BOOST_TEST(shared.call(0, 0, 0) == loaded_bits(memory_loads[0]));
   BOOST_TEST(second_memory[0] == 0xa5U);
   BOOST_TEST(second_memory.back() == 0xa5U);
   for (const bool wide : {false, true}) {
      auto floats = native_probe{float_bits_request(wide)};
      const auto nan = wide ? std::uint64_t{0x7ff8000000012345} : std::uint64_t{0x7fc12345};
      BOOST_TEST(floats.call(2, 0, 32) == nan);
      BOOST_TEST(floats.call(0, 0, 32) == loaded_bits(memory_loads[wide ? 1 : 0]));
      BOOST_TEST(floats.call(1, 0, 32) == loaded_bits(memory_loads[wide ? 1 : 0]));
      auto integers = native_probe{globals_request(wide)};
      BOOST_TEST(integers.call(0, 0, 0) == 7U);
      BOOST_TEST(integers.call(1, UINT64_MAX, 0) == (wide ? UINT64_MAX : std::uint64_t{UINT32_MAX}));
      // Same immutable artifact/mapping, but distinct invocation-local globals.
      auto second = integers.context;
      auto second_globals = std::array<vm::expr_value, 1>{};
      second_globals[0].i64 = 19;
      second.globals = second_globals.data();
      auto arguments = std::array<vm::expr_value, 1>{};
      auto result = vm::expr_value{};
      integers.artifact.entry(0)(&second, arguments.data(), &result);
      BOOST_TEST(result.i64 == 19);
      BOOST_TEST(integers.call(0, 0, 0) == (wide ? UINT64_MAX : std::uint64_t{UINT32_MAX}));
   }
   auto growth = native_probe{growth_request()};
   auto* previous = growth.context.memory_base;
   BOOST_TEST(growth.call(0, 0, 0) == 1U);
   BOOST_TEST(growth.call(1, 0, 0) == 1U);
   BOOST_TEST(growth.context.memory_base == previous);
   BOOST_TEST(growth.call(2, 1, 0) == loaded_bits(memory_loads[0]));
   BOOST_TEST(growth.context.memory_base != previous);
   BOOST_TEST(growth.context.current_pages == 2U);
   BOOST_TEST(growth.context.memory_bytes == 2U * vm::page_size);
   for (std::size_t offset = vm::page_size; offset < 2U * vm::page_size; ++offset)
      BOOST_REQUIRE(growth.context.memory_base[offset] == 0);
   auto* grown = growth.context.memory_base;
   BOOST_TEST(growth.call(1, 2, 0) == UINT32_MAX);
   BOOST_TEST(growth.call(1, UINT32_MAX, 0) == UINT32_MAX);
   BOOST_TEST(growth.context.memory_base == grown);
   BOOST_TEST(growth.call(0, 0, 0) == 2U);
   BOOST_TEST(growth.call(2, 0, 0) == loaded_bits(memory_loads[0]));
   auto direct_growth = native_probe{growth_request()};
   auto* direct_previous = direct_growth.context.memory_base;
   BOOST_TEST(direct_growth.call(3, 1, 0) == loaded_bits(memory_loads[0]));
   BOOST_TEST(direct_growth.context.memory_base != direct_previous);
   auto execution_limited = growth_request();
   execution_limited.configuration.execution.max_pages = 1;
   auto limited = native_probe{execution_limited};
   BOOST_TEST(limited.call(1, 1, 0) == UINT32_MAX);
   BOOST_TEST(limited.call(0, 0, 0) == 1U);
   auto empty = native_probe{growth_request(0)};
   BOOST_TEST(empty.call(0, 0, 0) == 0U);
   BOOST_TEST(empty.call(1, 1, 0) == 0U);
   BOOST_TEST(empty.call(0, 0, 0) == 1U);
   auto started = native_probe{start_initialization_request()};
   BOOST_TEST(started.globals[0].i64 == 7);
   BOOST_TEST(started.context.memory_base[0] == 0xa5U);
   started.call(started.artifact.image().start, 0, 0); // Explicit owner start path, not executor emulation.
   BOOST_TEST(started.call(1, 0, 0) == 22U);
}

BOOST_AUTO_TEST_CASE(native_fp_raw_transport_calls_the_actual_shared_numeric_owner_primitive) {
   for (const auto& fixture : numeric_cases()) {
      auto probe = native_probe{numeric_request(fixture)};
      const auto first = fixture.input == vm::f32 ? std::uint64_t{std::bit_cast<std::uint32_t>(13.25F)} :
                         fixture.input == vm::f64 ? std::bit_cast<std::uint64_t>(13.25) : std::uint64_t{13};
      const auto second = fixture.input == vm::f32 ? std::uint64_t{std::bit_cast<std::uint32_t>(3.5F)} :
                                                  std::bit_cast<std::uint64_t>(3.5);
      const auto arguments = std::array{std::bit_cast<vm::expr_value>(first), std::bit_cast<vm::expr_value>(second)};
      const auto expected = std::bit_cast<std::uint64_t>(
          vm::execute_numeric(fixture.operation, std::span{arguments}.first(fixture.arity)));
      BOOST_TEST(probe.call(0, first, second) == expected);
      BOOST_TEST(probe.call(1, first, second) == expected);
      BOOST_TEST(probe.numerics == 2U);
   }
   for (const bool wide : {false, true}) {
      const auto type = static_cast<std::uint8_t>(wide ? vm::f64 : vm::f32);
      const auto reinterpret = static_cast<std::uint8_t>(wide ? vm::i64_reinterpret_f64 : vm::i32_reinterpret_f32);
      const auto constant = wide ? std::uint64_t{0xfff0000000012345} : std::uint64_t{0xffa12345};
      auto body = bytes{0, static_cast<std::uint8_t>(wide ? vm::f64_const : vm::f32_const)};
      for (unsigned i = 0; i < (wide ? 8U : 4U); ++i)
         body.push_back(static_cast<std::uint8_t>(constant >> (8 * i)));
      body.insert(body.end(), {reinterpret, 0x0b});
      auto constant_probe = native_probe{functions_request({}, wide ? 0x7e : 0x7f, {body})};
      BOOST_TEST(constant_probe.call(0, 0, 0) == constant);
      auto zero_probe = native_probe{functions_request({}, type, {{1, 1, type, 0x20, 0, 0x0b}})};
      BOOST_TEST(zero_probe.call(0, 0, 0) == 0U);
      // Typed branch/result merges and mixed parameters preserve raw NaN bits.
      auto branch_body = bytes{0, 0x20, 0, 0x04, type, 0x20, 1, 0x05,
                                static_cast<std::uint8_t>(wide ? vm::f64_const : vm::f32_const)};
      const auto negative_zero = wide ? std::uint64_t{0x8000000000000000} : std::uint64_t{0x80000000};
      for (unsigned i = 0; i < (wide ? 8U : 4U); ++i)
         branch_body.push_back(static_cast<std::uint8_t>(negative_zero >> (8 * i)));
      branch_body.insert(branch_body.end(), {0x0b, 0x0b});
      auto branch = native_probe{functions_request({0x7f, type}, type, {branch_body})};
      BOOST_TEST(branch.call(0, 1, constant) == constant);
      BOOST_TEST(branch.call(0, 0, constant) == negative_zero);
   }
}

BOOST_AUTO_TEST_CASE(native_import_thunks_and_mixed_indirect_calls_are_raw_abi_primitives_only) {
   auto probe = native_probe{import_indirect_request()};
   for (const auto index : {0U, 4U}) {
      BOOST_TEST(probe.call(index, 19, 5) == 9U);
      BOOST_TEST(probe.last_import == 1U);
   }
   for (const auto index : {1U, 5U}) {
      BOOST_TEST(probe.call(index, 19, 5) == 34U);
      BOOST_TEST(probe.last_import == 0U);
   }
   BOOST_TEST(probe.host_effects == 4U);
   BOOST_TEST(probe.call(3, 19, 5, 0) == 9U);
   BOOST_TEST(probe.last_import == 1U);
   BOOST_TEST(probe.call(3, 19, 5, 1) == 34U);
   BOOST_TEST(probe.last_import == 0U);
   BOOST_TEST(probe.call(3, 19, 5, 2) == 14U);
   BOOST_TEST(probe.host_effects == 6U); // Defined target does not enter host dispatch.
   BOOST_TEST(probe.call(2, 19, 5) == 14U);
   BOOST_TEST(probe.call(6, 0, 0) == 0U);

   auto raw = native_probe{imported_entry_request({vm::i32, vm::f64}, vm::f32)};
   raw.callbacks.host_call = +[](oc::invocation_abi* context, std::uint32_t index,
                                const vm::expr_value* arguments, vm::expr_value* result) noexcept {
      auto& probe = *static_cast<native_probe*>(context->owner);
      if (index || static_cast<std::uint32_t>(arguments[0].i64) != 19U ||
          std::bit_cast<std::uint64_t>(arguments[1].i64) != 0x7ff0000000012345ULL)
         return oc::trap_reason::host_exception;
      ++probe.host_effects;
      result->i64 = std::bit_cast<std::int64_t>(std::uint64_t{0xdeadbeef7f812345});
      return oc::trap_reason::none;
   };
   BOOST_TEST(raw.call(0, 19, 0x7ff0000000012345ULL) == 0x7f812345U);
   BOOST_TEST(raw.host_effects == 1U);
   for (const auto& request : {imported_entry_request({}, 0, true), imported_entry_request({}, vm::i32)}) {
      auto empty = native_probe{request};
      empty.callbacks.host_call = +[](oc::invocation_abi* context, std::uint32_t index,
                                      const vm::expr_value*, vm::expr_value* result) noexcept {
         auto& probe = *static_cast<native_probe*>(context->owner);
         if (index)
            return oc::trap_reason::host_exception;
         ++probe.host_effects;
         if (!probe.artifact.image().configuration.imports[index].signature.ret.empty())
            result->i64 = 27;
         return oc::trap_reason::none;
      };
      BOOST_TEST(empty.call(0, 0, 0) == (request.configuration.imports[0].signature.ret.empty() ? 0U : 27U));
      BOOST_TEST(empty.host_effects == 1U);
      BOOST_TEST(empty.artifact.image().start == (request.configuration.imports[0].signature.ret.empty() ? 0U : UINT32_MAX));
   }
}

BOOST_AUTO_TEST_CASE(native_indirect_software_guards_precede_dereferences_and_host_effects_primitive) {
   if (const auto* mode = fresh_process::mode()) {
      fresh_process::disable_core_dumps();
      const auto control = std::strtoul(mode, nullptr, 10);
      auto probe = native_probe{import_indirect_request()};
      auto functions = std::vector<oc::native_function>{probe.artifact.functions().begin(),
                                                        probe.artifact.functions().end()};
      probe.context.functions = functions.data();
      auto selector = std::uint32_t{0};
      auto entry = std::uint32_t{3};
      switch (control) {
      case 0: selector = 5; probe.context.table = nullptr; break;
      case 1: selector = UINT32_MAX; probe.context.table = nullptr; break;
      case 2: selector = 3; break; // Null slot.
      case 3: selector = 4; break; // A real function with the wrong structural signature.
      case 4: probe.table[0].function_index = UINT32_MAX - 1; break;
      case 5: functions[0].type_index = 2; break;
      case 6: functions[0].entry = nullptr; break;
      case 7: entry = 0; [[fallthrough]];
      case 8:
         probe.callbacks.host_call = +[](oc::invocation_abi*, std::uint32_t, const vm::expr_value*,
                                         vm::expr_value* result) noexcept {
            result->i64 = 0x12345;
            return oc::trap_reason::host_exception;
         };
         break;
      default: std::_Exit(91);
      }
      probe.callbacks.trap = +[](oc::invocation_abi* context, oc::trap_reason reason) noexcept {
         const auto& probe = *static_cast<native_probe*>(context->owner);
         // A hardware fault, timeout, return, or any host side effect cannot
         // masquerade as the generated software guard's expected typed exit.
         std::_Exit(probe.host_effects ? 90 : 32 + static_cast<int>(reason));
      };
      probe.call(entry, 19, 5, selector);
      std::_Exit(99);
   }
   const auto expected = std::array{oc::trap_reason::table_bounds, oc::trap_reason::table_bounds,
                                   oc::trap_reason::table_bounds, oc::trap_reason::indirect_type,
                                   oc::trap_reason::table_bounds, oc::trap_reason::indirect_type,
                                   oc::trap_reason::indirect_type, oc::trap_reason::host_exception,
                                   oc::trap_reason::host_exception};
   for (std::size_t control = 0; control < expected.size(); ++control) {
      const auto child = fresh_process::run(
          "optimizing_compiler/native_indirect_software_guards_precede_dereferences_and_host_effects_primitive",
          std::to_string(control));
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status)); // WIFSIGNALED is always a failure, not recovery evidence.
      BOOST_TEST(WEXITSTATUS(child.status) == 32 + static_cast<int>(expected[control]));
   }
}

#if defined(__linux__) && defined(__x86_64__)
BOOST_AUTO_TEST_CASE(native_memory_bounds_are_explicit_traps_before_pointer_access_primitive) {
   const auto check = [](const oc::compile_request& request, std::uint32_t address) {
      const auto child = fork();
      BOOST_REQUIRE(child >= 0);
      if (child == 0) {
         std::signal(SIGALRM, SIG_DFL);
         alarm(5);
         auto probe = native_probe{request};
         probe.callbacks.trap =
             +[](oc::invocation_abi*, oc::trap_reason reason) noexcept { std::_Exit(32 + static_cast<int>(reason)); };
         probe.call(0, address, UINT64_MAX);
         std::_Exit(99);
      }
      int status = 0;
      BOOST_REQUIRE(waitpid(child, &status, 0) == child);
      BOOST_REQUIRE(WIFEXITED(status)); // A native hardware fault is NOT a bounds-test success.
      BOOST_TEST(WEXITSTATUS(status) == 32 + static_cast<int>(oc::trap_reason::memory_bounds));
   };
   for (const auto& load : memory_loads)
      check(load_request(load), vm::page_size - 3 - load.width + 1);
   for (const auto& store : memory_stores)
      check(store_request(store), vm::page_size - 3 - store.width + 1);
   check(load_request(memory_loads[0], UINT32_MAX), UINT32_MAX);
   check(load_request(memory_loads[0], 0), UINT32_MAX);
   check(load_request(memory_loads[0], 0, 0, 0), 0);
}

BOOST_AUTO_TEST_CASE(native_division_traps_and_backward_edge_cancellation_are_bounded_primitive) {
   const auto check_child = [&](const oc::compile_request& request, std::uint64_t left, std::uint64_t right,
                                oc::trap_reason expected, bool cancel = false) {
      const auto child = fork();
      BOOST_REQUIRE(child >= 0);
      if (child == 0) {
         std::signal(SIGALRM, SIG_DFL);
         alarm(5);
         auto probe = native_probe{request};
         probe.callbacks.trap =
             +[](oc::invocation_abi*, oc::trap_reason reason) noexcept { std::_Exit(32 + static_cast<int>(reason)); };
         if (cancel)
            probe.context.epoch_deadline = 0; // Deliberately keep this old oracle on the slow path.
         if (cancel)
            probe.callbacks.poll = +[](oc::invocation_abi* invocation) noexcept {
               auto& probe = *static_cast<native_probe*>(invocation->owner);
               return ++probe.polls > 10 ? oc::trap_reason::cancelled : oc::trap_reason::none;
            };
         probe.call(0, left, right);
         std::_Exit(99);
      }
      int status = 0;
      BOOST_REQUIRE(waitpid(child, &status, 0) == child);
      BOOST_REQUIRE(WIFEXITED(status));
      BOOST_TEST(WEXITSTATUS(status) == 32 + static_cast<int>(expected));
   };
   for (const auto operation : {vm::i32_div_s, vm::i32_div_u, vm::i32_rem_s, vm::i32_rem_u})
      check_child(integer_request(static_cast<std::uint8_t>(operation)), 1, 0, oc::trap_reason::integer_divide_by_zero);
   check_child(integer_request(0x6d), 0x80000000U, UINT32_MAX, oc::trap_reason::integer_overflow);
   for (const auto operation : {vm::i64_div_s, vm::i64_div_u, vm::i64_rem_s, vm::i64_rem_u}) {
      const auto request =
          functions_request({0x7e, 0x7e}, 0x7e, {{0, 0x20, 0, 0x20, 1, static_cast<std::uint8_t>(operation), 0x0b}});
      check_child(request, 1, 0, oc::trap_reason::integer_divide_by_zero);
      if (operation == vm::i64_div_s)
         check_child(request, std::uint64_t{0x8000000000000000}, UINT64_MAX, oc::trap_reason::integer_overflow);
   }
   const auto loop = functions_request({}, 0, {{0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b}});
   check_child(loop, 0, 0, oc::trap_reason::cancelled, true);
}
#endif
#endif

BOOST_AUTO_TEST_SUITE_END()
