module;

#include "test_prelude.hpp"
#include "optimizing_fixture_export.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if (defined(__linux__) && defined(__x86_64__)) || (defined(__APPLE__) && defined(__aarch64__))
#include "fresh_process.hpp"
#endif

// Owning implementation-unit test: private lowering oracle, no exported
// compiler test hook and no alternative parser/metadata production pipeline.
module forge.tooling.wasm_compiler.compiler;

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.exceptions;

#include "../../libraries/tooling/wasm_compiler/details/lowering.hxx"

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;

oc::compile_request epoch_loop_request(bool darwin) {
   auto request = oc::compile_request{};
   // One canonical void function, one type, one infinite loop/backedge.
   request.wasm = {0, 97, 115, 109, 1, 0, 0, 0,
                   1, 4, 1, 0x60, 0, 0,
                   3, 2, 1, 0,
                   10, 9, 1, 7, 0, 3, 0x40, 0x0c, 0, 0x0b, 0x0b};
   request.configuration.limits = {1024U * 1024U, 16384};
   if (darwin) {
      request.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
      request.configuration.target.cpu = "generic";
      request.configuration.target.features = "+fp-armv8,+neon";
   }
   return request;
}

std::size_t occurrences(std::string_view text, std::string_view needle) {
   auto count = std::size_t{};
   for (auto offset = text.find(needle); offset != text.npos; offset = text.find(needle, offset + needle.size()))
      ++count;
   return count;
}
} // namespace

BOOST_AUTO_TEST_SUITE(optimizing_epoch_ir)

BOOST_AUTO_TEST_CASE(export_checked_infinite_runtime_fixture, *boost::unit_test::disabled()) {
   for (const auto darwin : {false, true}) {
      const auto request = epoch_loop_request(darwin);
      const auto code = forge::tooling::wasm_compiler::compiler{}.compile(request);
      const auto encoded_request = request.encode();
      const auto encoded_artifact = code.encode();
      const auto checked = oc::artifact::decode(encoded_artifact, oc::identify(request), encoded_artifact.size());
      BOOST_TEST(checked.image().configuration.target.host_abi_version == 2U);
      BOOST_TEST(checked.image().configuration.target.codegen_version == 3U);
      BOOST_TEST(checked.encode() == encoded_artifact);
      BOOST_TEST(!checked.is_prepared());
      const auto name = darwin ? "infinite-darwin-arm64" : "infinite-linux-x86-64";
      optimizing_fixture_export::write(name, encoded_request, encoded_artifact);
      BOOST_TEST_MESSAGE("fixture=" << name << " identity=" << checked.identity().str()
                         << " wasm=" << checked.image().wasm.str()
                         << " request_bytes=" << encoded_request.size()
                         << " artifact_bytes=" << encoded_artifact.size()
                         << " triple=" << checked.image().configuration.target.triple
                         << " ABI=" << checked.image().configuration.target.host_abi_version
                         << " codegen=" << checked.image().configuration.target.codegen_version
                         << " entry_index=0");
   }
}

BOOST_AUTO_TEST_CASE(export_large_finite_stack_O2_objects_without_native_execution, *boost::unit_test::disabled()) {
   for (const auto parameters : {512U, 1024U}) {
      for (const auto darwin : {false, true}) {
         const auto encoded_request = optimizing_fixture_export::finite_request(parameters, darwin);
         const auto request = oc::compile_request::decode(encoded_request, 2U * 1024U * 1024U);
         const auto code = forge::tooling::wasm_compiler::compiler{}.compile(request);
         BOOST_REQUIRE(code.image().functions.size() == 3U);
         BOOST_REQUIRE(code.image().types.size() == 1U);
         BOOST_REQUIRE(code.image().types[0].params.size() == parameters);
         BOOST_REQUIRE(request.configuration.execution.max_call_depth == 251U);
         auto parsed = vm::module{};
         parsed.allocator.use_default_memory();
         auto debug = vm::null_debug_info{};
         auto input = request.wasm;
         vm::interpreter::parser<std::nullptr_t, vm::options, vm::null_debug_info>{
             parsed.allocator, request.configuration.execution}.parse_module(input, parsed, debug);
         parsed.finalize();
         parsed.validate_instantiation();
         BOOST_REQUIRE(parsed.types.size() == 1U && parsed.type_aliases[0] == 0U);
         const auto canonical_types = std::array<std::uint32_t, 1>{0}; // controlled SAME type only
         auto ir = std::string{};
         const auto object = forge::tooling::wasm_compiler::lowering::emit(
             parsed, code.image(), canonical_types, &ir);
         BOOST_TEST(object == forge::tooling::wasm_compiler::lowering::emit(parsed, code.image(), canonical_types));
         auto aggregate = std::uint64_t{};
         for (const auto& function : code.image().functions) {
            BOOST_TEST(function.stack_bytes > 4096U);
            BOOST_TEST(function.stack_bytes <= request.configuration.limits.max_function_stack_bytes);
            aggregate += function.stack_bytes;
         }
         const auto name = "finite-large-" + std::to_string(parameters) +
                           (darwin ? "-darwin-arm64" : "-linux-x86-64");
         const auto encoded = code.encode();
         const auto checked = oc::artifact::decode(encoded, oc::identify(request), encoded.size());
         BOOST_TEST(checked.encode() == encoded);
         BOOST_TEST(!checked.is_prepared());
         optimizing_fixture_export::write(name, encoded_request, encoded);
         optimizing_fixture_export::write_diagnostic(name, "o", object);
         optimizing_fixture_export::write_diagnostic(name, "ll",
             {reinterpret_cast<const std::uint8_t*>(ir.data()), ir.size()});
         BOOST_TEST_MESSAGE("largefinite=" << name << " identity=" << checked.identity().str()
                            << " frames=" << code.image().functions[0].stack_bytes << ','
                            << code.image().functions[1].stack_bytes << ','
                            << code.image().functions[2].stack_bytes << " aggregate=" << aggregate
                            << " cap=" << request.configuration.limits.max_function_stack_bytes
                            << " canonical_depth=251 native_execution=NOT_RUN");
      }
   }
}

BOOST_AUTO_TEST_CASE(valid_finite_input_over_native_frame_cap_is_typed_compile_limit_not_invalid_wasm) {
   for (const auto darwin : {false, true}) {
      const auto encoded = optimizing_fixture_export::finite_request(2000, darwin);
      const auto request = oc::compile_request::decode(encoded, 2U * 1024U * 1024U);
      BOOST_REQUIRE(request.configuration.limits.max_function_stack_bytes == 16384U);
      BOOST_REQUIRE(request.configuration.execution.max_call_depth == 251U);
      // SAME canonical parser accepts this finite module. The failure is its
      // actual native frame budget, not guest validity or canonical depth.
      auto parsed = vm::module{};
      parsed.allocator.use_default_memory();
      auto debug = vm::null_debug_info{};
      auto input = request.wasm;
      vm::interpreter::parser<std::nullptr_t, vm::options, vm::null_debug_info>{
          parsed.allocator, request.configuration.execution}.parse_module(input, parsed, debug);
      parsed.finalize();
      parsed.validate_instantiation();
      BOOST_REQUIRE(parsed.get_functions_total() == 3U);
      BOOST_REQUIRE(parsed.types.size() == 1U && parsed.types[0].param_types.size() == 2000U);
      BOOST_CHECK_THROW(forge::tooling::wasm_compiler::compiler{}.compile(request), oc::exceptions::compile_limit);
      BOOST_TEST(request.encode() == encoded); // No silent input or budget adjustment.
      BOOST_TEST_MESSAGE("finite_negative_parameters=2000 canonical_parser=PASS typed_compile_limit=4040004 cap=16384 profile="
                         << request.configuration.target.triple << " native_execution=NOT_RUN");
   }
}

BOOST_AUTO_TEST_CASE(actual_O2_ir_has_one_acquire_load_per_entry_and_backedge_and_null_sink_is_identical) {
   for (const auto darwin : {false, true}) {
      const auto request = epoch_loop_request(darwin);
      const auto code = forge::tooling::wasm_compiler::compiler{}.compile(request);
      auto parsed = vm::module{};
      parsed.allocator.use_default_memory();
      auto debug = vm::null_debug_info{};
      auto input = request.wasm;
      vm::interpreter::parser<std::nullptr_t, vm::options, vm::null_debug_info>{
          parsed.allocator, request.configuration.execution}.parse_module(input, parsed, debug);
      parsed.finalize();
      parsed.validate_instantiation();
      BOOST_REQUIRE(parsed.types.size() == 1U);
      BOOST_REQUIRE(parsed.type_aliases[0] == 0U);
      const auto canonical_types = std::array<std::uint32_t, 1>{0}; // controlled one-type fixture only
      auto ir = std::string{};
      const auto with_diagnostic = forge::tooling::wasm_compiler::lowering::emit(
          parsed, code.image(), canonical_types, &ir);
      const auto no_diagnostic = forge::tooling::wasm_compiler::lowering::emit(parsed, code.image(), canonical_types);
      BOOST_TEST(with_diagnostic == no_diagnostic);
      BOOST_TEST(occurrences(ir, "load atomic i64") == 2U); // entry + sole backedge; not caller duplication
      for (auto offset = ir.find("load atomic i64"); offset != ir.npos; offset = ir.find("load atomic i64", offset + 1)) {
         const auto line = std::string_view{ir}.substr(offset, ir.find('\n', offset) - offset);
         BOOST_TEST(line.find(" acquire, align 8") != line.npos);
         BOOST_TEST(line.find("syncscope") == line.npos); // LLVM's default system scope
      }
      BOOST_TEST((ir.find("icmp ult i64") != ir.npos || ir.find("icmp uge i64") != ir.npos));
      BOOST_TEST(ir.find("i64 152") != ir.npos); // SAME offsetof ABI2 threshold
      BOOST_TEST(ir.find("cold") != ir.npos);
      BOOST_TEST(ir.find("invariant.load") == ir.npos);
      BOOST_TEST(ir.find("invariant.group") == ir.npos);
      // SAME one-function O2 oracle: canonical depth and local capacity have
      // distinct constant reasons, in separate guarded blocks before debit.
      const auto depth_check = ir.find("icmp ult i32");
      const auto depth_reason = ir.find(", i32 8)");
      const auto remaining = ir.find("i64 88"); // SAME ABI2 stack_remaining_bytes
      const auto local_reason = ir.find(", i32 12)");
      BOOST_TEST(occurrences(ir, ", i32 8)") == 1U);
      BOOST_TEST(occurrences(ir, ", i32 12)") == 1U);
      BOOST_REQUIRE(depth_check != ir.npos);
      BOOST_REQUIRE(remaining != ir.npos);
      BOOST_TEST(depth_check < depth_reason);
      BOOST_TEST(depth_reason < remaining);
      BOOST_TEST(remaining < local_reason);
      BOOST_TEST(ir.find("icmp ult i64", remaining) < local_reason);
      BOOST_TEST_MESSAGE("exact O2 IR profile=" << request.configuration.target.triple << '\n' << ir);
   }
}

#if (defined(__linux__) && defined(__x86_64__)) || (defined(__APPLE__) && defined(__aarch64__))
BOOST_AUTO_TEST_CASE(actual_infinite_guest_no_tick_stop_callback_is_an_owning_module_primitive_not_lifetime) {
   if (fresh_process::mode() != nullptr) {
      fresh_process::disable_core_dumps();
#if defined(__APPLE__)
      constexpr auto darwin = true;
#else
      constexpr auto darwin = false;
#endif
      // Reuse this module's controlled one-function fixture and actual public
      // compiler/artifact owners. No generic parser/metadata/native_probe copy.
      const auto request = epoch_loop_request(darwin);
      auto original = forge::tooling::wasm_compiler::compiler{}.compile(request);
      const auto encoded = original.encode();
      auto code = oc::artifact::decode(encoded, original.identity(), encoded.size());
      original.prepare();
      code.prepare();
      BOOST_REQUIRE(original.entry(0) != code.entry(0));
      alignas(std::atomic_ref<std::uint64_t>::required_alignment) auto epoch = std::uint64_t{};
      alignas(std::atomic_ref<std::uint32_t>::required_alignment) auto reason = std::uint32_t{};
      std::atomic_ref{epoch}.store(0, std::memory_order_relaxed);
      std::atomic_ref{reason}.store(0, std::memory_order_relaxed);
      auto callbacks = oc::invocation_callbacks{};
      auto context = oc::invocation_abi{};
      callbacks.poll = +[](oc::invocation_abi* context) noexcept {
         const auto observed = std::atomic_ref{*const_cast<std::uint64_t*>(context->epoch)}.load(std::memory_order_acquire);
         const auto reason = std::atomic_ref{*const_cast<std::uint32_t*>(context->sticky_reason)}.load(std::memory_order_acquire);
         if (reason)
            return static_cast<oc::trap_reason>(reason);
         context->epoch_deadline = observed == UINT64_MAX ? UINT64_MAX : observed + 1;
         return oc::trap_reason::none;
      };
      callbacks.trap = +[](oc::invocation_abi*, oc::trap_reason reason) noexcept {
         std::_Exit(32 + static_cast<int>(reason));
      };
      context.callbacks = &callbacks;
      context.epoch = &epoch;
      context.sticky_reason = &reason;
      context.epoch_deadline = 1;
      context.functions = code.functions().data();
      context.function_count = code.functions().size();
      context.max_call_depth = 4;
      context.stack_remaining_bytes = 1024U * 1024U;
      // ACTUAL scenario lives in its owning test unit; no extra std API call
      // exists merely to emit a symbol for an imported ordinary consumer.
      auto stop = std::stop_source{};
      auto callback = std::stop_callback{stop.get_token(), [&]() noexcept {
         auto expected = std::uint32_t{0};
         std::atomic_ref{reason}.compare_exchange_strong(
             expected, static_cast<std::uint32_t>(oc::trap_reason::cancelled),
             std::memory_order_release, std::memory_order_relaxed);
         std::atomic_ref{epoch}.fetch_add(1, std::memory_order_acq_rel);
      }};
      auto publisher = std::thread{[&] {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
         stop.request_stop();
      }};
      auto result = vm::expr_value{};
      code.entry(0)(&context, nullptr, &result);
      publisher.join();
      std::_Exit(99);
   }
   const auto child = fresh_process::run(
       "optimizing_epoch_ir/actual_infinite_guest_no_tick_stop_callback_is_an_owning_module_primitive_not_lifetime",
       "infinite");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 32 + static_cast<int>(oc::trap_reason::cancelled));
   // _Exit confirms generated wake only, never callback destruction/unregister
   // or production compute/NULL/guest-host bridge lifetime acceptance.
}
#endif

BOOST_AUTO_TEST_SUITE_END()
