#include "../vm_wasm_execution/fixtures.hxx"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(FORGE_WAMR_CAPACITY_BASELINE)
import forge.vm.wasm.execution.compiled_module;
#else
#include "details/engine_module.hxx"
#include "details/engine_limits.hxx"
#include "details/engine_instance.hxx"
import forge.vm.wasm.interpret.options;
import forge.vm.wasm.interpret.capacity_profile;
#endif

namespace {
fixtures::bytes capacity_fixture(std::size_t locals) {
   fixtures::bytes module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(module, 1, {1, 0x60, 3, 0x7e, 0x7e, 0x7e, 0});
   fixtures::section(module, 3, {1, 0});
   fixtures::bytes exports{1};
   fixtures::text(exports, "apply");
   exports.insert(exports.end(), {0, 0});
   fixtures::section(module, 7, exports);
   fixtures::bytes body{1};
   fixtures::size(body, locals);
   body.insert(body.end(), {0x7f, 0x41, 0, 0x21});
   fixtures::size(body, locals + 2);
   body.push_back(0x0b);
   fixtures::bytes code{1};
   fixtures::size(code, body.size());
   code.insert(code.end(), body.begin(), body.end());
   fixtures::section(module, 10, code);
   return module;
}

void require(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}

void write(const std::filesystem::path& path, const fixtures::bytes& module) {
   std::ofstream out(path, std::ios::binary);
   out.write(reinterpret_cast<const char*>(module.data()), module.size());
   out.close();
   require(static_cast<bool>(out), "Cannot write capacity fixture");
}

#if !defined(FORGE_WAMR_CAPACITY_BASELINE)
namespace vm = forge::vm::wasm::interpret;
namespace detail = vm::detail;

struct memory_case {
   const char* name;
   std::uint32_t initial;
   std::optional<std::uint32_t> maximum;
};

constexpr auto memory_cases() {
   constexpr auto cap = vm::capacity_profile::max_pages;
   static_assert(cap == std::numeric_limits<std::uint32_t>::max() / 65536u);
   return std::array{
      memory_case{"memory-zero", 0, 1},
      memory_case{"memory-cap", cap, cap},
      memory_case{"memory-over", cap + 1, cap + 1},
      memory_case{"memory-declared-max", 1, cap + 1},
      memory_case{"memory-no-max", 1, std::nullopt}};
}

fixtures::bytes memory_fixture(const memory_case& item) {
   fixtures::bytes module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(module, 1, {1, 0x60, 0, 0});
   fixtures::section(module, 3, {1, 0});
   fixtures::bytes memory{1, static_cast<std::uint8_t>(item.maximum.has_value())};
   fixtures::size(memory, item.initial);
   if (item.maximum) fixtures::size(memory, *item.maximum);
   fixtures::section(module, 5, memory);
   fixtures::section(module, 10, {1, 2, 0, 0x0b});
   return module;
}

std::shared_ptr<detail::engine_module> load(const fixtures::bytes& module) {
   return std::make_shared<detail::engine_module>(detail::engine_runtime::acquire(), module,
      std::make_shared<detail::engine_bindings>(std::vector<detail::engine_bindings::function>{}));
}

fixtures::bytes read(const std::filesystem::path& path) {
   std::ifstream input(path, std::ios::binary);
   require(static_cast<bool>(input), "Cannot read profile fixture");
   fixtures::bytes result{std::istreambuf_iterator<char>(input), {}};
   require(!input.bad(), "Cannot read complete profile fixture");
   return result;
}

void memory_boundaries(const std::filesystem::path* directory = nullptr) {
   for (const auto& item : memory_cases()) {
      const auto expected = memory_fixture(item);
      const auto source = directory ? read(*directory / (std::string(item.name) + ".wasm")) : expected;
      require(source == expected, "Wrong source memory fixture");
      const auto loaded = load(source);
      require(source == expected, "Admission changed memory source bytes");
      const detail::engine_limits measured(loaded->get());
      require(measured.get(detail::engine_limits::metric::pages) == item.initial,
         "Initial page metric wrapped or used the declared maximum");
      bool rejected = false;
      try { measured.check_profile<vm::capacity_profile>(); }
      catch (const detail::engine_limits::exceeded& error) {
         require(error.field == detail::engine_limits::metric::pages &&
            error.actual == item.initial && error.allowed == vm::capacity_profile::max_pages,
            "Wrong typed initial-page refusal");
         rejected = true;
      }
      require(rejected == (item.initial > vm::capacity_profile::max_pages),
         "Source memory admission differs from the common page profile");
   }
   // Declaration admission only: never instantiate the nearly 4 GiB cases.
}

void invoke_control(std::shared_ptr<detail::engine_module> loaded) {
   // One shallow call; this is not a production stack-size/depth qualification.
   detail::engine_instance instance(std::move(loaded), 1024 * 1024, {}, nullptr, 1, 1);
   std::array<std::uint32_t, 6> arguments{};
   require(instance.invoke("apply", arguments.size(), arguments), "Accepted profile control did not execute");
}

void boundary(const fixtures::bytes& source, const fixtures::bytes& runnable, std::uint64_t budget) {
   const auto original = source;
   auto loaded = load(source);
   require(source == original, "Interpreter changed caller-owned WASM bytes");
   const detail::engine_limits measured(loaded->get());
   require(measured.function_bytes(6) == budget, "Wrong locals-plus-operands accounting");
   require(measured.function_bytes(7) == budget + 24, "Parameters lost from combined accounting");
   vm::options options{};
   options.max_code_bytes = UINT32_MAX;
   options.max_local_sets = UINT32_MAX;
   options.max_nested_structures = UINT32_MAX;
   options.max_br_table_elements = UINT32_MAX;
   options.max_memory_offset = UINT32_MAX;
   options.max_func_local_bytes = budget;
   measured.check_instructions(options);
   --options.max_func_local_bytes;
   bool rejected = false;
   try { measured.check_instructions(options); }
   catch (const detail::engine_limits::exceeded& error) {
      require(error.field == detail::engine_limits::metric::function_bytes &&
         error.actual == budget && error.allowed == budget - 1,
         "Wrong typed caller-budget rejection");
      rejected = true;
   }
   require(rejected, "Caller function budget was ignored");
   if (source != runnable) loaded = load(runnable);
   invoke_control(std::move(loaded));
}

void rejected_capacity(const fixtures::bytes& module) {
   const auto original = module;
   bool rejected = false;
   try { (void)load(module); }
   catch (const std::runtime_error& error) {
      // Private loader diagnosis only. Public typed profile rejection is a separate gate.
      require(std::string_view(error.what()) ==
         "WASM load failed: WASM module load failed: local count too large",
         "Capacity fixture failed for an unrelated reason");
      rejected = true;
   }
   require(rejected, "Unsupported 65536-local fixture was admitted");
   require(module == original, "Rejection changed caller-owned WASM bytes");
}

void profile_boundaries(const fixtures::bytes* runnable = nullptr) {
   constexpr auto cap = vm::capacity_profile::max_function_storage_bytes;
   static_assert(cap >= 28 && cap % 4 == 0);
   const auto exact = load(capacity_fixture((cap - 28) / 4));
   const detail::engine_limits accepted(exact->get());
   require(accepted.function_bytes(7) == cap, "Wrong combined profile boundary accounting");
   accepted.check_profile<vm::capacity_profile>();
   const auto over = load(capacity_fixture((cap - 28) / 4 + 1));
   const detail::engine_limits refused(over->get());
   // Caller flags cannot bypass the independent common hard envelope.
   vm::options none{};
   none.max_code_bytes = UINT32_MAX;
   none.max_local_sets = UINT32_MAX;
   none.max_nested_structures = UINT32_MAX;
   none.max_br_table_elements = UINT32_MAX;
   none.max_memory_offset = UINT32_MAX;
   none.max_func_local_bytes_flags = static_cast<vm::max_func_local_bytes_flags_t>(0);
   none.max_func_local_bytes = 0;
   refused.check_instructions(none);
   bool rejected = false;
   try { refused.check_profile<vm::capacity_profile>(); }
   catch (const detail::engine_limits::exceeded& error) {
      require(error.field == detail::engine_limits::metric::function_storage_bytes &&
         error.actual == cap + 4 && error.allowed == cap, "Wrong common profile rejection");
      rejected = true;
   }
   require(rejected, "Caller accounting flags bypassed the common profile");
   invoke_control(runnable ? load(*runnable) : exact);
}
#endif
}

int main(int argc, char** argv) {
   try {
      const auto module = capacity_fixture(65536);
      if (argc == 3 && std::string_view(argv[1]) == "--fixtures") {
         const std::filesystem::path directory(argv[2]);
         write(directory / "capacity.wasm", module);
         write(directory / "boundary-8192.wasm", capacity_fixture(8192 / 4 - 1));
         write(directory / "boundary-16384.wasm", capacity_fixture(16384 / 4 - 1));
#if !defined(FORGE_WAMR_CAPACITY_BASELINE)
         constexpr auto cap = vm::capacity_profile::max_function_storage_bytes;
         write(directory / "profile-cap.wasm", capacity_fixture((cap - 28) / 4));
         write(directory / "profile-over.wasm", capacity_fixture((cap - 28) / 4 + 1));
         for (const auto& item : memory_cases())
            write(directory / (std::string(item.name) + ".wasm"), memory_fixture(item));
#endif
         return 0;
      }
#if defined(FORGE_WAMR_CAPACITY_BASELINE)
      require(argc <= 2, "Expected optional historical baseline output path");
      if (argc == 2) write(argv[1], module);
      namespace execution = forge::vm::wasm::execution;
      auto compiled = execution::compiled_module(module);
      (void)compiled.invoke({});
      std::cout << "HISTORICAL BASELINE ACCEPTED: 65536 i32 locals; not the WAMR profile\n";
#else
      if (argc == 3 && std::string_view(argv[1]) == "--memory-profile") {
         const std::filesystem::path directory(argv[2]);
         memory_boundaries(&directory);
         std::cout << "PASS source-only initial-page boundaries, declared/no max and zero; no guest allocation\n";
         return 0;
      }
      if (argc == 4 && std::string_view(argv[1]) == "--profile") {
         constexpr auto cap = vm::capacity_profile::max_function_storage_bytes;
         require(read(argv[2]) == capacity_fixture((cap - 28) / 4), "Wrong profile-cap source fixture");
         const auto runnable = read(argv[3]);
         profile_boundaries(&runnable);
         std::cout << "PASS combined profile-cap source metric and executable control: " << cap << '\n';
         return 0;
      }
      if (argc == 5 && std::string_view(argv[1]) == "--boundary") {
         const auto budget = std::string_view(argv[4]);
         require(budget == "8192" || budget == "16384", "Unknown boundary budget");
         boundary(read(argv[2]), read(argv[3]), budget == "8192" ? 8192 : 16384);
         std::cout << "PASS source caller-budget admission and executable control: " << budget << '\n';
         return 0;
      }
      require(argc <= 2, "Expected optional WASM output path, --fixtures directory, --memory-profile directory, --profile source runnable or --boundary source runnable budget");
      if (argc == 2) write(argv[1], module);
      memory_boundaries();
      for (const auto budget : {8192U, 16384U}) {
         const auto control = capacity_fixture(budget / 4 - 1);
         boundary(control, control, budget);
      }
      profile_boundaries();
      rejected_capacity(module);
      const auto recovery = capacity_fixture(8192 / 4 - 1);
      boundary(recovery, recovery, 8192);
      std::cout << "PASS expected 65536-local loader rejection; source page boundaries, 8/16 KiB controls, caller budgets and recovery\n";
#endif
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL capacity profile: " << error.what() << '\n';
      return 1;
   }
}
