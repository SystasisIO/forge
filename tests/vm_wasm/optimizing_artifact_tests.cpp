#include "test_prelude.hpp"

#include <barrier>
#include <boost/describe.hpp>
#include <boost/mp11.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

import forge.vm.wasm.interpret.backend;
import forge.raw.raw;
import forge.vm.wasm.optimizing.artifact;
import forge.vm.wasm.optimizing.exceptions;

namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;

namespace {
oc::compile_request request() {
   auto value = oc::compile_request{};
   value.wasm = {0, 97, 115, 109, 1, 0, 0, 0};
   value.configuration.execution = vm::resolve_options(vm::default_options{});
   value.configuration.imports = {{"env", "second", {{vm::i32}, {}}}, {"env", "first", {{vm::i64}, {vm::i32}}}};
   value.configuration.limits = {1024U * 1024U, 16384};
   return value;
}

oc::artifact_payload payload() {
   const auto source = request();
   auto value = oc::artifact_payload{};
   value.wasm = forge::crypto::digest::sha256::hash(std::span{source.wasm});
   value.configuration = oc::canonicalize(source.configuration);
   // Artifact-owner fixture only, not evidence of a WASM compiler: x86 ret.
   value.sections = {{oc::section_kind::code, 16, {0xc3, 0, 0, 0, 0, 0, 0, 0, 0}},
                     {oc::section_kind::readonly_data, 8, {1, 2, 3, 4, 5, 6, 7, 8}}};
   value.types = {{{}, {}}};
   value.functions = {{{0, 0}, 0, UINT32_MAX, 0}};
   value.exports = {{"run", 0}};
   value.memory = {true, 1, 2};
   value.data = {{1, {0x12, 0x34}}};
   value.globals = {{vm::i32, true, 42}};
   value.table = {0, UINT32_MAX};
   return value;
}

oc::artifact_payload darwin_payload() {
   auto value = payload();
   value.configuration.target.triple = "aarch64-apple-macosx11.0.0-elf";
   value.configuration.target.cpu = "generic";
   value.configuration.target.features = "+fp-armv8,+neon";
   value.sections[0].bytes.assign(68, 0);
   const auto word = [&](std::size_t offset, std::uint32_t bits) {
      for (unsigned i = 0; i < 4; ++i)
         value.sections[0].bytes[offset + i] = static_cast<std::uint8_t>(bits >> (8 * i));
   };
   word(0, 0x94000000U);
   word(4, 0x14000000U);
   word(8, 0x90000000U);
   word(12, 0x91000000U);
   word(16, 0x39400000U);
   word(20, 0x79400000U);
   word(24, 0xb9400000U);
   word(28, 0xf9400000U);
   word(64, 0xd65f03c0U); // Actual ret entry; relocation slots are metadata probes.
   value.functions[0].entry.offset = 64;
   value.relocations = {{oc::relocation_kind::aarch64_call26, {0, 0}, {0, 64}, 0},
                        {oc::relocation_kind::aarch64_jump26, {0, 4}, {0, 64}, 0},
                        {oc::relocation_kind::aarch64_page21, {0, 8}, {1, 0}, 0},
                        {oc::relocation_kind::aarch64_add12, {0, 12}, {1, 0}, 0},
                        {oc::relocation_kind::aarch64_load_store8, {0, 16}, {1, 0}, 1},
                        {oc::relocation_kind::aarch64_load_store16, {0, 20}, {1, 0}, 2},
                        {oc::relocation_kind::aarch64_load_store32, {0, 24}, {1, 0}, 4},
                        {oc::relocation_kind::aarch64_load_store64, {0, 28}, {1, 0}, 0},
                        {oc::relocation_kind::relative32, {0, 32}, {0, 64}, 0},
                        {oc::relocation_kind::absolute64, {0, 40}, {1, 0}, 0}};
   return value;
}

template <typename T> void mutate_scalar(T& value) {
   if constexpr (std::is_same_v<T, bool>)
      value = !value;
   else if constexpr (std::is_enum_v<T>)
      value = static_cast<T>(static_cast<std::underlying_type_t<T>>(value) ^ 1);
   else
      ++value;
}
} // namespace

BOOST_AUTO_TEST_SUITE(optimizing_artifact)

BOOST_AUTO_TEST_CASE(request_canonical_raw_roundtrip_and_digest_identity) {
   const auto original = request();
   const auto expected = oc::identify(original);
   auto reordered = original;
   std::reverse(reordered.configuration.imports.begin(), reordered.configuration.imports.end());
   BOOST_TEST(oc::identify(reordered) == expected);
   BOOST_TEST(original.encode() == reordered.encode());
   const auto decoded = oc::compile_request::decode(original.encode(), 1024U * 1024U);
   BOOST_TEST((decoded.configuration == oc::canonicalize(original.configuration)));
   BOOST_TEST(decoded.wasm == original.wasm);
   BOOST_TEST(oc::identify(decoded) == expected);
   auto changed = original;
   changed.wasm.back() ^= 1;
   BOOST_TEST(oc::identify(changed) != expected);
   changed = original;
   changed.configuration.imports[0].signature.params[0] = vm::f32;
   BOOST_TEST(oc::identify(changed) != expected);
   changed = original;
   changed.configuration.imports[0].module = "different";
   BOOST_TEST(oc::identify(changed) != expected);
   changed = original;
   changed.configuration.imports[0].name = "different";
   BOOST_TEST(oc::identify(changed) != expected);
   auto bytes = original.encode();
   bytes.push_back(0);
   BOOST_CHECK_THROW(oc::compile_request::decode(bytes, bytes.size()), forge::exceptions::base);
   BOOST_CHECK_THROW(oc::compile_request::decode(original.encode(), 1), oc::exceptions::compile_limit);
}

BOOST_AUTO_TEST_CASE(identity_includes_every_execution_option_target_and_compile_budget_field) {
   const auto original = request();
   const auto expected = oc::identify(original);
   using fields = boost::describe::describe_members<vm::options, boost::describe::mod_public>;
   auto count = 0U;
   boost::mp11::mp_for_each<fields>([&](auto descriptor) {
      auto changed = original;
      mutate_scalar(changed.configuration.execution.*descriptor.pointer);
      BOOST_TEST(oc::identify(changed) != expected);
      ++count;
   });
   BOOST_TEST(count == 35U);
   using target_fields = boost::describe::describe_members<oc::target_profile, boost::describe::mod_public>;
   boost::mp11::mp_for_each<target_fields>([&](auto descriptor) {
      auto changed = original;
      auto& value = changed.configuration.target.*descriptor.pointer;
      if constexpr (std::is_same_v<std::remove_reference_t<decltype(value)>, std::string>)
         value += "changed";
      else
         mutate_scalar(value);
      BOOST_CHECK_THROW(oc::identify(changed), oc::exceptions::unsupported_target);
   });
   using budget_fields = boost::describe::describe_members<oc::compile_limits, boost::describe::mod_public>;
   boost::mp11::mp_for_each<budget_fields>([&](auto descriptor) {
      auto changed = original;
      mutate_scalar(changed.configuration.limits.*descriptor.pointer);
      BOOST_TEST(oc::identify(changed) != expected);
   });
   auto changed = original;
   ++changed.configuration.version;
   BOOST_CHECK_THROW(oc::identify(changed), oc::exceptions::invalid_module);
}

BOOST_AUTO_TEST_CASE(duplicate_and_unsupported_import_signatures_are_typed_errors) {
   auto value = request();
   value.configuration.imports.push_back(value.configuration.imports.front());
   BOOST_CHECK_THROW(oc::identify(value), oc::exceptions::unsupported_import);
   value = request();
   value.configuration.imports[0].signature.ret = {vm::i32, vm::i64};
   BOOST_CHECK_THROW(oc::identify(value), oc::exceptions::unsupported_import);
   value = request();
   value.configuration.imports[0].signature.params = {vm::anyfunc};
   BOOST_CHECK_THROW(oc::identify(value), oc::exceptions::unsupported_import);
}

BOOST_AUTO_TEST_CASE(abi2_layout_codegen3_and_old_profiles_reject_before_prepared_publication) {
   BOOST_TEST(oc::invocation_abi::current_version == 2U);
   BOOST_TEST(sizeof(oc::invocation_abi) == 168U);
   BOOST_TEST(offsetof(oc::invocation_abi, epoch) == 144U);
   BOOST_TEST(offsetof(oc::invocation_abi, epoch_deadline) == 152U);
   BOOST_TEST(offsetof(oc::invocation_abi, sticky_reason) == 160U);
   BOOST_TEST(oc::target_profile{}.codegen_version == 3U);
   BOOST_TEST(static_cast<std::uint32_t>(oc::trap_reason::stack_overflow) == 8U);
   BOOST_TEST(static_cast<std::uint32_t>(oc::trap_reason::host_exception) == 11U);
   BOOST_TEST(static_cast<std::uint32_t>(oc::trap_reason::native_stack_exhausted) == 12U);
   for (const auto darwin : {false, true}) {
      const auto original = darwin ? darwin_payload() : payload();
      const auto accepted = oc::artifact::from_payload(original);
      BOOST_TEST(!accepted.is_prepared());
      for (const auto versions : {std::pair{1U, 1U}, std::pair{1U, 2U}, std::pair{2U, 1U},
                                  std::pair{2U, 2U}, std::pair{1U, 3U}, std::pair{2U, 4U}}) {
         auto obsolete = original;
         obsolete.configuration.target.host_abi_version = versions.first;
         obsolete.configuration.target.codegen_version = versions.second;
         BOOST_CHECK_THROW(oc::canonicalize(obsolete.configuration), oc::exceptions::unsupported_target);
         BOOST_CHECK_THROW(oc::artifact::from_payload(obsolete), oc::exceptions::unsupported_target);
         // Adversarial old-format fixture uses SAME Raw field order/hash owner,
         // not a production codec/key algorithm. No old artifact can be built
         // by the new validated from_payload() API.
         const auto checksum = forge::crypto::digest::sha256::hash(forge::raw::pack(obsolete));
         const auto encoded = forge::raw::pack(std::tuple{oc::artifact::current_version, checksum, obsolete});
         const auto old_identity = forge::crypto::digest::sha256::packhash(obsolete.wasm, obsolete.configuration);
         BOOST_CHECK_THROW(oc::artifact::decode(encoded, old_identity, encoded.size()),
                           oc::exceptions::unsupported_target);
         BOOST_TEST(!accepted.is_prepared());
      }
   }
}

BOOST_AUTO_TEST_CASE(artifact_roundtrip_pins_one_checked_view_without_source_or_native_addresses) {
   const auto image = oc::artifact::from_payload(payload());
   const auto encoded = image.encode();
   auto decoded = oc::artifact::decode(encoded, image.identity(), encoded.size());
   BOOST_TEST(decoded.identity() == image.identity());
   BOOST_TEST(decoded.encode() == encoded);
   BOOST_TEST(decoded.image().sections[0].bytes == image.image().sections[0].bytes);
   BOOST_TEST(!decoded.is_prepared());
   BOOST_CHECK_THROW(decoded.native_code(), oc::exceptions::compiler_unavailable);
   BOOST_CHECK_THROW(decoded.functions(), oc::exceptions::compiler_unavailable);
   const auto copy = decoded;
   BOOST_TEST(&copy.image() == &decoded.image());
   BOOST_TEST(copy.byte_size() >= encoded.size());
   auto other = request();
   other.wasm.push_back(0);
   BOOST_CHECK_THROW(oc::artifact::decode(encoded, oc::identify(other), encoded.size()),
                     oc::exceptions::invalid_artifact);
   auto corrupted = encoded;
   corrupted.back() ^= 1;
   BOOST_CHECK_THROW(oc::artifact::decode(corrupted, image.identity(), corrupted.size()),
                     oc::exceptions::invalid_artifact);
   corrupted = encoded;
   corrupted.pop_back();
   BOOST_CHECK_THROW(oc::artifact::decode(corrupted, image.identity(), corrupted.size()),
                     oc::exceptions::invalid_artifact);
   corrupted = encoded;
   corrupted.push_back(0);
   BOOST_CHECK_THROW(oc::artifact::decode(corrupted, image.identity(), corrupted.size()),
                     oc::exceptions::invalid_artifact);
   BOOST_CHECK_THROW(oc::artifact::decode(encoded, image.identity(), 1), oc::exceptions::compile_limit);
}

BOOST_AUTO_TEST_CASE(prospective_size_keeps_artifacts_cold_and_identity_unchanged_for_both_profiles) {
   for (const auto& source : {payload(), darwin_payload()}) {
      for (const auto count : {0U, 1U, 3U, 17U}) {
         auto value = source;
         value.functions.assign(count, source.functions.front());
         if (!count) {
            value.exports.clear();
            value.table.clear();
         }
         const auto image = oc::artifact::from_payload(value);
         const auto encoded = image.encode();
         const auto identity = image.identity();
         const auto allocated = image.byte_size();
         const auto prospective = image.prepared_byte_size();
         // This fixture needs two 64KiB image regions plus a full alignment
         // reservation. The native table has exact count, not vector capacity.
         BOOST_TEST(prospective - allocated == 3U * 65536U + count * sizeof(oc::native_function));
         BOOST_TEST(image.prepared_byte_size() == prospective);
         BOOST_TEST(image.byte_size() == allocated);
         BOOST_TEST(!image.is_prepared());
         BOOST_CHECK_THROW(image.native_code(), oc::exceptions::compiler_unavailable);
         BOOST_CHECK_THROW(image.functions(), oc::exceptions::compiler_unavailable);
         BOOST_TEST(image.identity() == identity);
         BOOST_TEST(image.encode() == encoded);
         const auto decoded = oc::artifact::decode(encoded, identity, encoded.size());
         BOOST_TEST(decoded.identity() == identity);
         BOOST_TEST(decoded.encode() == encoded);
         BOOST_TEST(decoded.prepared_byte_size() - decoded.byte_size() == prospective - allocated);
         BOOST_TEST(!decoded.is_prepared());
      }
   }
   auto invalid = payload();
   invalid.configuration.target.cpu = "not-the-canonical-cpu";
   BOOST_CHECK_THROW(oc::artifact::from_payload(invalid), oc::exceptions::unsupported_target);
   invalid = payload();
   invalid.sections[0].alignment = UINT64_MAX;
   BOOST_CHECK_THROW(oc::artifact::from_payload(invalid), oc::exceptions::invalid_artifact);
   invalid = payload();
   invalid.configuration.limits.max_generated_bytes = 2U * 65536U - 1;
   BOOST_CHECK_THROW(oc::artifact::from_payload(invalid), oc::exceptions::compile_limit);
}

BOOST_AUTO_TEST_CASE(ownership_predicate_observes_pins_without_mutating_artifact_state) {
   const auto image = oc::artifact::from_payload(payload());
   BOOST_TEST(image.is_unique());
   const auto identity = image.identity();
   auto phase = std::barrier{2};
   auto worker = std::thread{[&] {
      for (unsigned iteration = 0; iteration < 64; ++iteration) {
         auto copy = std::optional<oc::artifact>{image};
         phase.arrive_and_wait();
         phase.arrive_and_wait();
         copy.reset();
         phase.arrive_and_wait();
         phase.arrive_and_wait();
      }
   }};
   for (unsigned iteration = 0; iteration < 64; ++iteration) {
      phase.arrive_and_wait();
      BOOST_TEST(!image.is_unique());
      phase.arrive_and_wait();
      phase.arrive_and_wait();
      BOOST_TEST(image.is_unique()); // External exclusion, not use_count, keeps this observation stable.
      phase.arrive_and_wait();
   }
   worker.join();
   BOOST_TEST(image.is_unique());
   BOOST_TEST(image.identity() == identity);
   BOOST_TEST(!image.is_prepared());
}

#if (defined(__linux__) && defined(__x86_64__)) || (defined(__APPLE__) && defined(__aarch64__))
BOOST_AUTO_TEST_CASE(prospective_accounting_matches_actual_prepared_storage_and_shared_pins) {
#if defined(__APPLE__)
   const auto source = darwin_payload();
#else
   const auto source = payload();
#endif
   for (const auto count : {0U, 1U, 3U, 17U}) {
      auto value = source;
      value.functions.assign(count, source.functions.front());
      if (!count) {
         value.exports.clear();
         value.table.clear();
      }
      const auto image = oc::artifact::from_payload(value);
      const auto encoded = image.encode();
      const auto prospective = image.prepared_byte_size();
      image.prepare();
      BOOST_TEST(image.is_prepared());
      BOOST_TEST(image.byte_size() == prospective);
      BOOST_TEST(image.prepared_byte_size() == image.byte_size());
      BOOST_TEST(image.functions().size() == count);
      BOOST_TEST(image.is_unique());
      {
         const auto copy = image;
         BOOST_TEST(!image.is_unique());
         BOOST_TEST(!copy.is_unique());
         BOOST_TEST(copy.functions().data() == image.functions().data());
         BOOST_TEST(copy.native_code().data() == image.native_code().data());
         BOOST_TEST(copy.prepared_byte_size() == prospective);
      }
      BOOST_TEST(image.is_unique());
      BOOST_TEST(image.encode() == encoded);
      const auto decoded = oc::artifact::decode(encoded, image.identity(), encoded.size());
      const auto decoded_prospective = decoded.prepared_byte_size();
      decoded.prepare();
      BOOST_TEST(decoded.byte_size() == decoded_prospective);
      BOOST_TEST(decoded.identity() == image.identity());
      BOOST_TEST(decoded.encode() == encoded);
   }
}
#endif

BOOST_AUTO_TEST_CASE(invalid_metadata_and_unknown_overlapping_relocations_are_rejected_before_mapping) {
   auto value = payload();
   value.sections[0].alignment = 3;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.functions[0].entry.offset = value.sections[0].bytes.size();
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.functions[0].stack_bytes = value.configuration.limits.max_function_stack_bytes + 1;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::compile_limit);
   value = payload();
   value.types.push_back(value.types.front());
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.start = 1;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.table[0] = 1;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.globals[0].bits = UINT64_MAX;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.data[0].offset = vm::page_size;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.relocations = {{static_cast<oc::relocation_kind>(99), {0, 1}, {1, 0}, 0}};
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = payload();
   value.relocations = {{oc::relocation_kind::absolute64, {0, 1}, {1, 0}, 0},
                        {oc::relocation_kind::relative32, {0, 2}, {1, 0}, -4}};
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
}

BOOST_AUTO_TEST_CASE(one_prepare_publication_or_explicit_unsupported_host) {
   const auto image = oc::artifact::from_payload(payload());
#if defined(__linux__) && defined(__x86_64__)
   auto phase = std::barrier{3};
   const auto worker = [&] {
      phase.arrive_and_wait();
      image.prepare();
   };
   auto first = std::thread{worker};
   auto second = std::thread{worker};
   phase.arrive_and_wait();
   first.join();
   second.join();
   BOOST_TEST(image.is_prepared());
   const auto copy = image;
   BOOST_TEST(copy.native_code().data() == image.native_code().data());
   BOOST_TEST(copy.functions().data() == image.functions().data());
   BOOST_TEST(copy.entry(0) == image.entry(0));
   BOOST_TEST(copy.encode() == image.encode());
   BOOST_CHECK_THROW(image.entry(1), oc::exceptions::invalid_artifact);
   auto context = oc::invocation_abi{};
   image.entry(0)(&context, nullptr, nullptr);
#else
   BOOST_CHECK_THROW(image.prepare(), oc::exceptions::unsupported_target);
   BOOST_TEST(!image.is_prepared());
   BOOST_CHECK_THROW(image.prepare(), oc::exceptions::unsupported_target);
   BOOST_TEST(!image.is_prepared());
#endif
}

BOOST_AUTO_TEST_CASE(shared_native_abi_is_one_standard_layout_and_noexcept_callback_contract) {
   auto context = oc::invocation_abi{};
   BOOST_TEST(context.version == oc::invocation_abi::current_version);
   BOOST_TEST(context.size == sizeof(context));
   BOOST_TEST(alignof(oc::invocation_abi) == 8U);
   BOOST_TEST(sizeof(oc::native_function) == 24U);
   BOOST_TEST(sizeof(oc::table_entry) == 8U);
   BOOST_TEST(offsetof(oc::invocation_abi, memory_base) == 8U);
   static_assert(std::is_nothrow_invocable_r_v<oc::trap_reason, oc::numeric_callback, oc::invocation_abi*,
                                               std::uint32_t, std::uint32_t, const vm::expr_value*, vm::expr_value*>);
}

BOOST_AUTO_TEST_CASE(canonical_os_abi_profiles_have_distinct_raw_identities_and_reject_aliases) {
   auto linux_request = request();
   auto darwin = linux_request;
   darwin.configuration.target = darwin_payload().configuration.target;
   BOOST_TEST(oc::identify(linux_request) != oc::identify(darwin));
   BOOST_TEST((oc::compile_request::decode(darwin.encode(), 1024 * 1024).configuration ==
               oc::canonicalize(darwin.configuration)));
   for (const auto triple : {"aarch64-unknown-linux-gnu", "arm64-apple-darwin", "aarch64-apple-macosx11.0.0"}) {
      auto invalid = darwin;
      invalid.configuration.target.triple = triple;
      BOOST_CHECK_THROW(oc::identify(invalid), oc::exceptions::unsupported_target);
   }
   auto invalid = darwin;
   invalid.configuration.target.features = "+neon,+fp-armv8";
   BOOST_CHECK_THROW(oc::identify(invalid), oc::exceptions::unsupported_target);
   auto value = payload();
   value.relocations = {{oc::relocation_kind::aarch64_call26, {0, 0}, {0, 0}, 0}};
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
}

BOOST_AUTO_TEST_CASE(aarch64_relocations_require_real_opcodes_alignment_extents_and_ranges) {
   const auto original = darwin_payload();
   const auto image = oc::artifact::from_payload(original);
   const auto encoded = image.encode();
   BOOST_TEST(oc::artifact::decode(encoded, image.identity(), encoded.size()).encode() == encoded);
   for (std::size_t index = 0; index < 8; ++index) {
      auto value = original;
      value.sections[0].bytes[value.relocations[index].source.offset + 3] = 0;
      BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
      value = original;
      ++value.relocations[index].source.offset;
      BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   }
   for (const auto index : {0U, 1U, 5U, 6U, 7U}) {
      auto value = original;
      ++value.relocations[index].addend;
      BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   }
   auto value = original;
   value.sections[0].alignment = 2;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = original;
   value.sections[0].bytes.push_back(0);
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = original;
   value.relocations[2].addend = 8; // One past readonly section is not an instruction address operand.
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = original;
   value.relocations[0].target.section = 1; // Branches must address code.
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = original;
   value.functions[0].stack_bytes = 8;
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
   value = original;
   value.configuration.limits.max_generated_bytes = 256U * 1024U * 1024U;
   // Sparse aligned sections put the last byte at the exact CALL26 positive
   // limit without allocating 128MiB of fixture storage.
   value.sections.assign(2049, {oc::section_kind::code, 65536, {0, 0, 0, 0x94}});
   value.functions[0].entry.offset = 0;
   value.relocations = {{oc::relocation_kind::aarch64_call26, {0, 0}, {2048, 0}, 0}};
   BOOST_CHECK_THROW(oc::artifact::from_payload(value), oc::exceptions::invalid_artifact);
}

#if defined(__APPLE__) && defined(__aarch64__)
BOOST_AUTO_TEST_CASE(darwin_native_mapping_patches_all_admitted_relocations_and_separates_decoded_image) {
   const auto image = oc::artifact::from_payload(darwin_payload());
   const auto encoded = image.encode();
   const auto decoded = oc::artifact::decode(encoded, image.identity(), encoded.size());
   image.prepare();
   decoded.prepare();
   BOOST_TEST(static_cast<bool>(image.entry(0) != decoded.entry(0)));
   BOOST_TEST(image.encode() == decoded.encode());
   for (const auto* artifact : {&image, &decoded}) {
      const auto code = artifact->native_code();
      const auto read_word = [&](std::size_t offset) {
         auto word = std::uint32_t{0};
         std::memcpy(&word, code.data() + offset, sizeof(word));
         return word;
      };
      BOOST_TEST(read_word(0) == 0x94000010U);
      BOOST_TEST(read_word(4) == 0x1400000fU);
      BOOST_TEST(read_word(8) == 0x90000080U);
      BOOST_TEST(read_word(12) == 0x91000000U);
      BOOST_TEST(read_word(16) == 0x39400400U);
      BOOST_TEST(read_word(20) == 0x79400400U);
      BOOST_TEST(read_word(24) == 0xb9400400U);
      BOOST_TEST(read_word(28) == 0xf9400000U);
      BOOST_TEST(read_word(32) == 32U);
      auto absolute = std::uint64_t{0};
      std::memcpy(&absolute, code.data() + 40, sizeof(absolute));
      BOOST_TEST(absolute == reinterpret_cast<std::uintptr_t>(code.data()) + 65536);
      auto context = oc::invocation_abi{};
      artifact->entry(0)(&context, nullptr, nullptr);
   }
   const auto wrong_os = oc::artifact::from_payload(payload());
   BOOST_CHECK_THROW(wrong_os.prepare(), oc::exceptions::unsupported_target);
   BOOST_TEST(!wrong_os.is_prepared());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
