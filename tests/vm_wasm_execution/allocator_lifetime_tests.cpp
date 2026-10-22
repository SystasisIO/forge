#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
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

fixtures::bytes owner_module() {
   auto code = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(code, 1, {2, 0x60, 3, 0x7e, 0x7e, 0x7e, 0, 0x60, 0, 0});
   fixtures::section(code, 2, {1, 3, 'e', 'n', 'v', 6, 'e', 'f', 'f', 'e', 'c', 't', 0, 0});
   fixtures::section(code, 3, {2, 0, 1});
   fixtures::section(code, 5, {1, 1, 1, 2});
   fixtures::section(code, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
   fixtures::section(code, 7, {1, 5, 'a', 'p', 'p', 'l', 'y', 0, 1});
   fixtures::section(code, 8, {2});
   auto bodies = fixtures::bytes{2};
   for (const auto& body :
        {fixtures::bytes{0,    0x23, 0,    0x41, 7,    0x47, 0x04, 0x40, 0,    0x0b, 0x41, 0, 0x2d, 0,
                         0,    0x41, 7,    0x47, 0x04, 0x40, 0,    0x0b, 0x20, 0,    0x20, 1, 0x20, 2,
                         0x10, 0,    0x41, 9,    0x24, 0,    0x41, 0,    0x41, 9,    0x3a, 0, 0,    0x0b},
         fixtures::bytes{0, 0x41, 7, 0x24, 0, 0x41, 0, 0x41, 7, 0x3a, 0, 0, 0x0b}}) {
      fixtures::size(bodies, body.size());
      bodies.insert(bodies.end(), body.begin(), body.end());
   }
   fixtures::section(code, 10, bodies);
   return code;
}

struct owner_host {
   unsigned calls = 0;
   void effect(std::uint64_t first, std::uint64_t second, std::uint64_t third) {
      if (first != 11 || second != 22 || third != 33)
         throw std::logic_error("Retained host binding received different arguments");
      ++calls;
   }
};
using owner_functions = vm::registered_host_functions<owner_host>;
using owner_backend = vm::backend<owner_functions, vm::interpreter, vm::options>;

vm::host_binding owner_binding(std::shared_ptr<unsigned> pin = {}) {
   return {"env",
           "effect",
           {{vm::types::i64, vm::types::i64, vm::types::i64}, {}},
           [pin = std::move(pin)](void* host, vm::execution_interface interface) {
              const auto third = interface.pop_operand().to_ui64();
              const auto second = interface.pop_operand().to_ui64();
              const auto first = interface.pop_operand().to_ui64();
              if (pin)
                 ++*pin;
              static_cast<owner_host*>(host)->effect(first, second, third);
           }};
}

const std::array<vm::operand_stack_elem, 3> owner_arguments{
    vm::i64_const_t{UINT64_C(11)}, vm::i64_const_t{UINT64_C(22)}, vm::i64_const_t{UINT64_C(33)}};

struct charge_lifetime {
   bool* released;
   explicit charge_lifetime(bool& value) : released(&value) {}
   ~charge_lifetime() {
      *released = true;
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(allocator_lifetime)

BOOST_AUTO_TEST_CASE(external_context_survives_backend_destruction_allocator_retirement_and_reinitialization) {
   static const auto registered = [] {
      owner_functions::add<&owner_host::effect>("env", "effect");
      return true;
   }();
   static_cast<void>(registered);
   auto code = owner_module();
   auto pointer = vm::wasm_code_ptr{code.data(), code.size()};
   auto backend = std::make_unique<owner_backend>(pointer, code.size(), nullptr, vm::options{}, true, false);
   auto memory = vm::wasm_allocator{};
   auto context = std::make_unique<vm::runtime_context>(backend->get_module(), 251, vm::execution_mode::interpreter);
   const auto index = backend->get_module().get_exported_function("apply");
   context->initialize_globals();
   backend->set_context(context.get());
   backend->set_wasm_allocator(&memory);
   auto host = owner_host{};
   backend->initialize(&host);
   BOOST_TEST(context->linear_memory()[0] == 7);
   context->call(&host, "apply", owner_arguments);
   BOOST_TEST(context->linear_memory()[0] == 9);
   backend.reset();
   BOOST_CHECK_NO_THROW(memory.reset());
   BOOST_CHECK(!context->initialized());
   BOOST_CHECK(context->has_linear_memory());
   BOOST_CHECK_NO_THROW(context->initialize(&host));
   BOOST_TEST(context->linear_memory()[0] == 7);
   context->call(&host, index, owner_arguments);
   BOOST_TEST(context->linear_memory()[0] == 9);
   BOOST_TEST(host.calls == 2U);
   context.reset();
   BOOST_CHECK_NO_THROW(memory.reset());
   BOOST_TEST(memory.get_current_page() == -1);
}

BOOST_AUTO_TEST_CASE(external_context_can_first_initialize_and_resolve_metadata_after_wrapper_destruction) {
   auto module = std::make_unique<vm::prepared_module>(owner_module(), vm::options{},
                                                       std::vector<vm::host_binding>{owner_binding()});
   auto memory = vm::wasm_allocator{};
   auto context = std::make_unique<vm::runtime_context>(*module, 251, vm::execution_mode::interpreter);
   context->set_wasm_allocator(&memory);
   module.reset();
   BOOST_CHECK(context->has_linear_memory());
   auto host = owner_host{};
   context->initialize(&host);
   BOOST_TEST(context->linear_memory()[0] == 7);
   BOOST_CHECK_THROW(context->call(&host, "absent", owner_arguments), vm::exceptions::interpreter);
   BOOST_CHECK_THROW(context->call(&host, "apply", {}), vm::exceptions::interpreter);
   context->call(&host, "apply", owner_arguments);
   BOOST_TEST(host.calls == 1U);
   BOOST_TEST(context->linear_memory()[0] == 9);
   context.reset();
   BOOST_CHECK_NO_THROW(memory.reset());
}

BOOST_AUTO_TEST_CASE(context_identity_is_not_reused_wrapper_storage_or_a_different_execution_mode) {
   auto module = std::optional<vm::prepared_module>{std::in_place, owner_module(), vm::options{},
                                                    std::vector<vm::host_binding>{owner_binding()}};
   auto context = vm::runtime_context{*module, 251, vm::execution_mode::interpreter};
   const auto* address = &*module;
   BOOST_CHECK(context.matches(*module, vm::execution_mode::interpreter));
   BOOST_CHECK(!context.matches(*module, vm::execution_mode::jit));
   module.reset();
   module.emplace(owner_module(), vm::options{}, std::vector<vm::host_binding>{owner_binding()});
   BOOST_REQUIRE(&*module == address);
   BOOST_CHECK(!context.matches(*module, vm::execution_mode::interpreter));
   auto memory = vm::wasm_allocator{};
   context.set_wasm_allocator(&memory);
   auto host = owner_host{};
   context.initialize(&host);
   context.call(&host, "apply", owner_arguments);
   BOOST_TEST(host.calls == 1U);
   memory.reset();
}

BOOST_AUTO_TEST_CASE(context_retains_bounded_preparation_callable_and_charge_until_last_owner) {
   using budget = vm::prepared_module::preparation;
   constexpr std::uint64_t ceiling = 1024 * 1024; // Same ceiling as preparation_budget_tests.cpp.
   bool released = false;
   auto account = std::optional<budget>{std::in_place, budget::limits{ceiling, ceiling}};
   const auto root = account->current_bytes();
   account->attach_resident_charge(std::make_shared<charge_lifetime>(released));
   auto pin = std::make_shared<unsigned>(0);
   const auto weak = std::weak_ptr{pin};
   auto bindings = std::vector<vm::host_binding>{owner_binding(pin)};
   pin.reset();
   auto module = std::make_unique<vm::prepared_module>(owner_module(), vm::options{}, std::span{bindings},
                                                       vm::prepared_module::purpose::execution, *account);
   BOOST_REQUIRE(!bindings[0].invoke);
   const auto retained = account->current_bytes();
   BOOST_TEST(retained > root);
   BOOST_TEST(account->seal() == retained);
   auto memory = vm::wasm_allocator{};
   auto context = std::make_unique<vm::runtime_context>(*module, 251, vm::execution_mode::interpreter);
   context->set_wasm_allocator(&memory);
   module.reset();
   BOOST_TEST(account->current_bytes() == retained);
   BOOST_CHECK(!weak.expired());
   auto host = owner_host{};
   context->initialize(&host);
   context->call(&host, "apply", owner_arguments);
   memory.reset();
   BOOST_TEST(account->current_bytes() == retained);
   BOOST_CHECK(!released);
   BOOST_CHECK(!weak.expired());
   context.reset();
   BOOST_TEST(account->current_bytes() == root);
   BOOST_CHECK(weak.expired());
   BOOST_CHECK(!released);

   account.reset();
   BOOST_CHECK(released);
   released = false;
   account.emplace(budget::limits{ceiling, ceiling});
   account->attach_resident_charge(std::make_shared<charge_lifetime>(released));
   bindings[0] = owner_binding();
   module = std::make_unique<vm::prepared_module>(owner_module(), vm::options{}, std::span{bindings},
                                                  vm::prepared_module::purpose::execution, *account);
   context = std::make_unique<vm::runtime_context>(*module, 251, vm::execution_mode::interpreter);
   module.reset();
   account.reset();
   BOOST_CHECK(!released);
   context.reset();
   BOOST_CHECK(released);
}

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
