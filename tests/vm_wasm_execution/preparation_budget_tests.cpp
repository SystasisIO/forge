#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.prepared_module;

namespace {
namespace vm = forge::vm::wasm::interpret;
using budget = vm::prepared_module::preparation;
constexpr std::uint64_t test_ceiling = 1024 * 1024;

const std::string wide_module = "zeta_" + std::string(96, 'z');
const std::string small_module = "alpha_" + std::string(96, 'a');

fixtures::bytes source_module() {
   auto result = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(result, 1, {2, 0x60, 1, 0x7e, 1, 0x7e, 0x60, 1, 0x7f, 1, 0x7f});
   auto imports = fixtures::bytes{2};
   fixtures::text(imports, wide_module);
   fixtures::text(imports, "wide");
   imports.insert(imports.end(), {0, 0});
   fixtures::text(imports, small_module);
   fixtures::text(imports, "small");
   imports.insert(imports.end(), {0, 1});
   fixtures::section(result, 2, imports);
   fixtures::section(result, 7, {2, 4, 'w', 'i', 'd', 'e', 0, 0, 5, 's', 'm', 'a', 'l', 'l', 0, 1});
   return result;
}

struct counters {
   unsigned copies = 0, wide = 0, small = 0;
};
struct wide_callback {
   counters* counts;
   std::array<std::byte, 512> opaque{};
   explicit wide_callback(counters& value) : counts(&value) {}
   wide_callback(const wide_callback& source) : counts(source.counts), opaque(source.opaque) {
      ++counts->copies;
   }
   void operator()(void*, vm::execution_interface interface) const {
      ++counts->wide;
      const auto argument = interface.pop_operand().to_ui64();
      interface.push_operand(vm::i64_const_t{argument + 700});
   }
};

std::vector<vm::host_binding> bindings(counters& counts) {
   std::vector<vm::host_binding> result;
   result.push_back({wide_module, "wide", {{vm::types::i64}, {vm::types::i64}}, wide_callback{counts}});
   result.push_back({small_module,
                     "small",
                     {{vm::types::i32}, {vm::types::i32}},
                     [&counts](void*, vm::execution_interface interface) {
                        ++counts.small;
                        const auto argument = interface.pop_operand().to_ui32();
                        interface.push_operand(vm::i32_const_t{argument + 3});
                     }});
   return result;
}

struct refund {
   std::atomic<bool>* completed;
   explicit refund(std::atomic<bool>& value) : completed(&value) {}
   ~refund() {
      completed->store(true);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(preparation_budget)

BOOST_AUTO_TEST_CASE(root_and_owned_header_boundaries_reject_before_allocation) {
   static_assert(noexcept(budget::retry_pending_cleanup(0)));
   const auto pending = budget::retry_pending_cleanup(0);
   BOOST_TEST(pending.attempted == 0U);
   BOOST_TEST(pending.remaining == 0U);
   const auto root = budget::root_byte_size();
   BOOST_TEST(root > 0U);
   BOOST_TEST(budget::minimum_workspace_bytes() == root);
   BOOST_CHECK_THROW(budget(budget::limits{root - 1, root}), vm::exceptions::execution_unavailable);
   BOOST_CHECK_THROW(budget(budget::limits{root, root - 1}), vm::exceptions::execution_unavailable);
   const auto bytes = root + budget::allocation_overhead() + 17;
   auto exact = budget{{bytes, bytes}};
   auto* address = exact.allocate(17);
   BOOST_TEST(exact.current_bytes() == bytes);
   budget::deallocate(address);
   BOOST_TEST(exact.current_bytes() == root);
   BOOST_TEST(exact.retry_cleanup());
   auto excess = budget{{bytes - 1, bytes - 1}};
   BOOST_CHECK_THROW(excess.allocate(17), vm::exceptions::execution_unavailable);
   BOOST_TEST(excess.current_bytes() == root);
   BOOST_TEST(excess.retry_cleanup());
   BOOST_CHECK_THROW(excess.check(), vm::exceptions::execution_unavailable);
}

BOOST_AUTO_TEST_CASE(allocator_alignment_control_blocks_and_manual_projection_use_one_owner) {
   auto account = budget{{test_ceiling, test_ceiling}};
   const auto root = account.current_bytes();
   auto* aligned = account.allocate(31, 256);
   BOOST_TEST(reinterpret_cast<std::uintptr_t>(aligned) % 256 == 0U);
   const auto backing = account.current_bytes();
   BOOST_TEST(backing >= root + 31 + budget::allocation_overhead());
   {
      std::vector<std::uint64_t, budget::allocator<std::uint64_t>> values(32, 7,
                                                                          budget::allocator<std::uint64_t>{account});
      auto shared = std::allocate_shared<std::uint64_t>(budget::allocator<std::uint64_t>{account}, 99);
      BOOST_TEST(values.back() == 7U);
      BOOST_TEST(*shared == 99U);
      BOOST_TEST(account.current_bytes() > backing + 32 * sizeof(std::uint64_t));
      const auto before = account.current_bytes();
      account.reserve(123);
      BOOST_TEST(account.current_bytes() == before + 123);
      account.release(123);
   }
   BOOST_TEST(account.current_bytes() == backing);
   std::jthread release([&] { budget::deallocate(aligned); });
   release.join();
   BOOST_TEST(account.current_bytes() == root);
}

BOOST_AUTO_TEST_CASE(seal_discards_workspace_headroom_but_keeps_historical_peak) {
   const auto root = budget::root_byte_size();
   const auto resident = root + budget::allocation_overhead() + 32;
   auto account = budget{{resident + 4096, test_ceiling}};
   auto* retained = account.allocate(32);
   auto* temporary = account.allocate(8192);
   budget::deallocate(temporary);
   const auto current = account.current_bytes();
   BOOST_TEST(current == resident);
   BOOST_TEST(account.peak_bytes() > account.configured_limits().resident_bytes);
   BOOST_TEST(account.seal() == current);
   BOOST_CHECK_THROW(account.allocate(1), vm::exceptions::execution_unavailable);
   BOOST_TEST(account.current_bytes() == current);
   budget::deallocate(retained);
   BOOST_TEST(account.current_bytes() == root);
}

BOOST_AUTO_TEST_CASE(seal_rejects_retained_excess_and_cleanup_remains_available) {
   const auto root = budget::root_byte_size();
   auto account = budget{{root, test_ceiling}};
   auto* retained = account.allocate(32);
   BOOST_CHECK_THROW(account.seal(), vm::exceptions::execution_unavailable);
   budget::deallocate(retained);
   BOOST_TEST(account.current_bytes() == root);
}

BOOST_AUTO_TEST_CASE(cancellation_is_typed_before_root_and_after_partial_allocation) {
   auto stopped = std::stop_source{};
   stopped.request_stop();
   BOOST_CHECK_THROW(budget(budget::limits{test_ceiling, test_ceiling}, stopped.get_token()), vm::exceptions::canceled);
   auto stop = std::stop_source{};
   auto account = budget{{test_ceiling, test_ceiling}, stop.get_token()};
   auto* retained = account.allocate(64);
   const auto before = account.current_bytes();
   stop.request_stop();
   BOOST_CHECK_THROW(account.allocate(1), vm::exceptions::canceled);
   BOOST_TEST(account.current_bytes() == before);
   BOOST_TEST(account.retry_cleanup());
   BOOST_CHECK_THROW(account.seal(), vm::exceptions::canceled);
   budget::deallocate(retained);
   BOOST_TEST(account.current_bytes() == budget::root_byte_size());
}

BOOST_AUTO_TEST_CASE(sorted_bindings_preserve_original_ordinals_signatures_and_one_callable) {
   auto code = source_module();
   const auto original = code;
   auto counts = counters{};
   auto functions = bindings(counts);
   const auto copied = counts.copies;
   auto account = budget{{test_ceiling, test_ceiling}};
   const auto root = account.current_bytes();
   {
      auto module = std::allocate_shared<vm::prepared_module>(
          budget::allocator<vm::prepared_module>{account}, code, vm::options{}, std::span{functions},
          vm::prepared_module::purpose::execution, account, vm::execution_mode::interpreter);
      BOOST_TEST(counts.copies == copied);
      BOOST_CHECK(!functions[0].invoke && !functions[1].invoke);
      BOOST_REQUIRE_EQUAL(module->imports.size(), 2U);
      BOOST_CHECK((std::is_same_v<decltype(module->imports), std::vector<vm::module::jit_mod_t::jit_import_entry>>));
      BOOST_TEST(module->imports[0].module_str.size() == wide_module.size());
      BOOST_TEST(module->imports[1].module_str.size() == small_module.size());
      BOOST_TEST(module->function_count() == 2U);
      const auto before = account.current_bytes();
      const auto first = module->function_signature(0);
      const auto second = module->function_signature(1);
      BOOST_CHECK(first.params[0] == vm::types::i64 && first.results[0] == vm::types::i64);
      BOOST_CHECK(second.params[0] == vm::types::i32 && second.results[0] == vm::types::i32);
      for (unsigned i = 0; i < 100; ++i) {
         BOOST_CHECK(module->function_signature(0).params.data() == first.params.data());
         BOOST_TEST(module->native_function_stack_bytes(i % 2) == 0U);
      }
      BOOST_TEST(account.current_bytes() == before);
      BOOST_CHECK_THROW(module->function_signature(2), vm::exceptions::interpreter);
      BOOST_CHECK_THROW(module->native_function_stack_bytes(2), vm::exceptions::interpreter);
      BOOST_TEST(account.seal() == before);
      auto context = vm::runtime_context{*module, 251, vm::execution_mode::interpreter};
      context.initialize(nullptr);
      const std::array<vm::operand_stack_elem, 1> wide{vm::i64_const_t{UINT64_C(0x100000003)}};
      const std::array<vm::operand_stack_elem, 1> small{vm::i32_const_t{17}};
      BOOST_TEST(context.call(nullptr, "wide", wide)->to_ui64() == UINT64_C(0x1000002bf));
      BOOST_TEST(context.call(nullptr, "small", small)->to_ui32() == 20U);
      BOOST_TEST(counts.wide == 1U);
      BOOST_TEST(counts.small == 1U);
      BOOST_TEST(counts.copies == copied);
      BOOST_CHECK(code == original);
   }
   BOOST_TEST(account.current_bytes() == root);
}

BOOST_AUTO_TEST_CASE(failed_link_and_budget_sweep_release_all_forge_preparation_storage) {
   const auto code = source_module();
   auto counts = counters{};
   {
      auto functions = bindings(counts);
      functions[0].signature.params[0] = vm::types::i32;
      auto account = budget{{test_ceiling, test_ceiling}};
      BOOST_CHECK_THROW(vm::prepared_module(code, vm::options{}, std::span{functions},
                                            vm::prepared_module::purpose::execution, account),
                        vm::exceptions::link);
      BOOST_TEST(account.current_bytes() == budget::root_byte_size());
   }
   unsigned refused = 0, admitted = 0;
   for (auto extra : {UINT64_C(1), UINT64_C(64), UINT64_C(512), UINT64_C(4096), test_ceiling}) {
      auto functions = bindings(counts);
      const auto ceiling = budget::root_byte_size() + extra;
      auto account = budget{{ceiling, ceiling}};
      try {
         auto module = vm::prepared_module{code, vm::options{}, std::span{functions},
                                           vm::prepared_module::purpose::execution, account};
         BOOST_TEST(account.current_bytes() <= ceiling);
         BOOST_TEST(account.peak_bytes() <= ceiling);
         ++admitted;
      } catch (const vm::exceptions::execution_unavailable&) {
         ++refused;
      }
      BOOST_TEST(account.current_bytes() == budget::root_byte_size());
   }
   BOOST_TEST(refused > 0U);
   BOOST_TEST(admitted > 0U);
   BOOST_TEST(counts.wide == 0U);
   BOOST_TEST(counts.small == 0U);
}

BOOST_AUTO_TEST_CASE(source_admission_precedes_any_native_load_in_bounded_overload) {
   auto code = fixtures::multiple_results();
   const std::array<std::uint8_t, 8> invalid_native{0, 'a', 'o', 't', 0, 0, 0, 0};
   auto account = budget{{test_ceiling, test_ceiling}};
   BOOST_CHECK_THROW(vm::prepared_module(code, invalid_native, vm::options{}, std::span<vm::host_binding>{},
                                         vm::prepared_module::native_origin::trusted_compiler, account),
                     vm::exceptions::profile);
   BOOST_TEST(account.current_bytes() == budget::root_byte_size());
}

BOOST_AUTO_TEST_CASE(caller_mutation_of_public_imports_is_explicitly_outside_initial_projection_charge) {
   auto counts = counters{};
   auto functions = bindings(counts);
   auto account = budget{{test_ceiling, test_ceiling}};
   {
      auto module = vm::prepared_module{source_module(), vm::options{}, std::span{functions},
                                        vm::prepared_module::purpose::execution, account};
      const auto initial = account.current_bytes();
      module.imports.reserve(module.imports.capacity() + 100);
      module.imports[0].module_str.reserve(module.imports[0].module_str.capacity() + 100);
      BOOST_TEST(account.current_bytes() == initial);
      BOOST_TEST(module.imports.size() == 2U);
   }
   BOOST_TEST(account.current_bytes() == budget::root_byte_size());
}

BOOST_AUTO_TEST_CASE(resident_token_outlives_module_aliases_and_weak_control_storage_across_threads) {
   auto counts = counters{};
   auto functions = bindings(counts);
   std::atomic<bool> refunded{false};
   std::optional<budget> account{std::in_place, budget::limits{test_ceiling, test_ceiling}};
   account->attach_resident_charge(std::make_shared<refund>(refunded));
   auto module = std::allocate_shared<vm::prepared_module>(budget::allocator<vm::prepared_module>{*account},
                                                           source_module(), vm::options{}, std::span{functions},
                                                           vm::prepared_module::purpose::execution, *account);
   auto alias = std::shared_ptr<const void>{module, module.get()};
   auto weak = std::weak_ptr{module};
   account->seal();
   module.reset();
   account.reset();
   BOOST_TEST(!refunded.load());
   alias.reset();
   BOOST_TEST(weak.expired());
   BOOST_TEST(!refunded.load());
   std::jthread release([pin = std::move(weak)]() mutable { pin.reset(); });
   release.join();
   BOOST_TEST(refunded.load());
}

BOOST_AUTO_TEST_CASE(allocation_prefix_keeps_root_and_refund_alive_after_last_handle_is_destroyed) {
   std::atomic<bool> refunded{false};
   std::optional<budget> account{std::in_place, budget::limits{test_ceiling, test_ceiling}};
   account->attach_resident_charge(std::make_shared<refund>(refunded));
   auto* allocation = account->allocate(64);
   account.reset();
   BOOST_TEST(!refunded.load());
   std::jthread release([&] { budget::deallocate(allocation); });
   release.join();
   BOOST_TEST(refunded.load());
}

BOOST_AUTO_TEST_SUITE_END()
