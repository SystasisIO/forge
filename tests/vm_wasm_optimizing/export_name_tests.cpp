#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "../vm_wasm_execution/export_name_fixtures.hxx"

#if defined(FORGE_WAMR_EXPORT_NAME_COMPILER_FIXTURE)
import forge.vm.wasm.compiler.compiler;
#else
import forge.asio.blocking;
import forge.asio.runtime;
import forge.vm.wasm.optimizing.executor;
#endif

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
namespace fixture = export_name_fixtures;

#if defined(FORGE_WAMR_EXPORT_NAME_COMPILER_FIXTURE)
void write(const std::string& path, const std::vector<std::uint8_t>& bytes) {
   auto output = std::ofstream{path, std::ios::binary};
   output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
   if (!output)
      throw std::runtime_error("Cannot write export-name compiler fixture");
}

void generate(const std::string& prefix) {
   const auto fixtures = std::vector<std::pair<std::string, fixture::bytes>>{
       {"distinct", fixture::module(fixture::many_names())},
       {"hook-suffix", fixture::hook(fixture::suffixed("__post_instantiate"))},
       {"hook-exact", fixture::hook("__post_instantiate")}};
   for (const auto& [label, bytes] : fixtures) {
      auto request = oc::compile_request{};
      request.wasm = bytes;
      request.configuration.target = oc::target_profile::native();
      request.configuration.limits = {8 * 1024 * 1024, 64 * 1024};
      request.configuration = oc::canonicalize(std::move(request.configuration));
      const auto code = forge::vm::wasm::compiler::compiler{}.compile(request);
      write(prefix + "-" + label + ".foc", code.encode());
      write(prefix + "-" + label + ".request", request.encode());
   }
}
#else
using namespace std::chrono_literals;
void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const std::string& path) {
   auto input = std::ifstream{path, std::ios::binary};
   if (!input)
      throw std::runtime_error("Cannot read export-name compiler fixture");
   return {std::istreambuf_iterator<char>{input}, {}};
}

oc::artifact load(const std::string& prefix, const std::string& label, const fixture::bytes& source) {
   const auto request = oc::compile_request::decode(read(prefix + "-" + label + ".request"), 16 * 1024 * 1024);
   require(request.wasm == source, "AOT export-name fixture has foreign source bytes");
   auto code = oc::artifact::decode(read(prefix + "-" + label + ".foc"), oc::identify(request), 32 * 1024 * 1024,
                                    oc::artifact::origin::isolated_compiler);
   code.prepare();
   require(code.is_prepared() && !code.native_code().empty(), "Export-name fixture has no prepared native code");
   return code;
}

void run(const std::string& prefix) {
   using registered = vm::registered_host_functions<vm::standalone_function_t>;
   const auto names = fixture::many_names();
   require(names.size() > 32, "AOT fixture does not exercise the large export path");
   auto code = load(prefix, "distinct", fixture::module(names));
   auto suffix = load(prefix, "hook-suffix", fixture::hook(fixture::suffixed("__post_instantiate")));
   auto exact = load(prefix, "hook-exact", fixture::hook("__post_instantiate"));
   const auto binding = oc::executor::bind<registered>(code);
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto executor = oc::executor{scheduler, {8 * 1024 * 1024, 64 * 1024}};
   forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 5s));
   auto failure = std::exception_ptr{};
   auto prepared_thread = false;
   try {
      executor.prepare_thread();
      prepared_thread = true;
      auto memory = vm::wasm_allocator{};
      require(code.exports().size() == names.size(), "AOT export count changed");
      const auto projected = code.runtime_module()->function_exports();
      require(projected.size() == names.size(), "Prepared AOT source export count changed");
      for (std::size_t i = 0; i < names.size(); ++i) {
         const auto found = std::ranges::find(code.exports(), names[i], &oc::compiled_export::name);
         require(found != code.exports().end() && found->function_index == i,
                 "AOT artifact truncated or aliased an export name");
         require(std::count(projected.begin(), projected.end(), names[i]) == 1 &&
                     code.runtime_module()->get_exported_function(names[i]) == i,
                 "Prepared AOT source truncated or aliased an export name");
         const auto result = executor.invoke(binding, nullptr, found->function_index, {}, memory,
                                             std::chrono::steady_clock::now() + 5s);
         require(result.trap == oc::trap_reason::none && !result.host_error && result.value &&
                     result.value->to_ui32() == i + 1,
                 "AOT export resolved to the wrong native function");
      }
      // Each invoke gets fresh guest state. The hook and read run in the same initialization.
      for (const auto* hook : {&suffix, &exact}) {
         const auto hooks = oc::executor::bind<registered>(*hook);
         const auto value = executor.invoke(hooks, nullptr, hook->runtime_module()->get_exported_function("read"), {},
                                            memory, std::chrono::steady_clock::now() + 5s);
         const auto expected = hook == &suffix ? 0U : 1U;
         require(value.trap == oc::trap_reason::none && !value.host_error && value.value &&
                     value.value->to_ui32() == expected,
                 "AOT hook suffix or its active positive control is incorrect");
      }
   } catch (...) {
      failure = std::current_exception();
   }
   if (prepared_thread)
      executor.release_thread();
   forge::asio::blocking::run(runtime, executor.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   if (failure)
      std::rethrow_exception(failure);
}
#endif
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 2)
         return 2;
#if defined(FORGE_WAMR_EXPORT_NAME_COMPILER_FIXTURE)
      generate(argv[1]);
      std::cout << "PASS export-name fixtures compiled and encoded\n";
#else
      run(argv[1]);
      std::cout << "PASS real AOT export-name compile/decode/prepare/call and hook controls\n";
#endif
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   }
}
