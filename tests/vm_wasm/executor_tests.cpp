module;

#include "fresh_process.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.exceptions;

#include "details/native_stack.hxx"

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;
constexpr std::uint64_t guest_reserve_bytes = 4 * 1024 * 1024;

// Runtime-own normal ABI fixture, NOT LLVM/compiler acceptance. It calls the
// actual context callback and generated-style terminal branch, without process
// addresses/relocations or a second lowering path. Compiler->executor fixtures
// will separately consume immutable output from the existing compiler owner.
extern "C" char forge_runtime_host_fixture_end[];
#if defined(__APPLE__) && defined(__aarch64__)
extern "C" [[gnu::naked, gnu::noinline]] void forge_runtime_host_fixture(oc::invocation_abi*, const vm::expr_value*,
                                                                         vm::expr_value*) noexcept {
   asm volatile("stp x29, x30, [sp, #-32]!\n"
                "mov x29, sp\n"
                "str x0, [sp, #16]\n"
                "ldr x8, [x0, #136]\n"
                "ldr x8, [x8]\n"
                "mov x3, x2\n"
                "mov x2, x1\n"
                "mov w1, wzr\n"
                "blr x8\n"
                "cbz w0, 1f\n"
                "mov w1, w0\n"
                "ldr x0, [sp, #16]\n"
                "ldr x8, [x0, #136]\n"
                "ldr x8, [x8, #32]\n"
                "blr x8\n"
                "1: ldp x29, x30, [sp], #32\n"
                "ret\n"
                ".globl _forge_runtime_host_fixture_end\n"
                "_forge_runtime_host_fixture_end:\n");
}
#elif defined(__linux__) && defined(__x86_64__)
extern "C" [[gnu::naked, gnu::noinline]] void forge_runtime_host_fixture(oc::invocation_abi*, const vm::expr_value*,
                                                                         vm::expr_value*) noexcept {
   asm volatile("pushq %rbp\n"
                "movq %rsp, %rbp\n"
                "pushq %rbx\n"
                "subq $8, %rsp\n"
                "movq %rdi, %rbx\n"
                "movq %rdx, %rcx\n"
                "movq %rsi, %rdx\n"
                "xorl %esi, %esi\n"
                "movq 136(%rdi), %rax\n"
                "callq *(%rax)\n"
                "testl %eax, %eax\n"
                "jz 1f\n"
                "movl %eax, %esi\n"
                "movq %rbx, %rdi\n"
                "movq 136(%rbx), %rax\n"
                "callq *32(%rax)\n"
                "1: addq $8, %rsp\n"
                "popq %rbx\n"
                "leave\n"
                "retq\n"
                ".globl forge_runtime_host_fixture_end\n"
                "forge_runtime_host_fixture_end:\n");
}
#endif
static_assert(offsetof(oc::invocation_abi, callbacks) == 136);
static_assert(offsetof(oc::invocation_callbacks, host_call) == 0 && offsetof(oc::invocation_callbacks, trap) == 32);

// Runtime-own terminal/guard classification control, NOT compiler frame/depth
// lowering. UINT32_MAX accesses the owned guard from owned RX instructions;
// other arguments exercise the existing raw trap callback with the given reason.
extern "C" char forge_runtime_stack_fixture_end[];
#if defined(__APPLE__) && defined(__aarch64__)
extern "C" [[gnu::naked, gnu::noinline]] void forge_runtime_stack_fixture(oc::invocation_abi*, const vm::expr_value*,
                                                                          vm::expr_value*) noexcept {
   asm volatile("ldr w1, [x1]\n"
                "cmn w1, #1\n"
                "b.eq 1f\n"
                "ldr x8, [x0, #136]\n"
                "ldr x8, [x8, #32]\n"
                "br x8\n"
                "1: ldr x9, [x0, #112]\n"
                "ldr x8, [x9]\n"
                "ret\n"
                ".globl _forge_runtime_stack_fixture_end\n"
                "_forge_runtime_stack_fixture_end:\n");
}
#elif defined(__linux__) && defined(__x86_64__)
extern "C" [[gnu::naked, gnu::noinline]] void forge_runtime_stack_fixture(oc::invocation_abi*, const vm::expr_value*,
                                                                          vm::expr_value*) noexcept {
   asm volatile("movl (%rsi), %esi\n"
                "cmpl $-1, %esi\n"
                "je 1f\n"
                "movq 136(%rdi), %rax\n"
                "jmpq *32(%rax)\n"
                "1: movq 112(%rdi), %rax\n"
                "movq (%rax), %rax\n"
                "retq\n"
                ".globl forge_runtime_stack_fixture_end\n"
                "forge_runtime_stack_fixture_end:\n");
}
#endif
static_assert(offsetof(oc::invocation_abi, stack_guard_low) == 112);

struct host_failure {
   const void* identity;
};
struct host {
   unsigned effects = 0;
   unsigned destroyed = 0;
   std::uintptr_t pointer = 0;
   std::stop_source* stop = nullptr;
   bool fail = false;
   bool allocation_failure = false;
   std::function<void()> nested;
   std::uint32_t lhs_bits = 0;
   std::uint64_t rhs_bits = 0;

   std::uint32_t add(std::uint32_t lhs, std::uint32_t rhs) {
      const auto cleanup = vm::scope_guard{[&] { ++destroyed; }};
      pointer = oc::native_stack::current_pointer();
      ++effects;
      if (nested)
         nested();
      if (stop)
         stop->request_stop();
      if (fail)
         throw host_failure{this};
      if (allocation_failure)
         throw std::bad_alloc{};
      return lhs + rhs;
   }
   std::uint32_t range(vm::span<const char> bytes) {
      ++effects;
      return static_cast<std::uint32_t>(bytes.size());
   }
   std::uint64_t float_bits(float lhs, double rhs) {
      ++effects;
      lhs_bits = std::bit_cast<std::uint32_t>(lhs);
      rhs_bits = std::bit_cast<std::uint64_t>(rhs);
      return rhs_bits ^ lhs_bits;
   }
};
using registered = vm::registered_host_functions<host>;

oc::artifact prepared(const char* name = "add") {
   static const auto registration = [] {
      registered::add<&host::add>("runtime_test", "add");
      registered::add<&host::range>("runtime_test", "range");
      registered::add<&host::float_bits>("runtime_test", "float_bits");
      return true;
   }();
   static_cast<void>(registration);
   auto image = oc::artifact_payload{};
   image.configuration.limits = {1024 * 1024, 16384};
#if defined(__APPLE__) && defined(__aarch64__)
   image.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
   image.configuration.target.cpu = "generic";
   image.configuration.target.features = "+fp-armv8,+neon";
#endif
   const auto signature = std::strcmp(name, "float_bits") == 0 ? vm::host_function{{vm::f32, vm::f64}, {vm::i64}}
                                                               : vm::host_function{{vm::i32, vm::i32}, {vm::i32}};
   image.configuration.imports.push_back({"runtime_test", name, signature});
   image.types.push_back(signature);
   auto* begin = reinterpret_cast<const std::uint8_t*>(&forge_runtime_host_fixture);
   auto* end = reinterpret_cast<const std::uint8_t*>(forge_runtime_host_fixture_end);
   if (end <= begin || end - begin > 256)
      throw std::runtime_error{"runtime-own fixture extent invalid"};
   image.sections.push_back({oc::section_kind::code, 16, {begin, end}});
   image.functions.push_back({{0, 0}, 0, 0, 32});
   image.exports.push_back({"invoke", 0});
   image.memory = {true, 1, 2};
   auto code = oc::artifact::from_payload(std::move(image));
   code.prepare();
   return code;
}

oc::artifact prepared_stack_control() {
   auto image = oc::artifact_payload{};
   image.configuration.limits = {1024 * 1024, 16384};
#if defined(__APPLE__) && defined(__aarch64__)
   image.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
   image.configuration.target.cpu = "generic";
   image.configuration.target.features = "+fp-armv8,+neon";
#endif
   image.types.push_back({{vm::i32}, {}});
   auto* begin = reinterpret_cast<const std::uint8_t*>(&forge_runtime_stack_fixture);
   auto* end = reinterpret_cast<const std::uint8_t*>(forge_runtime_stack_fixture_end);
   if (end <= begin || end - begin > 128)
      throw std::runtime_error{"runtime-own stack fixture extent invalid"};
   image.sections.push_back({oc::section_kind::code, 16, {begin, end}});
   image.functions.push_back({{0, 0}, 0, UINT32_MAX, 32});
   auto code = oc::artifact::from_payload(std::move(image));
   code.prepare();
   return code;
}

struct fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime};
   oc::executor executor{scheduler, {guest_reserve_bytes + 256 * 1024, 64 * 1024}};
   vm::wasm_allocator memory;

   fixture() {
      forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 1s));
      executor.prepare_thread();
   }
   ~fixture() {
      executor.release_thread();
      forge::asio::blocking::run(runtime, executor.shutdown());
      forge::asio::blocking::run(runtime, scheduler.shutdown());
   }
};

const auto arguments = std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{17}, vm::i32_const_t{25}};
} // namespace

BOOST_AUTO_TEST_SUITE(optimizing_executor)

BOOST_AUTO_TEST_CASE(prepared_artifact_runs_through_typed_registry_on_actual_ordinary_host_stack) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   auto active = host{};
   const auto result =
       state.executor.invoke(code, &active, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_TEST(!static_cast<bool>(result.host_error));
   BOOST_REQUIRE(result.value);
   BOOST_TEST(result.value->to_i32() == 42);
   BOOST_TEST(active.effects == 1U);
   BOOST_TEST(active.destroyed == 1U);
   const auto bounds = oc::native_stack::current_thread_bounds();
   BOOST_TEST(active.pointer >= bounds.low);
   BOOST_TEST(active.pointer < bounds.high);
}

BOOST_AUTO_TEST_CASE(host_exception_unwinds_destructor_before_raw_terminal_branch_and_keeps_identity) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   auto active = host{.fail = true};
   const auto result =
       state.executor.invoke(code, &active, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::host_exception));
   BOOST_TEST(!result.value);
   BOOST_TEST(active.destroyed == 1U);
   BOOST_REQUIRE(result.host_error != nullptr);
   try {
      std::rethrow_exception(result.host_error);
      BOOST_FAIL("expected exact host failure");
   } catch (const host_failure& error) {
      BOOST_TEST(error.identity == &active);
   }
}

BOOST_AUTO_TEST_CASE(software_bad_range_traps_before_typed_host_body) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared("range"));
   auto active = host{};
   const auto bad = std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{UINT32_MAX}, vm::i32_const_t{16}};
   const auto result =
       state.executor.invoke(code, &active, 0, bad, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::memory_bounds));
   BOOST_TEST(!static_cast<bool>(result.host_error));
   BOOST_TEST(active.effects == 0U);
}

BOOST_AUTO_TEST_CASE(post_host_stop_drops_value_and_returns_after_complete_cleanup) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   auto stop = std::stop_source{};
   auto active = host{.stop = &stop};
   const auto result = state.executor.invoke(code, &active, 0, arguments, state.memory,
                                             std::chrono::steady_clock::now() + 1s, stop.get_token());
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::cancelled));
   BOOST_TEST(!result.value);
   BOOST_TEST(!static_cast<bool>(result.host_error));
   BOOST_TEST(active.effects == 1U);
   BOOST_TEST(active.destroyed == 1U);
}

BOOST_AUTO_TEST_CASE(nested_context_uses_distinct_memory_and_preserves_local_typed_catch) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   auto nested_memory = vm::wasm_allocator{};
   auto nested = host{.fail = true};
   auto caught = false;
   auto active = host{};
   active.nested = [&] {
      const auto result =
          state.executor.invoke(code, &nested, 0, arguments, nested_memory, std::chrono::steady_clock::now() + 1s);
      try {
         std::rethrow_exception(result.host_error);
      } catch (const host_failure& error) {
         caught = error.identity == &nested;
      }
      BOOST_CHECK_THROW(
          state.executor.invoke(code, &nested, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s),
          vm::exceptions::execution_unavailable);
   };
   const auto result =
       state.executor.invoke(code, &active, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(caught);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_TEST(active.destroyed == 1U);
   BOOST_TEST(nested.destroyed == 1U);
   BOOST_REQUIRE(result.value);
   BOOST_TEST(result.value->to_i32() == 42);
}

BOOST_AUTO_TEST_CASE(cold_registry_resolver_reuses_type_and_missing_mapping_checks) {
   const auto code = prepared();
   const auto expected = registered::resolve_import("runtime_test", "add", code.image().types.front());
   BOOST_TEST(expected ==
              registered::resolve_import("runtime_test", "add", vm::host_function{{vm::i32, vm::i32}, {vm::i32}}));
   BOOST_CHECK_THROW(registered::resolve_import("runtime_test", "absent", code.image().types.front()),
                     vm::exceptions::link);
   BOOST_CHECK_THROW(registered::resolve_import("runtime_test", "add", vm::host_function{{vm::i64}, {vm::i32}}),
                     vm::exceptions::link);
}

BOOST_AUTO_TEST_CASE(stateful_null_host_is_rejected_before_effects) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   BOOST_CHECK_THROW(state.executor.invoke(code, static_cast<host*>(nullptr), 0, arguments, state.memory,
                                           std::chrono::steady_clock::now() + 1s),
                     vm::exceptions::interpreter);
}

BOOST_AUTO_TEST_CASE(wrong_operand_alternative_is_parameter_error_before_host_effects) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   auto active = host{};
   const auto wrong = std::array<vm::operand_stack_elem, 2>{vm::i64_const_t{std::uint64_t{17}}, vm::i32_const_t{25}};
   BOOST_CHECK_THROW(
       state.executor.invoke(code, &active, 0, wrong, state.memory, std::chrono::steady_clock::now() + 1s),
       vm::exceptions::interpreter);
   BOOST_TEST(active.effects == 0U);
}

BOOST_AUTO_TEST_CASE(host_allocation_exception_is_not_reclassified_as_cold_setup_failure) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared());
   auto active = host{.allocation_failure = true};
   const auto result =
       state.executor.invoke(code, &active, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::host_exception));
   BOOST_TEST(active.effects == 1U);
   BOOST_TEST(active.destroyed == 1U);
   BOOST_TEST(!result.value);
   BOOST_REQUIRE(result.host_error != nullptr);
   BOOST_CHECK_THROW(std::rethrow_exception(result.host_error), std::bad_alloc);
}

BOOST_AUTO_TEST_CASE(float_argument_bits_including_nan_payload_are_preserved_by_existing_slot_transport) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared("float_bits"));
   auto active = host{};
   constexpr auto lhs = std::uint32_t{0x7fc12345};
   constexpr auto rhs = std::uint64_t{0x7ff800001234abcd};
   const auto raw = std::array<vm::operand_stack_elem, 2>{vm::f32_const_t{std::bit_cast<float>(lhs)},
                                                          vm::f64_const_t{std::bit_cast<double>(rhs)}};
   const auto result =
       state.executor.invoke(code, &active, 0, raw, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_TEST(active.effects == 1U);
   BOOST_TEST(active.lhs_bits == lhs);
   BOOST_TEST(active.rhs_bits == rhs);
   BOOST_REQUIRE(result.value);
   BOOST_TEST(result.value->to_ui64() == (rhs ^ lhs));
}

BOOST_AUTO_TEST_CASE(cold_guest_mapping_failure_is_operational_and_cannot_admit_callback) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   // Above the canonical userspace virtual-address extent on BOTH supported
   // targets. mmap must fail before touching pages, not a disk/heap stress test.
   auto executor = oc::executor{scheduler, {UINT64_MAX / 2, 64 * 1024}};
   forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 1s));
   BOOST_CHECK_THROW(executor.prepare_thread(), vm::exceptions::execution_unavailable);
   auto memory = vm::wasm_allocator{};
   auto code = oc::executor::bind<registered>(prepared());
   auto active = host{};
   BOOST_CHECK_THROW(executor.invoke(code, &active, 0, arguments, memory, std::chrono::steady_clock::now() + 1s),
                     vm::exceptions::execution_unavailable);
   BOOST_TEST(active.effects == 0U);
   BOOST_CHECK_NO_THROW(executor.release_thread());
   forge::asio::blocking::run(runtime, executor.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
}

BOOST_AUTO_TEST_CASE(oversized_local_host_headroom_is_rejected_without_wrapping_admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   BOOST_CHECK_THROW((oc::executor{scheduler, {guest_reserve_bytes + 256 * 1024, UINT64_MAX}}),
                     vm::exceptions::execution_unavailable);
   forge::asio::blocking::run(runtime, scheduler.shutdown());
}

BOOST_AUTO_TEST_CASE(guest_physical_cap_includes_donor_reserve_without_silent_mapping_growth) {
   const auto configured = guest_reserve_bytes + 64 * 1024 + 7;
   const auto stack = oc::native_stack{configured};
   const auto page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
   BOOST_REQUIRE_GT(page, 0U);
   const auto bounds = stack.usable();
   BOOST_TEST(bounds.high - bounds.low == configured - configured % page);
   BOOST_REQUIRE_LE(bounds.high - bounds.low, configured);
   BOOST_REQUIRE_GE(stack.guard().size(), 16 * 1024U);
   BOOST_TEST(reinterpret_cast<std::uintptr_t>(stack.guard().data()) + stack.guard().size() == bounds.low);
}

BOOST_AUTO_TEST_CASE(insufficient_physical_guest_reserve_is_operational_before_worker_entry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   BOOST_CHECK_THROW((oc::executor{scheduler, {guest_reserve_bytes, 64 * 1024}}),
                     vm::exceptions::execution_unavailable);
   forge::asio::blocking::run(runtime, scheduler.shutdown());
}

BOOST_AUTO_TEST_CASE(local_raw_stack_reason_is_operational_but_canonical_depth_reason_remains_guest_trap) {
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared_stack_control());
   auto active = host{};
   const auto local = std::array<vm::operand_stack_elem, 1>{
       vm::i32_const_t{static_cast<std::uint32_t>(oc::trap_reason::native_stack_exhausted)}};
   BOOST_CHECK_THROW(
       state.executor.invoke(code, &active, 0, local, state.memory, std::chrono::steady_clock::now() + 1s),
       vm::exceptions::execution_unavailable);
   const auto depth = std::array<vm::operand_stack_elem, 1>{
       vm::i32_const_t{static_cast<std::uint32_t>(oc::trap_reason::stack_overflow)}};
   const auto result =
       state.executor.invoke(code, &active, 0, depth, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::stack_overflow));
   BOOST_TEST(!result.value);
   BOOST_TEST(!static_cast<bool>(result.host_error));
   BOOST_TEST(active.effects == 0U);
   auto happy = oc::executor::bind<registered>(prepared());
   const auto next =
       state.executor.invoke(happy, &active, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(next.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_REQUIRE(next.value);
   BOOST_TEST(next.value->to_i32() == 42U);
   BOOST_TEST(active.destroyed == 1U);
}

BOOST_AUTO_TEST_CASE(owned_physical_stack_guard_is_operational_and_next_invocation_reuses_clean_owner) {
   if (fresh_process::mode() == nullptr) {
      const auto child = fresh_process::run(
          "optimizing_executor/owned_physical_stack_guard_is_operational_and_next_invocation_reuses_clean_owner",
          "owned-stack-guard");
      BOOST_REQUIRE(child.error == 0);
      BOOST_TEST(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status));
      BOOST_TEST(WEXITSTATUS(child.status) == 0);
      return;
   }
   fresh_process::disable_core_dumps();
   auto state = fixture{};
   auto code = oc::executor::bind<registered>(prepared_stack_control());
   auto active = host{};
   const auto guard = std::array<vm::operand_stack_elem, 1>{vm::i32_const_t{UINT32_MAX}};
   BOOST_CHECK_THROW(
       state.executor.invoke(code, &active, 0, guard, state.memory, std::chrono::steady_clock::now() + 1s),
       vm::exceptions::execution_unavailable);
   BOOST_TEST(active.effects == 0U);
   auto happy = oc::executor::bind<registered>(prepared());
   const auto result =
       state.executor.invoke(happy, &active, 0, arguments, state.memory, std::chrono::steady_clock::now() + 1s);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::none));
   BOOST_REQUIRE(result.value);
   BOOST_TEST(result.value->to_i32() == 42U);
   BOOST_TEST(active.effects == 1U);
   BOOST_TEST(active.destroyed == 1U);
}

BOOST_AUTO_TEST_CASE(shutdown_waits_for_actual_host_cleanup_and_worker_lease_pins_owner_after_public_release) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto executor =
       std::make_unique<oc::executor>(scheduler, oc::executor::options{guest_reserve_bytes + 256 * 1024, 64 * 1024});
   forge::asio::blocking::run(runtime, executor->start(std::chrono::steady_clock::now() + 1s));
   auto code = oc::executor::bind<registered>(prepared());
   auto mutex = std::mutex{};
   auto changed = std::condition_variable{};
   auto entered = false;
   auto release_host = false;
   auto release_worker = false;
   auto result = oc::invocation_result{};
   auto error = std::exception_ptr{};
   auto destroyed = unsigned{0};
   auto* owner = executor.get();
   auto worker = std::thread{[&] {
      try {
         owner->prepare_thread();
         auto memory = vm::wasm_allocator{};
         auto active = host{};
         active.nested = [&] {
            auto lock = std::unique_lock{mutex};
            entered = true;
            changed.notify_all();
            changed.wait_for(lock, 1s, [&] { return release_host; });
         };
         result = owner->invoke(code, &active, 0, arguments, memory, std::chrono::steady_clock::now() + 2s);
         destroyed = active.destroyed;
      } catch (...) {
         error = std::current_exception();
      }
      auto lock = std::unique_lock{mutex};
      changed.wait_for(lock, 2s, [&] { return release_worker; });
      // No call through the released public executor. SAME thread-local lease
      // owns implementation/reserve through its existing thread-exit cleanup.
   }};
   auto host_entered = false;
   {
      auto lock = std::unique_lock{mutex};
      host_entered = changed.wait_for(lock, 1s, [&] { return entered; });
   }
   auto drain = boost::asio::co_spawn(runtime.context(), executor->shutdown(), boost::asio::use_future);
   const auto held = drain.wait_for(10ms) == std::future_status::timeout;
   {
      const auto lock = std::lock_guard{mutex};
      release_host = true;
   }
   changed.notify_all();
   const auto completed = drain.wait_for(1s) == std::future_status::ready;
   auto drain_error = std::exception_ptr{};
   if (completed) {
      try {
         drain.get();
      } catch (...) {
         drain_error = std::current_exception();
      }
      // This is safe only with the complete retirement gate and SAME impl pin;
      // the supplied runtime still lives until the worker's lease has retired.
      if (!drain_error)
         executor.reset();
   }
   {
      const auto lock = std::lock_guard{mutex};
      release_worker = true;
   }
   changed.notify_all();
   worker.join();
   if (executor)
      forge::asio::blocking::run(runtime, executor->shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   BOOST_TEST(host_entered);
   BOOST_TEST(held);
   BOOST_TEST(completed);
   BOOST_TEST(!static_cast<bool>(drain_error));
   BOOST_TEST(!static_cast<bool>(error));
   BOOST_TEST(destroyed == 1U);
   BOOST_TEST(static_cast<unsigned>(result.trap) == static_cast<unsigned>(oc::trap_reason::cancelled));
}

BOOST_AUTO_TEST_SUITE_END()
