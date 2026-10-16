module;

#include "fresh_process.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <boost/asio/post.hpp>
#include <boost/test/unit_test.hpp>
#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach.h>
#elif defined(__linux__) && defined(__x86_64__)
#include <pthread.h>
#include <ucontext.h>
#endif

module forge.vm.wasm.optimizing.executor;

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.raw.raw;
import forge.vm.wasm.interpret.backend;

#include "details/native_stack.hxx"

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;
constexpr std::uint64_t guest_reserve_bytes = 4 * 1024 * 1024;

#if defined(__linux__) && defined(__x86_64__)
// Fresh-child observation only, never a VM fault/recovery handler. This sole
// selected child owns SIGUSR2; the handler only publishes the actual interrupted
// PC, without TLS lookup, allocation, logging, locking or exceptions.
std::atomic<std::uintptr_t> sampled_native_pc{0};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
void sample_native_pc(int, siginfo_t*, void* raw) noexcept {
   const auto& context = *static_cast<const ucontext_t*>(raw);
   sampled_native_pc.store(static_cast<std::uintptr_t>(context.uc_mcontext.gregs[REG_RIP]), std::memory_order_release);
}
#endif

struct compiled_host {
   unsigned effects = 0;
   unsigned destroyed = 0;
   std::stop_source* stop = nullptr;
   bool slow = false;
   std::uint64_t checksum = 0;
   void printui(std::uint64_t value) {
      checksum = value;
      ++effects;
   }
   std::uint32_t a(std::uint32_t lhs, std::uint32_t rhs) {
      const auto cleanup = vm::scope_guard{[&] { ++destroyed; }};
      ++effects;
      return lhs + rhs + 10;
   }
   std::uint32_t z(std::uint32_t lhs, std::uint32_t rhs) {
      const auto cleanup = vm::scope_guard{[&] { ++destroyed; }};
      ++effects;
      return lhs - rhs - 5;
   }
   void tick() {
      const auto cleanup = vm::scope_guard{[&] { ++destroyed; }};
      ++effects;
      if (slow)
         std::this_thread::sleep_for(15ms);
      if (stop)
         stop->request_stop();
   }
};
using compiled_registry = vm::registered_host_functions<compiled_host>;

std::vector<std::uint8_t> read_fixture(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
   if (!input)
      throw std::runtime_error{"required immutable compiler fixture unavailable"};
   const auto size = input.tellg();
   if (size <= 0 || size > 2 * 1024 * 1024)
      throw std::runtime_error{"compiler fixture size outside bounded test input"};
   auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
   input.seekg(0);
   if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
      throw std::runtime_error{"compiler fixture read incomplete"};
   return bytes;
}

void register_compiled_host() {
   static const auto registration = [] {
      compiled_registry::add<&compiled_host::a>("env", "a");
      compiled_registry::add<&compiled_host::z>("env", "z");
      compiled_registry::add<&compiled_host::tick>("env", "tick");
      compiled_registry::add<&compiled_host::printui>("env", "printui");
      return true;
   }();
   static_cast<void>(registration);
}

oc::artifact load_fixture(const char* name) {
   register_compiled_host();
   const auto* directory = std::getenv("FORGE_OC_RUNTIME_FIXTURE_DIRECTORY");
   if (directory == nullptr || !std::filesystem::path{directory}.is_absolute())
      throw std::runtime_error{"explicit immutable fixture directory is required for this selected gate"};
#if defined(__APPLE__) && defined(__aarch64__)
   constexpr auto suffix = "-darwin-arm64";
#elif defined(__linux__) && defined(__x86_64__)
   constexpr auto suffix = "-linux-x86-64";
#else
#error Compiler runtime fixtures require a supported native profile.
#endif
   const auto prefix = std::filesystem::path{directory} / (std::string{name} + suffix);
   const auto request_bytes = read_fixture(prefix.string() + ".request");
   const auto artifact_bytes = read_fixture(prefix.string() + ".foc");
   const auto request = oc::compile_request::decode(request_bytes, request_bytes.size());
   auto code = oc::artifact::decode(artifact_bytes, oc::identify(request), artifact_bytes.size());
   BOOST_TEST(code.encode() == artifact_bytes);
   BOOST_CHECK(code.image().configuration == oc::canonicalize(request.configuration));
   BOOST_TEST(!code.is_prepared());
   code.prepare(); // The SAME artifact owns the ONLY mapping, before invocation.
   BOOST_TEST_MESSAGE("real compiler fixture=" << name << " identity=" << code.identity().str());
   return code;
}

struct compiled_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime};
   oc::executor executor;
   vm::wasm_allocator memory;
   compiled_host host;
   explicit compiled_fixture(std::uint64_t physical_capacity = guest_reserve_bytes + 1024 * 1024)
       : executor{scheduler, {physical_capacity, 64 * 1024}} {
      forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 1s));
      executor.prepare_thread();
   }
   ~compiled_fixture() {
      executor.release_thread();
      forge::asio::blocking::run(runtime, executor.shutdown());
      forge::asio::blocking::run(runtime, scheduler.shutdown());
   }
};

struct benchmark_fixture {
   oc::artifact code;
   std::array<std::uint64_t, 4> inputs;
};

std::filesystem::path benchmark_directory() {
   const auto* configured = std::getenv("FORGE_OC_BENCHMARK_CORPUS_DIRECTORY");
   BOOST_REQUIRE_MESSAGE(configured != nullptr, "explicit immutable benchmark corpus is required");
   const auto directory = std::filesystem::path{configured};
   BOOST_REQUIRE(directory.is_absolute());
   BOOST_REQUIRE(std::filesystem::is_directory(directory));
   for (const auto name : {"benchmark-integer", "benchmark-memory"}) {
      for (const auto suffix : {".wasm", ".inputs", "-linux-x86-64.request", "-linux-x86-64.foc",
                                "-darwin-arm64.request", "-darwin-arm64.foc"}) {
         const auto path = directory / (std::string{name} + suffix);
         BOOST_REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)));
         const auto size = std::filesystem::file_size(path);
         BOOST_REQUIRE((size > 0 && size <= 2 * 1024 * 1024));
      }
   }
   auto count = 0U;
   for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator{directory})
      BOOST_REQUIRE(++count <= 12U);
   BOOST_REQUIRE_EQUAL(count, 12U);
   return directory;
}

benchmark_fixture load_benchmark_fixture(const char* name) {
   register_compiled_host();
   const auto prefix = benchmark_directory() / name;
   const auto wasm = read_fixture(prefix.string() + ".wasm");
   const auto input_bytes = read_fixture(prefix.string() + ".inputs");
   BOOST_REQUIRE_EQUAL(input_bytes.size(), 32U);
   const auto inputs = forge::raw::unpack_exact<std::array<std::uint64_t, 4>>(input_bytes);
   auto selected = std::optional<oc::artifact>{};
   for (const auto suffix : {"-linux-x86-64", "-darwin-arm64"}) {
      const auto request_bytes = read_fixture(prefix.string() + suffix + ".request");
      const auto artifact_bytes = read_fixture(prefix.string() + suffix + ".foc");
      const auto request = oc::compile_request::decode(request_bytes, request_bytes.size());
      BOOST_REQUIRE(request.wasm == wasm);
      const auto checked = oc::artifact::decode(artifact_bytes, oc::identify(request), artifact_bytes.size());
      BOOST_REQUIRE((checked.image().configuration == oc::canonicalize(request.configuration)));
      BOOST_REQUIRE_EQUAL(checked.image().configuration.imports.size(), 1U);
      const auto& imported = checked.image().configuration.imports[0];
      BOOST_REQUIRE(imported.module == "env");
      BOOST_REQUIRE(imported.name == "printui");
      BOOST_REQUIRE((imported.signature == vm::host_function{{vm::i64}, {}}));
      BOOST_REQUIRE_EQUAL(checked.exports().size(), 1U);
      BOOST_REQUIRE(checked.exports()[0].name == "apply");
      BOOST_REQUIRE(checked.exports()[0].function_index == 1U);
      BOOST_REQUIRE(!checked.is_prepared());
      if (request.configuration.target.matches_host()) {
         BOOST_REQUIRE(!selected.has_value());
         selected.emplace(checked);
      }
   }
   BOOST_REQUIRE_MESSAGE(selected.has_value(), "corpus has no exact current host profile");
   selected->prepare(); // SAME existing artifact owns RX; never inside native timer.
   return {std::move(*selected), inputs};
}

std::uint32_t i32(const oc::invocation_result& result) {
   BOOST_REQUIRE(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_REQUIRE(!static_cast<bool>(result.host_error));
   BOOST_REQUIRE(result.value);
   return result.value->to_ui32();
}
} // namespace

// Explicit selected gate, not silently skipped fixture acceptance. The normal
// runtime-own suite has no external file prerequisite. Run this suite with the
// reviewed immutable directory; missing bytes are a harness FAILURE, not PASS.
BOOST_AUTO_TEST_SUITE(optimizing_executor_compiled, *boost::unit_test::disabled())

BOOST_AUTO_TEST_CASE(actual_recursive_canonical_depth_is_a_guest_trap_with_sufficient_local_stack) {
   const auto original = load_fixture("recursive");
   BOOST_REQUIRE_EQUAL(original.functions().size(), 1U);
   BOOST_REQUIRE_EQUAL(original.functions()[0].stack_bytes, 48U);
   BOOST_REQUIRE_EQUAL(original.image().configuration.execution.max_call_depth, 251U);
   const auto encoded = original.encode();
   const auto code = oc::executor::bind<compiled_registry>(original);
   constexpr auto physical_capacity = guest_reserve_bytes + 256 * 1024;
   {
      // Independently observe the existing owner's page-rounded usable bounds.
      // All 252 prologues, including the failing entry, fit after F/anchor/G.
      const auto stack = oc::native_stack{physical_capacity};
      const auto bounds = stack.usable();
      const auto usable = (bounds.high & ~std::uintptr_t{15}) - bounds.low;
      const auto frame = original.functions()[0].stack_bytes;
      BOOST_REQUIRE_LE(usable, physical_capacity);
      BOOST_REQUIRE_GT(usable, guest_reserve_bytes + oc::native_stack::anchor_bytes + frame);
      const auto allowance = usable - oc::native_stack::anchor_bytes - frame - guest_reserve_bytes;
      const auto required = (original.image().configuration.execution.max_call_depth + 1ULL) * frame;
      BOOST_REQUIRE_GE(allowance, required);
      BOOST_TEST_MESSAGE("recursive actual usable=" << usable << " allowance=" << allowance << " required=" << required
                                                    << " frame=" << frame << " canonical_depth=251");
   }
   auto state = compiled_fixture{physical_capacity};
   const auto allowed = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{std::uint32_t{250}}};
   const auto invoke_allowed = [&] {
      const auto result =
          state.executor.invoke(code, &state.host, 0, allowed, state.memory, std::chrono::steady_clock::now() + 1s);
      BOOST_REQUIRE(result.value);
      BOOST_REQUIRE(result.value->is_a<vm::i32_const_t>());
      BOOST_REQUIRE_EQUAL(i32(result), 42U);
   };
   invoke_allowed();
   const auto exceeded = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{std::uint32_t{251}}};
   const auto result =
       state.executor.invoke(code, &state.host, 0, exceeded, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_REQUIRE_EQUAL(static_cast<unsigned>(result.trap), static_cast<unsigned>(oc::trap_reason::stack_overflow));
   BOOST_REQUIRE_EQUAL(static_cast<unsigned>(result.trap), 8U);
   BOOST_REQUIRE(!static_cast<bool>(result.host_error));
   BOOST_REQUIRE(!result.value);
   invoke_allowed(); // Same prepared mapping succeeds again after canonical trap cleanup.
   BOOST_REQUIRE_EQUAL(state.host.effects, 0U);
   BOOST_REQUIRE_EQUAL(state.host.destroyed, 0U);
   BOOST_REQUIRE(original.is_prepared());
   BOOST_CHECK(original.encode() == encoded);
}

BOOST_AUTO_TEST_CASE(actual_finite_large_frames_use_failing_frame_room_inside_same_physical_cap) {
   if (fresh_process::mode() == nullptr) {
      const auto child = fresh_process::run(
          "optimizing_executor_compiled/actual_finite_large_frames_use_failing_frame_room_inside_same_physical_cap",
          "finite-stack");
      BOOST_REQUIRE_EQUAL(child.error, 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status));
      BOOST_REQUIRE_EQUAL(WEXITSTATUS(child.status), 0);
      return;
   }
   fresh_process::disable_core_dumps();
   const auto original = load_fixture("finite");
   BOOST_REQUIRE_EQUAL(original.functions().size(), 3U);
   BOOST_REQUIRE_EQUAL(original.image().configuration.execution.max_call_depth, 251U);
#if defined(__APPLE__) && defined(__aarch64__)
   const auto frames = std::array<std::uint64_t, 3>{14064, 14064, 8256};
#else
   const auto frames = std::array<std::uint64_t, 3>{16368, 16368, 8240};
#endif
   for (auto index = std::size_t{0}; index < frames.size(); ++index)
      BOOST_REQUIRE_EQUAL(original.functions()[index].stack_bytes, frames[index]);
   const auto code = oc::executor::bind<compiled_registry>(original);
   const auto arguments = std::vector<vm::operand_stack_elem>(1024, vm::i64_const_t{std::uint64_t{0}});
   // Same immutable WASM/artifact/identity and canonical depth. Only local
   // physical capacity differs; never rewrite frame metadata or ABI context.
   for (const auto payload : {std::uint64_t{48 * 1024}, std::uint64_t{64 * 1024}}) {
      auto state = compiled_fixture{guest_reserve_bytes + payload};
      if (payload == 48 * 1024) {
         BOOST_CHECK_THROW(state.executor.invoke(code, &state.host, 0, arguments, state.memory,
                                                 std::chrono::steady_clock::now() + 1s),
                           vm::exceptions::execution_unavailable);
      } else {
         const auto result = state.executor.invoke(code, &state.host, 0, arguments, state.memory,
                                                   std::chrono::steady_clock::now() + 1s);
         BOOST_REQUIRE_EQUAL(i32(result), 42U);
      }
      BOOST_REQUIRE_EQUAL(state.host.effects, 0U);
      BOOST_REQUIRE_EQUAL(state.host.destroyed, 0U);
   }
}

BOOST_AUTO_TEST_CASE(actual_first_frame_capacity_refusal_precedes_memory_or_host_effects) {
   const auto original = load_fixture("finite");
   const auto code = oc::executor::bind<compiled_registry>(original);
   const auto arguments = std::vector<vm::operand_stack_elem>(1024, vm::i64_const_t{std::uint64_t{0}});
   auto state = compiled_fixture{guest_reserve_bytes + 16 * 1024};
   BOOST_REQUIRE_EQUAL(state.memory.get_current_page(), 0U);
   BOOST_CHECK_THROW(
       state.executor.invoke(code, &state.host, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s),
       vm::exceptions::execution_unavailable);
   BOOST_REQUIRE_EQUAL(state.memory.get_current_page(), 0U);
   BOOST_REQUIRE_EQUAL(state.host.effects, 0U);
}

BOOST_AUTO_TEST_CASE(actual_compiler_integer_start_relay_and_wide_use_the_same_prepared_image) {
   auto state = compiled_fixture{};
   const auto original = load_fixture("add");
   auto code = oc::executor::bind<compiled_registry>(original);
   const auto arguments = std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{17}, vm::i32_const_t{25}};
   for (const auto index : {0U, 1U})
      BOOST_TEST(i32(state.executor.invoke(code, &state.host, index, arguments, state.memory,
                                           std::chrono::steady_clock::now() + 1s)) == 42U);
   const auto wide =
       std::array<vm::operand_stack_elem, 2>{vm::i64_const_t{UINT64_C(0x100000001)}, vm::i64_const_t{std::uint64_t{7}}};
   const auto result =
       state.executor.invoke(code, &state.host, 2, wide, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_REQUIRE(result.value);
   BOOST_TEST(result.value->to_ui64() == UINT64_C(0x100000008));
   const auto empty =
       state.executor.invoke(code, &state.host, 3, {}, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(empty.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_TEST(!empty.value);
   BOOST_TEST(state.host.effects == 0U);
   BOOST_TEST(original.is_prepared());
}

BOOST_AUTO_TEST_CASE(canonical_import_ordinals_direct_and_indirect_calls_reuse_registered_host_dispatch) {
   auto state = compiled_fixture{};
   auto code = oc::executor::bind<compiled_registry>(load_fixture("imports-indirect"));
   const auto arguments = std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{19}, vm::i32_const_t{5}};
   for (const auto index : {0U, 4U})
      BOOST_TEST(i32(state.executor.invoke(code, &state.host, index, arguments, state.memory,
                                           std::chrono::steady_clock::now() + 1s)) == 9U);
   for (const auto index : {1U, 5U})
      BOOST_TEST(i32(state.executor.invoke(code, &state.host, index, arguments, state.memory,
                                           std::chrono::steady_clock::now() + 1s)) == 34U);
   for (const auto selector : {0U, 1U, 2U}) {
      const auto indirect =
          std::array<vm::operand_stack_elem, 3>{arguments[0], arguments[1], vm::i32_const_t{selector}};
      BOOST_TEST(i32(state.executor.invoke(code, &state.host, 3, indirect, state.memory,
                                           std::chrono::steady_clock::now() + 1s)) == (selector == 0   ? 9U
                                                                                       : selector == 1 ? 34U
                                                                                                       : 14U));
   }
   for (const auto selector : {3U, 4U, 5U}) {
      const auto indirect =
          std::array<vm::operand_stack_elem, 3>{arguments[0], arguments[1], vm::i32_const_t{selector}};
      const auto result =
          state.executor.invoke(code, &state.host, 3, indirect, state.memory, std::chrono::steady_clock::now() + 1s);
      BOOST_TEST(static_cast<unsigned>(result.trap) ==
                 static_cast<unsigned>(selector == 4 ? oc::trap_reason::indirect_type : oc::trap_reason::table_bounds));
      BOOST_TEST(!result.value);
   }
   BOOST_TEST(state.host.effects == 6U);
   BOOST_TEST(state.host.destroyed == 6U);
}

BOOST_AUTO_TEST_CASE(real_growth_refreshes_memory_then_a_fresh_invocation_resets_it) {
   auto state = compiled_fixture{};
   auto code = oc::executor::bind<compiled_registry>(load_fixture("grow"));
   const auto delta = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{1}};
   BOOST_TEST(i32(state.executor.invoke(code, &state.host, 1, delta, state.memory,
                                        std::chrono::steady_clock::now() + 1s)) == 1U);
   BOOST_TEST(state.memory.get_current_page() == 2);
   BOOST_TEST(i32(state.executor.invoke(code, &state.host, 3, delta, state.memory,
                                        std::chrono::steady_clock::now() + 1s)) == 0x2345ff80U);
   BOOST_TEST(state.memory.get_current_page() == 2);
   const auto excessive = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{UINT32_MAX}};
   BOOST_TEST(i32(state.executor.invoke(code, &state.host, 1, excessive, state.memory,
                                        std::chrono::steady_clock::now() + 1s)) == UINT32_MAX);
   BOOST_TEST(state.memory.get_current_page() == 1);
}

BOOST_AUTO_TEST_CASE(real_software_memory_guard_rejects_oob_without_host_effects) {
   auto state = compiled_fixture{};
   auto code = oc::executor::bind<compiled_registry>(load_fixture("memory-load"));
   const auto valid = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{0}};
   BOOST_TEST(i32(state.executor.invoke(code, &state.host, 0, valid, state.memory,
                                        std::chrono::steady_clock::now() + 1s)) == 0x2345ff80U);
   const auto bad = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{UINT32_MAX}};
   const auto result =
       state.executor.invoke(code, &state.host, 0, bad, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::memory_bounds));
   BOOST_TEST(!result.value);
   BOOST_TEST(state.host.effects == 0U);
}

BOOST_AUTO_TEST_CASE(real_integer_divide_guards_are_typed_vm_traps) {
   auto state = compiled_fixture{};
   auto code = oc::executor::bind<compiled_registry>(load_fixture("integer-traps"));
   const auto zero = std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{1}, vm::i32_const_t{0}};
   const auto divide =
       state.executor.invoke(code, &state.host, 0, zero, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(divide.trap) == static_cast<unsigned>(oc::trap_reason::integer_divide_by_zero));
   const auto overflow =
       std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{0x80000000U}, vm::i32_const_t{UINT32_MAX}};
   const auto result =
       state.executor.invoke(code, &state.host, 0, overflow, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::integer_overflow));
   BOOST_TEST(state.host.effects == 0U);
}

BOOST_AUTO_TEST_CASE(post_import_stop_and_real_clock_prevent_further_guest_host_effects) {
   auto state = compiled_fixture{};
   auto code = oc::executor::bind<compiled_registry>(load_fixture("import-loop"));
   const auto arguments = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{5}};
   auto stop = std::stop_source{};
   state.host.stop = &stop;
   const auto stopped = state.executor.invoke(code, &state.host, 1, arguments, state.memory,
                                              std::chrono::steady_clock::now() + 1s, stop.get_token());
   BOOST_TEST(static_cast<unsigned>(stopped.trap) == static_cast<unsigned>(oc::trap_reason::cancelled));
   BOOST_TEST(state.host.effects == 1U);
   BOOST_TEST(state.host.destroyed == 1U);
   state.host.stop = nullptr;
   state.host.slow = true;
   const auto deadline =
       state.executor.invoke(code, &state.host, 1, arguments, state.memory, std::chrono::steady_clock::now() + 5ms);
   BOOST_TEST(static_cast<unsigned>(deadline.trap) == static_cast<unsigned>(oc::trap_reason::deadline));
   BOOST_TEST(state.host.effects == 2U);
   BOOST_TEST(state.host.destroyed == 2U);
}

BOOST_AUTO_TEST_CASE(actual_infinite_guest_compute_stop_exits_with_ticker_unable_to_run) {
   if (fresh_process::mode() == nullptr) {
      const auto child = fresh_process::run(
          "optimizing_executor_compiled/actual_infinite_guest_compute_stop_exits_with_ticker_unable_to_run",
          "infinite");
      BOOST_REQUIRE(child.error == 0);
      BOOST_TEST(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status));
      BOOST_TEST(WEXITSTATUS(child.status) == 0);
      return;
   }
   fresh_process::disable_core_dumps();
   auto state = compiled_fixture{};
   const auto artifact = load_fixture("infinite");
   const auto native = artifact.native_code();
   auto code = oc::executor::bind<compiled_registry>(artifact);
   auto entered = std::atomic<bool>{false};
   auto exited = std::atomic<bool>{false};
   auto reason = std::atomic<unsigned>{0};
#if defined(__APPLE__)
   auto thread = std::atomic<mach_port_t>{MACH_PORT_NULL};
#elif defined(__linux__) && defined(__x86_64__)
   struct sigaction previous_signal{};
   struct sigaction sample_signal{};
   sample_signal.sa_sigaction = sample_native_pc;
   sample_signal.sa_flags = SA_SIGINFO;
   if (::sigemptyset(&sample_signal.sa_mask) != 0 || ::sigaction(SIGUSR2, &sample_signal, &previous_signal) != 0)
      throw std::runtime_error{"native PC observation setup failed"};
   const auto restore_signal = vm::scope_guard{[&] {
      if (::sigaction(SIGUSR2, &previous_signal, nullptr) != 0)
         std::_Exit(93);
   }};
   auto thread = pthread_t{};
   auto previous_mask = sigset_t{};
   auto thread_known = std::atomic<bool>{false};
   auto mask_restored = std::atomic<bool>{false};
#endif
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .on_worker_start =
           [&](std::size_t) {
              state.executor.prepare_thread();
#if defined(__APPLE__)
              thread.store(mach_thread_self(), std::memory_order_release);
#elif defined(__linux__) && defined(__x86_64__)
              auto sample_mask = sigset_t{};
              if (::sigemptyset(&sample_mask) != 0 || ::sigaddset(&sample_mask, SIGUSR2) != 0 ||
                  ::pthread_sigmask(SIG_SETMASK, nullptr, &previous_mask) != 0 ||
                  ::pthread_sigmask(SIG_UNBLOCK, &sample_mask, nullptr) != 0)
                 throw std::runtime_error{"native PC worker observation mask setup failed"};
              thread = ::pthread_self();
              thread_known.store(true, std::memory_order_release);
#endif
           },
       .on_worker_stop =
           [&](std::size_t) {
#if defined(__linux__) && defined(__x86_64__)
              const auto restore_mask = vm::scope_guard{[&] {
                 mask_restored.store(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr) == 0,
                                     std::memory_order_release);
              }};
#endif
              state.executor.release_thread();
           },
   }};
   auto operation = forge::asio::blocking::run(
       state.runtime,
       pool.get_executor().submit({.name = "compiled-infinite-owner"}, [&](forge::asio::compute::context& compute) {
          auto memory = vm::wasm_allocator{};
          auto host = compiled_host{};
          entered.store(true, std::memory_order_release);
          const auto result = state.executor.invoke(code, &host, 0, {}, memory, std::chrono::steady_clock::now() + 2s,
                                                    {}, compute.stop_token());
          reason.store(static_cast<unsigned>(result.trap), std::memory_order_release);
          exited.store(true, std::memory_order_release);
       }));
   const auto started_by = std::chrono::steady_clock::now() + 1s;
   while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < started_by)
      std::this_thread::sleep_for(1ms);
   auto observed_guest = false;
#if defined(__APPLE__)
   // Actual PC in compiler-owned RX code, not merely a marker before invoke.
   const auto sampled_by = std::chrono::steady_clock::now() + 500ms;
   while (!observed_guest && std::chrono::steady_clock::now() < sampled_by) {
      auto registers = arm_thread_state64_t{};
      auto count = static_cast<mach_msg_type_number_t>(ARM_THREAD_STATE64_COUNT);
      if (thread_get_state(thread.load(std::memory_order_acquire), ARM_THREAD_STATE64,
                           reinterpret_cast<thread_state_t>(&registers), &count) == KERN_SUCCESS) {
         const auto pc = static_cast<std::uintptr_t>(arm_thread_state64_get_pc(registers));
         const auto low = reinterpret_cast<std::uintptr_t>(native.data());
         observed_guest = pc >= low && pc - low < native.size();
      }
      if (!observed_guest)
         std::this_thread::sleep_for(1ms);
   }
#elif defined(__linux__) && defined(__x86_64__)
   // The pool still owns this known worker until shutdown/join below. Never
   // signal a recycled pthread after join. Capture must land in THIS immutable
   // artifact RX, not in pre-entry C++ or a host bridge.
   const auto sampled_by = std::chrono::steady_clock::now() + 500ms;
   while (!observed_guest && thread_known.load(std::memory_order_acquire) &&
          std::chrono::steady_clock::now() < sampled_by) {
      sampled_native_pc.store(0, std::memory_order_release);
      if (::pthread_kill(thread, SIGUSR2) != 0)
         break;
      std::this_thread::sleep_for(1ms);
      const auto pc = sampled_native_pc.load(std::memory_order_acquire);
      const auto low = reinterpret_cast<std::uintptr_t>(native.data());
      observed_guest = pc >= low && pc - low < native.size();
   }
#endif
   auto mutex = std::mutex{};
   auto changed = std::condition_variable{};
   auto release = false;
   auto ticker_blocked = std::atomic<bool>{false};
   auto guest_exited_while_ticker_held = std::atomic<bool>{false};
   boost::asio::post(state.runtime.context(), [&] {
      auto lock = std::unique_lock{mutex};
      ticker_blocked.store(true, std::memory_order_release);
      changed.wait_for(lock, 1s, [&] { return release; });
      // A stale entry marker is not proof: if the finite wait expired before
      // guest exit, the ticker could resume and the no-tick oracle must fail.
      guest_exited_while_ticker_held.store(exited.load(std::memory_order_acquire), std::memory_order_release);
   });
   const auto blocked_by = std::chrono::steady_clock::now() + 1s;
   while (!ticker_blocked.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < blocked_by)
      std::this_thread::sleep_for(1ms);
   pool.request_stop(); // Sync existing stop chain, independent of the IO/ticker.
   const auto stopped_by = std::chrono::steady_clock::now() + 100ms;
   while (!exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < stopped_by)
      std::this_thread::sleep_for(1ms);
   const auto stopped_without_tick = exited.load(std::memory_order_acquire);
   {
      const auto lock = std::lock_guard{mutex};
      release = true;
   }
   changed.notify_all();
   auto terminal = std::exception_ptr{};
   try {
      forge::asio::blocking::run(state.runtime, std::move(operation).wait());
   } catch (...) {
      terminal = std::current_exception();
   }
   forge::asio::blocking::run(state.runtime, pool.shutdown());
   BOOST_TEST(observed_guest);
#if defined(__APPLE__)
   BOOST_REQUIRE(mach_port_deallocate(mach_task_self(), thread.load()) == KERN_SUCCESS);
#elif defined(__linux__) && defined(__x86_64__)
   BOOST_TEST(mask_restored.load(std::memory_order_acquire));
#endif
   BOOST_TEST(ticker_blocked.load(std::memory_order_acquire));
   BOOST_TEST(guest_exited_while_ticker_held.load(std::memory_order_acquire));
   BOOST_TEST(stopped_without_tick);
   BOOST_TEST(reason.load(std::memory_order_acquire) == static_cast<unsigned>(oc::trap_reason::cancelled));
   BOOST_REQUIRE(static_cast<bool>(terminal));
   BOOST_CHECK_THROW(std::rethrow_exception(terminal), forge::asio::exceptions::canceled);
}

BOOST_AUTO_TEST_SUITE_END()

// Independent opt-in suite: selecting the historical genuine executor suite
// must not silently add benchmarks or acquire a different fixture prerequisite.
BOOST_AUTO_TEST_SUITE(optimizing_benchmark_executor, *boost::unit_test::disabled())

BOOST_AUTO_TEST_CASE(native_first_and_128_warm_calls_require_the_same_checksum) {
   for (const auto name : {"benchmark-integer", "benchmark-memory"}) {
      const auto fixture = load_benchmark_fixture(name);
      const auto code = oc::executor::bind<compiled_registry>(fixture.code);
      const auto arguments = std::array<vm::operand_stack_elem, 3>{
          vm::i64_const_t{fixture.inputs[0]}, vm::i64_const_t{fixture.inputs[1]}, vm::i64_const_t{fixture.inputs[2]}};
      auto elapsed = std::array<std::int64_t, 129>{};
      {
         auto state = compiled_fixture{}; // startup/thread lease outside native timing
         for (std::size_t iteration = 0; iteration < elapsed.size(); ++iteration) {
            const auto effects_before = state.host.effects;
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            const auto start = std::chrono::steady_clock::now();
            const auto result = state.executor.invoke(code, &state.host, 1, arguments, state.memory, deadline);
            const auto finish = std::chrono::steady_clock::now();
            elapsed[iteration] = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
            // ALL checking/output is outside the measured call. Each invocation
            // must execute the one real host effect, not reuse a previous checksum.
            BOOST_REQUIRE(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::none));
            BOOST_REQUIRE(!static_cast<bool>(result.host_error));
            BOOST_REQUIRE(!result.value.has_value());
            BOOST_REQUIRE(state.host.effects == effects_before + 1U);
            BOOST_REQUIRE(state.host.checksum == fixture.inputs[3]);
            BOOST_REQUIRE(elapsed[iteration] >= 0);
         }
      } // SAME executor/scheduler/thread owners drained before publishing samples
      auto total = std::int64_t{};
      const auto identity = fixture.code.identity().str();
      const auto wasm = fixture.code.image().wasm.str();
      const auto& target = fixture.code.image().configuration.target;
      std::printf("oc_input engine=forge workload=%s receiver=%" PRIu64 " code=%" PRIu64 " action=%" PRIu64
                  " expected_checksum=%" PRIu64
                  " identity=%s wasm=%s native_bytes=%zu llvm=%s codegen=%u host_abi=%u\n",
                  name, fixture.inputs[0], fixture.inputs[1], fixture.inputs[2], fixture.inputs[3], identity.c_str(),
                  wasm.c_str(), fixture.code.native_code().size(), target.llvm_version.c_str(), target.codegen_version,
                  target.host_abi_version);
      for (std::size_t iteration = 0; iteration < elapsed.size(); ++iteration) {
         if (iteration != 0)
            total += elapsed[iteration];
         std::printf("oc_metric engine=forge boundary=vm_invoke stage=%s workload=%s iteration=%zu "
                     "elapsed_ns=%" PRId64 " checksum=%" PRIu64 " identity=%s wasm=%s speed_verdict=NOT_EVALUATED\n",
                     iteration == 0 ? "cold_first" : "warm_repeat", name, iteration, elapsed[iteration],
                     fixture.inputs[3], identity.c_str(), wasm.c_str());
      }
      std::printf("oc_metric engine=forge boundary=vm_invoke stage=warm_aggregate workload=%s "
                  "iterations=128 elapsed_ns=%" PRId64 " checksum=%" PRIu64 " speed_verdict=NOT_EVALUATED\n",
                  name, total, fixture.inputs[3]);
   }
}

BOOST_AUTO_TEST_SUITE_END()
