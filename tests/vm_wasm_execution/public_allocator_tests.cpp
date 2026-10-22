#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <sys/mman.h>
#include <thread>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.prepared_module;

namespace {
namespace vm = forge::vm::wasm::interpret;

vm::host_binding effect() {
   return {
       "env", "effect", {{}, {}}, [](void* host, vm::execution_interface) { ++*static_cast<std::uint32_t*>(host); }};
}

std::uint32_t grow(vm::runtime_context& context, std::uint32_t& calls) {
   return context.call(&calls, "grow", {})->to_ui32();
}
} // namespace

BOOST_AUTO_TEST_CASE(allocator_is_the_actual_linear_storage_and_grows_in_place) {
   auto source = fixtures::allocator_memory(1, 3);
   auto module = vm::prepared_module{source, vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   auto context = vm::runtime_context{module, 251};
   context.set_wasm_allocator(&memory);
   context.set_max_pages(2);
   std::uint32_t calls = 0;
   context.initialize(&calls);
   auto* base = memory.get_base_ptr<unsigned char>();
   BOOST_TEST(static_cast<void*>(context.linear_memory()) == static_cast<void*>(base));
   BOOST_TEST(memory.get_current_page() == 1);
   BOOST_TEST(context.current_linear_memory() == 1);
   base[0] = 0xa5;
   BOOST_TEST(grow(context, calls) == 1);
   BOOST_TEST(memory.get_current_page() == 2);
   BOOST_TEST(context.current_linear_memory() == 2);
   BOOST_TEST(base[0] == 0xa5);
   BOOST_TEST(base[65536] == 0);
   BOOST_TEST(base[131071] == 0);
   BOOST_TEST(static_cast<void*>(context.linear_memory()) == static_cast<void*>(base));
   BOOST_TEST(grow(context, calls) == UINT32_MAX);
   BOOST_TEST(calls == 2);
   context.initialize(&calls);
   BOOST_TEST(memory.get_current_page() == 1);
   BOOST_TEST(base[0] == 0);
}

BOOST_AUTO_TEST_CASE(allocator_zero_pages_and_caller_or_module_growth_limits_are_canonical) {
   for (const auto cap : {0U, 1U, 2U}) {
      auto module = vm::prepared_module{fixtures::allocator_memory(0, 2), vm::options{}, {effect()}};
      auto memory = vm::wasm_allocator{};
      auto context = vm::runtime_context{module, 251};
      context.set_wasm_allocator(&memory);
      context.set_max_pages(cap);
      std::uint32_t calls = 0;
      context.initialize(&calls);
      BOOST_TEST(memory.get_current_page() == 0);
      BOOST_TEST(context.current_linear_memory() == 0);
      BOOST_TEST(static_cast<void*>(context.linear_memory()) == static_cast<void*>(memory.get_base_ptr<char>()));
      for (std::uint32_t i = 0; i < cap; ++i)
         BOOST_TEST(grow(context, calls) == i);
      BOOST_TEST(grow(context, calls) == UINT32_MAX);
      BOOST_TEST(context.current_linear_memory() == cap);
      BOOST_TEST(memory.get_current_page() == static_cast<std::int32_t>(cap));
      BOOST_TEST(calls == cap + 1);
   }
}

BOOST_AUTO_TEST_CASE(allocator_nested_host_calls_restore_the_outer_growth_owner) {
   auto inner_module = vm::prepared_module{fixtures::allocator_memory(1, 3), vm::options{}, {effect()}};
   auto inner_memory = vm::wasm_allocator{};
   auto inner = vm::runtime_context{inner_module, 251};
   inner.set_wasm_allocator(&inner_memory);
   std::uint32_t inner_calls = 0, outer_calls = 0;
   inner.initialize(&inner_calls);
   auto binding = vm::host_binding{"env", "effect", {{}, {}}, [&](void*, vm::execution_interface) {
                                      ++outer_calls;
                                      if (grow(inner, inner_calls) != 1)
                                         throw std::runtime_error("Nested growth result mismatch");
                                   }};
   auto outer_module = vm::prepared_module{fixtures::allocator_memory(1, 3, true), vm::options{}, {binding}};
   auto outer_memory = vm::wasm_allocator{};
   auto outer = vm::runtime_context{outer_module, 251};
   outer.set_wasm_allocator(&outer_memory);
   outer.initialize(&outer_calls);
   BOOST_TEST(grow(outer, outer_calls) == 2);
   BOOST_TEST(inner_memory.get_current_page() == 2);
   BOOST_TEST(outer_memory.get_current_page() == 3);
   BOOST_TEST(inner_calls == 1);
   BOOST_TEST(outer_calls == 1);
   BOOST_TEST(static_cast<void*>(inner.linear_memory()) == static_cast<void*>(inner_memory.get_base_ptr<char>()));
   BOOST_TEST(static_cast<void*>(outer.linear_memory()) == static_cast<void*>(outer_memory.get_base_ptr<char>()));
}

BOOST_AUTO_TEST_CASE(allocator_parallel_contexts_use_separate_storage) {
   auto module = vm::prepared_module{fixtures::allocator_memory(0, 2), vm::options{}, {effect()}};
   auto first_memory = vm::wasm_allocator{};
   auto second_memory = vm::wasm_allocator{};
   auto first = vm::runtime_context{module, 251};
   auto second = vm::runtime_context{module, 251};
   first.set_wasm_allocator(&first_memory);
   second.set_wasm_allocator(&second_memory);
   std::barrier start{2};
   std::array<std::exception_ptr, 2> errors{};
   std::array<std::uint32_t, 2> calls{};
   const auto run = [&](vm::runtime_context& context, unsigned index) {
      start.arrive_and_wait();
      try {
         context.initialize(&calls[index]);
         if (grow(context, calls[index]) != 0 || grow(context, calls[index]) != 1)
            throw std::runtime_error("Parallel growth result mismatch");
         context.linear_memory()[0] = static_cast<char>(index + 1);
      } catch (...) {
         errors[index] = std::current_exception();
      }
   };
   std::jthread a([&] { run(first, 0); });
   std::jthread b([&] { run(second, 1); });
   a.join();
   b.join();
   BOOST_CHECK(!errors[0]);
   BOOST_CHECK(!errors[1]);
   BOOST_TEST(first_memory.get_current_page() == 2);
   BOOST_TEST(second_memory.get_current_page() == 2);
   BOOST_TEST(first_memory.get_base_ptr<char>()[0] == 1);
   BOOST_TEST(second_memory.get_base_ptr<char>()[0] == 2);
   BOOST_TEST(calls[0] == 2);
   BOOST_TEST(calls[1] == 2);
}

BOOST_AUTO_TEST_CASE(allocator_idle_handoff_retires_the_previous_instance_before_memory_reset) {
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 2), vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   auto first = vm::runtime_context{module, 251};
   auto second = vm::runtime_context{module, 251};
   first.set_wasm_allocator(&memory);
   second.set_wasm_allocator(&memory);
   std::uint32_t calls = 0;
   first.initialize(&calls);
   first.linear_memory()[0] = 42;
   BOOST_CHECK_NO_THROW(second.initialize(&calls));
   BOOST_CHECK(!first.initialized());
   BOOST_CHECK(first.linear_memory() == nullptr);
   BOOST_TEST(first.memory_size() == 0U);
   BOOST_CHECK_THROW(first.call(&calls, "size", {}), vm::exceptions::interpreter);
   BOOST_TEST(memory.get_current_page() == 1);
   BOOST_TEST(second.linear_memory()[0] == 0);
   BOOST_CHECK_NO_THROW(first.initialize(&calls));
   BOOST_CHECK(!second.initialized());
   BOOST_TEST(first.linear_memory()[0] == 0);
}

BOOST_AUTO_TEST_CASE(allocator_parallel_borrow_has_exactly_one_owner) {
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 2), vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   auto first = vm::runtime_context{module, 251};
   auto second = vm::runtime_context{module, 251};
   first.set_wasm_allocator(&memory);
   second.set_wasm_allocator(&memory);
   std::barrier start{2};
   std::atomic<unsigned> owners{0}, rejected{0};
   std::array<std::exception_ptr, 2> errors{};
   std::uint32_t calls = 0;
   const auto run = [&](vm::runtime_context& context, unsigned index) {
      start.arrive_and_wait();
      try {
         context.initialize(&calls);
         ++owners;
      } catch (const vm::exceptions::busy&) {
         ++rejected;
      } catch (...) {
         errors[index] = std::current_exception();
      }
   };
   std::jthread a([&] { run(first, 0); });
   std::jthread b([&] { run(second, 1); });
   a.join();
   b.join();
   BOOST_TEST(owners.load() >= 1U);
   BOOST_TEST(owners.load() + rejected.load() == 2U);
   BOOST_TEST(static_cast<unsigned>(first.initialized()) + static_cast<unsigned>(second.initialized()) == 1U);
   BOOST_CHECK(!errors[0]);
   BOOST_CHECK(!errors[1]);
   first.reset();
   second.reset();
   BOOST_CHECK_NO_THROW(vm::wasm_allocator::borrow{memory});
}

BOOST_AUTO_TEST_CASE(allocator_zero_length_host_memory_view_keeps_the_borrowed_base) {
   auto memory = vm::wasm_allocator{};
   std::uint32_t calls = 0;
   auto binding =
       vm::host_binding{"env", "effect", {{}, {}}, [&](void*, vm::execution_interface interface) {
                           if (interface.get_memory() != memory.get_base_ptr<void>() || interface.memory_size != 0)
                              throw std::runtime_error("Zero-length host memory view lost its owner");
                           ++calls;
                        }};
   auto module = vm::prepared_module{fixtures::allocator_memory(0, 1), vm::options{}, {binding}};
   auto context = vm::runtime_context{module, 251};
   context.set_wasm_allocator(&memory);
   context.set_max_pages(0);
   context.initialize(nullptr);
   BOOST_TEST(grow(context, calls) == UINT32_MAX);
   BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(allocator_local_growth_failure_is_operational_and_blocks_host_side_effects) {
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 3, false, false, true), vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   auto context = vm::runtime_context{module, 251};
   context.set_wasm_allocator(&memory);
   std::uint32_t calls = 0;
   context.initialize(&calls);
   auto* next = memory.get_base_ptr<char>() + 65536;
   BOOST_REQUIRE(::munmap(next, 65536) == 0);
   BOOST_CHECK_THROW(grow(context, calls), vm::exceptions::execution_unavailable);
   BOOST_TEST(calls == 0);
   BOOST_TEST(memory.get_current_page() == 1);
   context.initialize(&calls);
   BOOST_TEST(context.call(&calls, "size", {})->to_ui32() == 1);
   context.set_max_pages(1);
   context.initialize(&calls);
   BOOST_TEST(grow(context, calls) == UINT32_MAX);
   BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(allocator_failed_initialization_releases_the_borrow_before_reuse) {
   auto failing = vm::prepared_module{fixtures::allocator_memory(1, 2, false, true), vm::options{}, {effect()}};
   auto zero = vm::prepared_module{fixtures::allocator_memory(0, 2), vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   auto context = vm::runtime_context{failing, 251};
   context.set_wasm_allocator(&memory);
   std::uint32_t calls = 0;
   BOOST_REQUIRE(::munmap(memory.get_base_ptr<char>(), 65536) == 0);
   BOOST_CHECK_THROW(context.initialize(&calls), vm::exceptions::execution_unavailable);
   BOOST_TEST(calls == 0);
   BOOST_CHECK(!context.initialized());
   auto recovery = vm::runtime_context{zero, 251};
   recovery.set_wasm_allocator(&memory);
   BOOST_CHECK_NO_THROW(recovery.initialize(&calls));
   BOOST_TEST(memory.get_current_page() == 0);
}

BOOST_AUTO_TEST_CASE(allocator_host_initialization_failure_and_cancellation_allow_fresh_reuse) {
   auto failing_binding = vm::host_binding{"env", "effect", {{}, {}}, [](void*, vm::execution_interface) {
                                              throw std::runtime_error("Host initialization failure");
                                           }};
   auto failing = vm::prepared_module{fixtures::allocator_memory(1, 2, false, true), vm::options{}, {failing_binding}};
   auto good = vm::prepared_module{fixtures::allocator_memory(1, 2), vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   auto first = vm::runtime_context{failing, 251};
   first.set_wasm_allocator(&memory);
   std::uint32_t calls = 0;
   BOOST_CHECK_THROW(first.initialize(&calls), std::runtime_error);
   auto context = vm::runtime_context{good, 251};
   context.set_wasm_allocator(&memory);
   context.initialize(&calls);
   context.linear_memory()[0] = 42;
   auto stop = std::stop_source{};
   std::jthread cancel([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      stop.request_stop();
   });
   BOOST_CHECK_THROW(context.call(&calls, "loop", {}, stop.get_token()), vm::exceptions::canceled);
   cancel.join();
   context.initialize(&calls);
   BOOST_TEST(context.linear_memory()[0] == 0);
   BOOST_TEST(grow(context, calls) == 1);
   BOOST_TEST(calls == 1);
}
