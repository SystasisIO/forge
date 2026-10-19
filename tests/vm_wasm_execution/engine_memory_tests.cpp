#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include "../../libraries/vm/wasm/interpret/details/engine_instance.hxx"
#include "../../libraries/vm/wasm/interpret/details/engine_limits.hxx"
#include <aot_runtime.h>
#include "fixtures.hxx"

namespace {
namespace engine = forge::vm::wasm::interpret::detail;
static_assert(WASM_MEM_ALLOC_WITH_USAGE == 1 && WASM_MEM_ALLOC_WITH_USER_DATA == 0);

bool zero(const void* pointer, std::size_t bytes) {
   const auto* data = static_cast<const unsigned char*>(pointer);
   return std::all_of(data, data + bytes, [](auto value) { return value == 0; });
}
} // namespace

BOOST_AUTO_TEST_CASE(system_provider_zeroes_initial_recycled_and_grown_storage) {
   auto provider = engine::engine_memory::system();
   const engine::engine_memory::scope scope{provider};
   for (unsigned iteration = 0; iteration < 16; ++iteration) {
      auto* data = engine::engine_memory::allocate(Alloc_For_LinearMemory, 65536);
      BOOST_REQUIRE(data);
      BOOST_CHECK(zero(data, 65536));
      std::memset(data, 0xa5, 65536);
      auto* grown = engine::engine_memory::reallocate(Alloc_For_LinearMemory, false, data, 131072);
      BOOST_REQUIRE(grown);
      BOOST_CHECK(std::all_of(static_cast<unsigned char*>(grown), static_cast<unsigned char*>(grown) + 65536,
                              [](auto value) { return value == 0xa5; }));
      BOOST_CHECK(zero(static_cast<unsigned char*>(grown) + 65536, 65536));
      std::memset(grown, 0x5a, 131072);
      engine::engine_memory::release(Alloc_For_LinearMemory, grown);
   }
   BOOST_CHECK(!provider.failed());
}

BOOST_AUTO_TEST_CASE(system_provider_zero_initial_memory_first_growth_is_zeroed) {
   auto provider = engine::engine_memory::system();
   const engine::engine_memory::scope scope{provider};
   auto* data = engine::engine_memory::reallocate(Alloc_For_LinearMemory, false, nullptr, 65536);
   BOOST_REQUIRE(data);
   BOOST_CHECK(zero(data, 65536));
   engine::engine_memory::release(Alloc_For_LinearMemory, data);
   BOOST_CHECK(!provider.failed());
   BOOST_CHECK(!engine::engine_memory::allocate(Alloc_For_LinearMemory, 0));
   BOOST_CHECK(provider.failed());
}

BOOST_AUTO_TEST_CASE(system_provider_actual_instances_zero_initial_and_grown_pages) {
   auto runtime = engine::engine_runtime::acquire();
   auto effect = engine::engine_bindings::function{};
   effect.module = "env";
   effect.name = "effect";
   effect.invoke = [](auto&) {};
   auto bindings = std::make_shared<engine::engine_bindings>(std::vector<engine::engine_bindings::function>{effect});
   for (const auto initial : {0U, 1U}) {
      auto source = std::make_shared<engine::engine_module>(runtime, fixtures::allocator_memory(initial, 2), bindings);
      for (unsigned iteration = 0; iteration < 4; ++iteration) {
         auto instance = engine::engine_instance{source, 65536};
         auto* memory = wasm_runtime_get_memory(instance.get(), 0);
         BOOST_REQUIRE(memory);
         if (initial) {
            auto* base = wasm_memory_get_base_address(memory);
            BOOST_CHECK(zero(base, 65536));
            std::memset(base, 0xa5, 65536);
         }
         std::array<std::uint32_t, 2> cells{};
         BOOST_REQUIRE(instance.invoke("grow", 0, cells));
         BOOST_TEST(cells[0] == initial);
         auto* base = static_cast<unsigned char*>(wasm_memory_get_base_address(memory));
         BOOST_CHECK(zero(base + initial * 65536, 65536));
         if (initial)
            BOOST_TEST(base[0] == 0xa5);
         std::memset(base, 0x5a, (initial + 1) * 65536);
      }
   }
}

BOOST_AUTO_TEST_CASE(coalesced_zero_width_memory_preserves_zero_ceiling_and_repeated_execution) {
   auto code = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(code, 1, {1, 0x60, 0, 1, 0x7f});
   fixtures::section(code, 3, {1, 0});
   fixtures::section(code, 5, {1, 0, 0}); // (memory 0), no memory.size/grow below.
   fixtures::section(code, 7, {1, 3, 'r', 'u', 'n', 0, 0});
   fixtures::section(code, 10, {1, 4, 0, 0x41, 42, 0x0b});
   auto runtime = engine::engine_runtime::acquire();
   for (const auto mode : {
            Mode_Interp,
#if WASM_ENABLE_JIT != 0
            Mode_LLVM_JIT,
#endif
        }) {
      BOOST_TEST_CONTEXT("mode=" << static_cast<unsigned>(mode)) {
         auto budget = engine::engine_load_budget::create(4 * 1024 * 1024);
         auto source = std::make_shared<engine::engine_module>(runtime, code, nullptr, false, false, budget);
         const auto* module = reinterpret_cast<const WASMModule*>(source->get());
         BOOST_REQUIRE_EQUAL(module->memory_count, 1U);
         BOOST_TEST(module->possible_memory_grow == false);
         BOOST_REQUIRE_EQUAL(module->memories[0].num_bytes_per_page, 0U);
         BOOST_TEST(module->memories[0].init_page_count == 0U);
         BOOST_TEST(module->memories[0].max_page_count == 0U);
         if (mode == Mode_LLVM_JIT)
            source->compile_llvm_jit();
         for (const auto cap : {0U, 1U}) {
            for (unsigned iteration = 0; iteration < 4; ++iteration) {
               auto instance = engine::engine_instance{source,     65536,   {},      nullptr, cap,
                                                       UINT32_MAX, nullptr, nullptr, nullptr, mode};
               auto* memory = wasm_runtime_get_memory(instance.get(), 0);
               BOOST_REQUIRE(memory);
               BOOST_TEST(memory->num_bytes_per_page == 0U);
               BOOST_TEST(memory->cur_page_count == 0U);
               BOOST_TEST(memory->max_page_count == 0U);
               BOOST_TEST(memory->memory_data_size == 0U);
               for (unsigned call = 0; call < 4; ++call) {
                  auto cells = std::array<std::uint32_t, 2>{};
                  BOOST_REQUIRE(instance.invoke("run", 0, cells));
                  BOOST_TEST(cells[0] == 42U);
               }
               BOOST_TEST(memory->max_page_count == 0U);
               BOOST_TEST(memory->memory_data_size == 0U);
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(growable_zero_initial_memory_keeps_page_caps_and_initial_admission) {
   auto runtime = engine::engine_runtime::acquire();
   auto calls = 0U;
   auto effect = engine::engine_bindings::function{};
   effect.module = "env";
   effect.name = "effect";
   effect.invoke = [&](auto&) { ++calls; };
   auto bindings = std::make_shared<engine::engine_bindings>(std::vector<engine::engine_bindings::function>{effect});
   for (const auto mode : {
            Mode_Interp,
#if WASM_ENABLE_JIT != 0
            Mode_LLVM_JIT,
#endif
        }) {
      BOOST_TEST_CONTEXT("mode=" << static_cast<unsigned>(mode)) {
         auto budget = engine::engine_load_budget::create(4 * 1024 * 1024);
         auto source = std::make_shared<engine::engine_module>(runtime, fixtures::allocator_memory(0, 2), bindings,
                                                               false, false, budget);
         const auto* module = reinterpret_cast<const WASMModule*>(source->get());
         BOOST_REQUIRE_EQUAL(module->memory_count, 1U);
         BOOST_TEST(module->possible_memory_grow == true);
         BOOST_REQUIRE_EQUAL(module->memories[0].num_bytes_per_page, 65536U);
         if (mode == Mode_LLVM_JIT)
            source->compile_llvm_jit();
         for (const auto cap : {0U, 1U, 2U}) {
            for (unsigned iteration = 0; iteration < 4; ++iteration) {
               auto instance = engine::engine_instance{source,     65536,   {},      nullptr, cap,
                                                       UINT32_MAX, nullptr, nullptr, nullptr, mode};
               auto* memory = wasm_runtime_get_memory(instance.get(), 0);
               BOOST_REQUIRE(memory);
               BOOST_TEST(memory->num_bytes_per_page == 65536U);
               BOOST_TEST(memory->cur_page_count == 0U);
               BOOST_TEST(memory->max_page_count == cap);
               const auto before = calls;
               auto cells = std::array<std::uint32_t, 2>{};
               for (unsigned page = 0; page < cap; ++page) {
                  BOOST_REQUIRE(instance.invoke("grow", 0, cells));
                  BOOST_TEST(cells[0] == page);
                  BOOST_TEST(memory->cur_page_count == page + 1);
               }
               BOOST_REQUIRE(instance.invoke("grow", 0, cells));
               BOOST_TEST(cells[0] == UINT32_MAX);
               BOOST_TEST(calls == before + cap + 1);
               BOOST_REQUIRE(instance.invoke("size", 0, cells));
               BOOST_TEST(cells[0] == cap);
               BOOST_TEST(memory->cur_page_count == cap);
               BOOST_TEST(memory->max_page_count == cap);
               BOOST_TEST(memory->memory_data_size == static_cast<std::uint64_t>(cap) * 65536);
            }
         }
         auto nonempty = std::make_shared<engine::engine_module>(runtime, fixtures::allocator_memory(1, 2), bindings,
                                                                 false, false, budget);
         if (mode == Mode_LLVM_JIT)
            nonempty->compile_llvm_jit();
         const auto before = calls;
         BOOST_CHECK_THROW(
             (engine::engine_instance{nonempty, 65536, {}, nullptr, 0, UINT32_MAX, nullptr, nullptr, nullptr, mode}),
             std::length_error);
         BOOST_TEST(calls == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(instance_metadata_observation_matches_upstream_signed_offset_boundary) {
   const auto prefix = align_uint64(offsetof(AOTModuleInstance, global_table_data.bytes), 8);
   const auto extra = sizeof(AOTModuleInstanceExtra);
   const auto entries =
       static_cast<std::uint32_t>((INT32_MAX - prefix - extra - offsetof(WASMTableInstance, elems)) / sizeof(void*));
   for (const auto count : {entries, entries + 1}) {
      auto wasm = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
      auto table = fixtures::bytes{1, 0x70, 0};
      fixtures::size(table, count);
      fixtures::section(wasm, 4, table);
      const auto module = engine::engine_module{engine::engine_runtime::acquire(), wasm};
      const auto limits = engine::engine_limits{module.get()};
      const auto actual = limits.get(engine::engine_limits::metric::instance_metadata_bytes);
      BOOST_TEST(actual == prefix + wasm_table_instance_size(count, sizeof(void*)) + extra);
      BOOST_TEST((actual <= INT32_MAX) == (count == entries));
   }
}
