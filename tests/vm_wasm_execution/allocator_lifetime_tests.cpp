#include <boost/test/unit_test.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.backend;

namespace {
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;

struct stage {
   std::mutex mutex;
   std::condition_variable condition;
   bool entered = false, released = false;
   void arrive_and_wait() {
      auto lock = std::unique_lock{mutex};
      entered = true;
      condition.notify_all();
      condition.wait_for(lock, 2s, [&] { return released; });
   }
   bool await() {
      auto lock = std::unique_lock{mutex};
      return condition.wait_for(lock, 1s, [&] { return entered; });
   }
   void release() {
      const auto lock = std::lock_guard{mutex};
      released = true;
      condition.notify_all();
   }
};

struct owner {
   std::optional<vm::wasm_allocator::borrow> registration;
   stage* retirement = nullptr;
   stage* destruction = nullptr;
   std::atomic<bool>* destroyed = nullptr;
   std::atomic<unsigned>* callbacks = nullptr;
   ~owner() {
      if (destruction)
         destruction->arrive_and_wait();
      if (destroyed)
         destroyed->store(true);
   }
   static void retire(void* pointer, const void* token) noexcept {
      auto& self = *static_cast<owner*>(pointer);
      if (self.callbacks)
         ++*self.callbacks;
      if (self.retirement)
         self.retirement->arrive_and_wait();
      if (self.registration && self.registration->identity() == token)
         self.registration.reset();
   }
};

vm::host_binding effect() {
   return {"env", "effect", {{}, {}}, [](void*, vm::execution_interface) {}};
}

struct converted_argument {
   std::uint32_t value;
};
struct conversion_failure : std::runtime_error {
   conversion_failure() : std::runtime_error("Conversion failure") {}
};
struct conversion_host {
   vm::wasm_allocator* memory;
   unsigned rejected = 0;
   bool fail = false;
   converted_argument produce() {
      return {99};
   }
};
struct converter : vm::type_converter<conversion_host> {
   using type_converter::to_wasm;
   using type_converter::type_converter;
   std::uint32_t to_wasm(converted_argument value) {
      auto& host = this->get_host();
      try {
         host.memory->reset();
         throw std::logic_error("Conversion was not covered by active ownership");
      } catch (const vm::exceptions::busy&) {
         ++host.rejected;
      }
      if (host.fail)
         throw conversion_failure{};
      return value.value;
   }
};
using conversion_functions = vm::registered_host_functions<conversion_host, vm::execution_interface, converter>;
using conversion_backend = vm::backend<conversion_functions, vm::interpreter>;
} // namespace

BOOST_AUTO_TEST_SUITE(allocator_lifetime)

BOOST_AUTO_TEST_CASE(retirement_pins_owner_until_callback_finishes_even_when_context_owner_is_destroyed) {
   auto memory = vm::wasm_allocator{};
   auto barrier = stage{};
   std::atomic<bool> destroyed{false};
   auto value = std::make_shared<owner>();
   value->retirement = &barrier;
   value->destroyed = &destroyed;
   value->registration.emplace(memory, std::weak_ptr<void>{value}, &owner::retire);
   const auto weak = std::weak_ptr{value};
   std::exception_ptr error;
   std::jthread reset([&] {
      try {
         memory.reset();
      } catch (...) {
         error = std::current_exception();
      }
   });
   const auto entered = barrier.await();
   value.reset();
   const auto pinned = !weak.expired();
   const auto destroyed_early = destroyed.load();
   barrier.release();
   reset.join();
   BOOST_TEST(entered);
   BOOST_TEST(pinned);
   BOOST_TEST(!destroyed_early);
   BOOST_TEST(destroyed.load());
   BOOST_CHECK(!error);
   BOOST_TEST(memory.get_current_page() == -1);
}

BOOST_AUTO_TEST_CASE(reset_waits_for_an_already_destroying_owner_to_unregister_without_calling_dangling_callback) {
   auto memory = vm::wasm_allocator{};
   auto barrier = stage{};
   std::atomic<unsigned> callbacks{0};
   auto value = std::make_shared<owner>();
   value->destruction = &barrier;
   value->callbacks = &callbacks;
   value->registration.emplace(memory, std::weak_ptr<void>{value}, &owner::retire);
   std::jthread destroy([owned = std::move(value)]() mutable { owned.reset(); });
   const auto entered = barrier.await();
   std::barrier start{2};
   std::atomic<bool> finished{false};
   std::exception_ptr error;
   std::jthread reset([&] {
      start.arrive_and_wait();
      try {
         memory.reset();
      } catch (...) {
         error = std::current_exception();
      }
      finished = true;
   });
   start.arrive_and_wait();
   std::this_thread::sleep_for(20ms);
   const auto returned_early = finished.load();
   barrier.release();
   destroy.join();
   reset.join();
   BOOST_TEST(entered);
   BOOST_TEST(!returned_early);
   BOOST_TEST(finished.load());
   BOOST_TEST(callbacks.load() == 0U);
   BOOST_CHECK(!error);
   BOOST_CHECK_NO_THROW(vm::wasm_allocator::borrow{memory});
}

BOOST_AUTO_TEST_CASE(delayed_retirement_token_cannot_detach_a_new_registration_on_another_allocator) {
   auto first = vm::wasm_allocator{};
   auto second = vm::wasm_allocator{};
   auto barrier = stage{};
   auto value = std::make_shared<owner>();
   value->retirement = &barrier;
   value->registration.emplace(first, std::weak_ptr<void>{value}, &owner::retire);
   const auto old_token = value->registration->identity();
   std::exception_ptr error;
   std::jthread reset([&] {
      try {
         first.reset();
      } catch (...) {
         error = std::current_exception();
      }
   });
   const auto entered = barrier.await();
   value->registration.reset();
   value->registration.emplace(second, std::weak_ptr<void>{value}, &owner::retire);
   const auto new_token = value->registration->identity();
   barrier.release();
   reset.join();
   BOOST_TEST(entered);
   BOOST_CHECK(old_token != new_token);
   BOOST_CHECK(!error);
   BOOST_REQUIRE(value->registration);
   BOOST_CHECK(value->registration->identity() == new_token);
   BOOST_CHECK_NO_THROW(vm::wasm_allocator::borrow::activity{*value->registration});
   value->registration.reset();
}

BOOST_AUTO_TEST_CASE(real_context_destruction_races_external_reset_without_stale_instance_access) {
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 2), vm::options{}, {effect()}};
   auto memory = vm::wasm_allocator{};
   for (unsigned iteration = 0; iteration < 100; ++iteration) {
      auto context = std::make_unique<vm::runtime_context>(module, 251);
      context->set_wasm_allocator(&memory);
      context->initialize(nullptr);
      std::barrier start{2};
      std::exception_ptr error;
      std::jthread destroy([owned = std::move(context), &start]() mutable {
         start.arrive_and_wait();
         owned.reset();
      });
      start.arrive_and_wait();
      try {
         memory.reset();
      } catch (...) {
         error = std::current_exception();
      }
      destroy.join();
      BOOST_CHECK(!error);
      BOOST_TEST(memory.get_current_page() == -1);
      auto fresh = vm::runtime_context{module, 251};
      fresh.set_wasm_allocator(&memory);
      BOOST_CHECK_NO_THROW(fresh.initialize(nullptr));
      BOOST_TEST(fresh.linear_memory()[0] == 0);
      memory.reset();
      BOOST_CHECK(!fresh.initialized());
      BOOST_CHECK(fresh.linear_memory() == nullptr);
      BOOST_CHECK_THROW(fresh.call(nullptr, "size", {}), vm::exceptions::interpreter);
   }
}

BOOST_AUTO_TEST_CASE(reentrant_start_and_host_reset_reject_before_zeroing_or_protection_changes) {
   auto memory = vm::wasm_allocator{};
   vm::runtime_context* active = nullptr;
   unsigned rejected = 0;
   const auto attempt = [&](auto&& reset) {
      try {
         reset();
      } catch (const vm::exceptions::busy&) {
         ++rejected;
         return;
      }
      throw std::logic_error("Active reset was admitted");
   };
   auto binding = vm::host_binding{"env", "effect", {{}, {}}, [&](void*, vm::execution_interface interface) {
                                      auto* base = static_cast<char*>(interface.get_memory());
                                      base[0] = 42;
                                      const auto pages = memory.get_current_page();
                                      attempt([&] { memory.reset(); });
                                      attempt([&] { memory.reset(0); });
                                      attempt([&] { active->reset(); });
                                      attempt([&] { active->set_wasm_allocator(nullptr); });
                                      attempt([&] { memory.alloc<char>(1); });
                                      attempt([&] { memory.free<char>(1); });
                                      attempt([&] { active->initialize(nullptr); });
                                      if (base[0] != 42 || memory.get_current_page() != pages)
                                         throw std::logic_error("Rejected reset changed memory");
                                   }};
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 3, false, true), vm::options{}, {binding}};
   auto context = vm::runtime_context{module, 251};
   active = &context;
   context.set_wasm_allocator(&memory);
   BOOST_CHECK_NO_THROW(context.initialize(nullptr));
   BOOST_TEST(rejected == 7U);
   BOOST_TEST(context.call(nullptr, "grow", {})->to_ui32() == 1U);
   BOOST_TEST(rejected == 14U);
   BOOST_TEST(memory.get_current_page() == 2);
   memory.reset();
   BOOST_CHECK(!context.initialized());
}

BOOST_AUTO_TEST_CASE(concurrent_reset_and_shared_initialization_reject_while_host_execution_is_active) {
   auto memory = vm::wasm_allocator{};
   auto barrier = stage{};
   auto binding =
       vm::host_binding{"env", "effect", {{}, {}}, [&](void*, vm::execution_interface) { barrier.arrive_and_wait(); }};
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 3), vm::options{}, {binding}};
   auto first = vm::runtime_context{module, 251};
   auto second = vm::runtime_context{module, 251};
   first.set_wasm_allocator(&memory);
   second.set_wasm_allocator(&memory);
   first.initialize(nullptr);
   first.linear_memory()[0] = 42;
   std::exception_ptr error;
   std::jthread call([&] {
      try {
         first.call(nullptr, "grow", {});
      } catch (...) {
         error = std::current_exception();
      }
   });
   const auto entered = barrier.await();
   BOOST_CHECK_THROW(memory.reset(), vm::exceptions::busy);
   BOOST_CHECK_THROW(memory.reset(0), vm::exceptions::busy);
   BOOST_CHECK_THROW(second.initialize(nullptr), vm::exceptions::busy);
   BOOST_TEST(memory.get_current_page() == 2);
   BOOST_TEST(memory.get_base_ptr<char>()[0] == 42);
   barrier.release();
   call.join();
   BOOST_TEST(entered);
   BOOST_CHECK(!error);
   BOOST_CHECK_NO_THROW(second.initialize(nullptr));
   BOOST_CHECK(!first.initialized());
   BOOST_TEST(second.linear_memory()[0] == 0);
}

BOOST_AUTO_TEST_CASE(argument_conversion_and_its_exception_unwind_keep_mapping_active_before_guest_entry) {
   auto wasm = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(wasm, 1, {1, 0x60, 1, 0x7f, 1, 0x7f});
   fixtures::section(wasm, 3, {1, 0});
   fixtures::section(wasm, 4, {1, 0x70, 1, 1, 1});
   fixtures::section(wasm, 5, {1, 1, 1, 2});
   fixtures::section(wasm, 7, {1, 4, 'e', 'c', 'h', 'o', 0, 0});
   fixtures::section(wasm, 9, {1, 0, 0x41, 0, 0x0b, 1, 0});
   fixtures::section(wasm, 10, {1, 4, 0, 0x20, 0, 0x0b});
   auto memory = vm::wasm_allocator{};
   auto host = conversion_host{&memory};
   auto backend = conversion_backend{wasm, host, &memory};
   memory.get_base_ptr<char>()[0] = 42;
   BOOST_TEST(backend.call_with_return(host, "env", "echo", converted_argument{17})->to_ui32() == 17U);
   BOOST_CHECK_NO_THROW(backend.call_indirect(&host, 0, converted_argument{18}));
   BOOST_TEST(host.rejected == 2U);
   host.fail = true;
   BOOST_CHECK_THROW(backend.call_with_return(host, "env", "echo", converted_argument{19}), conversion_failure);
   host.fail = false;
   BOOST_TEST(backend.call_with_return(host, "env", "echo", converted_argument{20})->to_ui32() == 20U);
   BOOST_TEST(host.rejected == 4U);
   BOOST_TEST(memory.get_base_ptr<char>()[0] == 42);
   BOOST_TEST(memory.get_current_page() == 1);
   memory.reset();
   BOOST_CHECK(!backend.get_context().initialized());
}

BOOST_AUTO_TEST_CASE(host_result_conversion_keeps_mapping_active_until_result_is_delivered) {
   static const auto registered = [] {
      conversion_functions::add<&conversion_host::produce>("env", "produce");
      return true;
   }();
   static_cast<void>(registered);
   auto wasm = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(wasm, 1, {1, 0x60, 0, 1, 0x7f});
   fixtures::section(wasm, 2, {1, 3, 'e', 'n', 'v', 7, 'p', 'r', 'o', 'd', 'u', 'c', 'e', 0, 0});
   fixtures::section(wasm, 3, {1, 0});
   fixtures::section(wasm, 5, {1, 1, 1, 2});
   fixtures::section(wasm, 7, {1, 3, 'r', 'u', 'n', 0, 1});
   fixtures::section(wasm, 10, {1, 4, 0, 0x10, 0, 0x0b});
   auto memory = vm::wasm_allocator{};
   auto host = conversion_host{&memory};
   auto backend = conversion_backend{wasm, host, &memory};
   BOOST_TEST(backend.call_with_return(host, "env", "run")->to_ui32() == 99U);
   BOOST_TEST(host.rejected == 1U);
   BOOST_TEST(memory.get_current_page() == 1);
}

BOOST_AUTO_TEST_SUITE_END()
