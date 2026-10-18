#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.backend;

namespace {

namespace wasm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;

#if FORGE_VM_WASM_INTERPRET_HAS_JIT && !defined(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
using implementations = boost::mpl::list<wasm::interpreter, wasm::jit>;
#else
using implementations = boost::mpl::list<wasm::interpreter>;
#endif

struct invocation_host {
   std::function<std::uint32_t()> on_tick;

   std::uint32_t tick() {
      return on_tick ? on_tick() : 0;
   }
};

struct recorded_host_failure {
   unsigned identity;
};

struct bounded_depth_options {
   std::uint32_t max_call_depth = 1;
};

using host_functions = wasm::registered_host_functions<invocation_host>;

void append_size(wasm::wasm_code& output, std::size_t value) {
   do {
      auto byte = static_cast<std::uint8_t>(value & 0x7fU);
      value >>= 7;
      output.push_back(value == 0 ? byte : static_cast<std::uint8_t>(byte | 0x80U));
   } while (value != 0);
}

void append_section(wasm::wasm_code& code, std::uint8_t id, const wasm::wasm_code& bytes) {
   code.push_back(id);
   append_size(code, bytes.size());
   code.insert(code.end(), bytes.begin(), bytes.end());
}

wasm::wasm_code program(bool with_start) {
   auto code = wasm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   append_section(code, 1, {2, 0x60, 0, 1, 0x7f, 0x60, 0, 0});
   append_section(code, 2, {1, 3, 'e', 'n', 'v', 4, 't', 'i', 'c', 'k', 0, 0});
   append_section(code, 3, {7, 0, 0, 1, 1, 1, 0, 0});
   append_section(code, 4, {1, 0x70, 0, 1});
   append_section(code, 5, {1, 1, 1, 2});
   append_section(code, 6, {1, 0x7f, 1, 0x41, 11, 0x0b});
   append_section(code, 7,
                  {
                      6,   4,   'r', 'e', 'a', 'd', 0,   1,   6, 'm', 'u', 't', 'a', 't', 'e', 0,
                      3,   4,   't', 'r', 'a', 'p', 0,   4,   4, 'w', 'a', 'i', 't', 0,   5,   8,
                      'i', 'n', 'd', 'i', 'r', 'e', 'c', 't', 0, 6,   3,   'o', 'o', 'b', 0,   7,
                  });
   if (with_start)
      append_section(code, 8, {5});
   append_section(code, 9, {1, 0, 0x41, 0, 0x0b, 1, 2});
   auto bodies = wasm::wasm_code{7};
   for (const auto& body : {
            wasm::wasm_code{0, 0x41, 0, 0x28, 2, 0, 0x23, 0, 0x6a, 0x0b}, // read: data + global
            wasm::wasm_code{0, 0x41, 42, 0x0b},                           // table function
            wasm::wasm_code{0, 0x41, 0xe3, 0, 0x24, 0, 0x41, 0, 0x41, 0xe3,
                            0, 0x36, 2,    0, 0x41, 1, 0x40, 0, 0x1a, 0x0b}, // mutate and grow
            wasm::wasm_code{0, 0, 0x0b},                                     // unreachable
            wasm::wasm_code{0, 3, 0x40, 0x10, 0, 0x0d, 0, 0x0b, 0x0b},       // loop while host tick != 0
            wasm::wasm_code{0, 0x41, 0, 0x11, 0, 0, 0x0b},                   // call_indirect table[0]
            wasm::wasm_code{0, 0x41, 0xff, 0xff, 3, 0x28, 2, 0, 0x0b},       // load across memory bound
        }) {
      append_size(bodies, body.size());
      bodies.insert(bodies.end(), body.begin(), body.end());
   }
   append_section(code, 10, bodies);
   append_section(code, 11, {1, 0, 0x41, 0, 0x0b, 1, 7});
   return code;
}

wasm::wasm_code_ptr registered_pointer(wasm::wasm_code& code) {
   static const auto registered = [] {
      host_functions::add<&invocation_host::tick>("env", "tick");
      return true;
   }();
   static_cast<void>(registered);
   return {code.data(), code.size()};
}

template <typename Impl = wasm::interpreter> struct prepared_instance {
   using backend_type = wasm::backend<host_functions, Impl>;
   wasm::wasm_code code;
   wasm::wasm_code_ptr pointer;
   backend_type backend;

   explicit prepared_instance(bool start = false)
       : code(program(start)), pointer(registered_pointer(code)),
         backend(pointer, code.size(), nullptr, wasm::default_options{}, false, false) {}
};

template <typename Backend> void check_fresh(Backend& backend, invocation_host& host) {
   BOOST_TEST(backend.get_context().current_linear_memory() == 1);
   const auto value = backend.call_with_return(host, "env", "read");
   BOOST_REQUIRE(value.has_value());
   BOOST_TEST(value->to_ui32() == 18U);
   const auto indirect = backend.call_with_return(host, "env", "indirect");
   BOOST_REQUIRE(indirect.has_value());
   BOOST_TEST(indirect->to_ui32() == 42U);
}

} // namespace

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_resets_memory_globals_data_and_table, Impl, implementations) {
   auto instance = prepared_instance<Impl>{};
   auto host = invocation_host{};
   auto& backend = instance.backend;
   for (auto round = 0; round != 3; ++round) {
      backend.with_invocation(&host, {}, 2s, {}, [&] {
         check_fresh(backend, host);
         backend.call(host, "env", "mutate");
         BOOST_TEST(backend.get_context().current_linear_memory() == 2);
         BOOST_TEST(backend.call_with_return(host, "env", "read")->to_ui32() == 198U);
         BOOST_TEST(backend.call_with_return(host, "env", "indirect")->to_ui32() == 42U);
      });
      BOOST_CHECK_THROW(backend.call(host, "env", "read"), wasm::exceptions::interpreter);
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_guest_traps_release_and_allow_fresh_reuse, Impl, implementations) {
   auto instance = prepared_instance<Impl>{};
   auto host = invocation_host{};
   auto& backend = instance.backend;
   BOOST_CHECK_THROW(backend.with_invocation(&host, {}, 2s, {},
                                             [&] {
                                                backend.call(host, "env", "mutate");
                                                backend.call(host, "env", "trap");
                                             }),
                     wasm::exceptions::interpreter);
   backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(backend, host); });
   BOOST_CHECK_THROW(backend.with_invocation(&host, {}, 2s, {}, [&] { backend.call(host, "env", "oob"); }),
                     wasm::exceptions::memory);
   backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(backend, host); });
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_call_depth_guards_guest_and_import_frames, Impl, implementations) {
   auto code = program(false);
   auto pointer = registered_pointer(code);
   auto limits = bounded_depth_options{};
   auto backend =
       wasm::backend<host_functions, Impl, bounded_depth_options>{pointer, code.size(), nullptr, limits, false, false};
   auto ticks = 0;
   auto host = invocation_host{[&] {
      ++ticks;
      return 0U;
   }};
   BOOST_CHECK_THROW(
       backend.with_invocation(&host, limits, 2s, {}, [&] { (void)backend.call_with_return(host, "env", "indirect"); }),
       wasm::exceptions::interpreter);
   limits.max_call_depth = 2;
   backend.with_invocation(&host, limits, 2s, {}, [&] {
      const auto value = backend.call_with_return(host, "env", "indirect");
      BOOST_REQUIRE(value.has_value());
      BOOST_TEST(value->to_ui32() == 42U);
   });
   limits.max_call_depth = 1;
   BOOST_CHECK_THROW(backend.with_invocation(&host, limits, 2s, {}, [&] { backend.call(host, "env", "wait"); }),
                     wasm::exceptions::interpreter);
   BOOST_TEST(ticks == 0);
   limits.max_call_depth = 2;
   backend.with_invocation(&host, limits, 2s, {}, [&] { backend.call(host, "env", "wait"); });
   BOOST_TEST(ticks == 1);
}

BOOST_AUTO_TEST_CASE(invocation_admission_rejects_shared_owned_and_canceled_before_start) {
   auto instance = prepared_instance{true};
   auto ticks = 0;
   auto host = invocation_host{[&] {
      ++ticks;
      return 0U;
   }};
   auto body = false;
   {
      auto shared = prepared_instance<>::backend_type{};
      shared.share(instance.backend);
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [&] { body = true; }),
                        wasm::exceptions::shared_instance);
      BOOST_CHECK_THROW(shared.with_invocation(&host, {}, 2s, {}, [&] { body = true; }),
                        wasm::exceptions::shared_instance);
   }
   auto stop = std::stop_source{};
   stop.request_stop();
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, stop.get_token(), [&] { body = true; }),
                     wasm::exceptions::canceled);
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 0s, {}, [&] { body = true; }),
                     wasm::exceptions::timeout);
   BOOST_TEST(ticks == 0);
   BOOST_TEST(!body);
   instance.backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(instance.backend, host); });
   BOOST_TEST(ticks == 1);

   auto code = program(false);
   auto memory = wasm::wasm_allocator{};
   auto owned = prepared_instance<>::backend_type{code, host, &memory};
   BOOST_CHECK_THROW(owned.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::invocation_state);
   BOOST_CHECK_NO_THROW(owned.call(host, "env", "read"));
}

BOOST_AUTO_TEST_CASE(invocation_admission_rejects_overlap_reentry_and_sharing_without_queueing) {
   auto instance = prepared_instance{};
   auto host = invocation_host{};
   auto entered = std::binary_semaphore{0};
   auto release = std::binary_semaphore{0};
   auto failure = std::exception_ptr{};
   auto worker = std::jthread{[&] {
      try {
         instance.backend.with_invocation(&host, {}, 3s, {}, [&] {
            entered.release();
            if (!release.try_acquire_for(2s))
               throw std::runtime_error{"test admission rendezvous expired"};
         });
      } catch (...) {
         failure = std::current_exception();
      }
   }};
   const auto active = entered.try_acquire_for(2s);
   if (active) {
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::busy);
      auto shared = prepared_instance<>::backend_type{};
      BOOST_CHECK_THROW(shared.share(instance.backend), wasm::exceptions::busy);
   }
   release.release();
   worker.join();
   BOOST_REQUIRE(active);
   BOOST_CHECK(!failure);
   instance.backend.with_invocation(&host, {}, 2s, {}, [&] {
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::busy);
      check_fresh(instance.backend, host);
   });
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_deadline_interrupts_start_and_export_and_allows_reuse, Impl, implementations) {
   for (const auto start : {false, true}) {
      auto instance = prepared_instance<Impl>{start};
      const auto fallback = std::chrono::steady_clock::now() + 1s;
      auto host = invocation_host{[&] { return std::chrono::steady_clock::now() < fallback ? 1U : 0U; }};
      const auto began = std::chrono::steady_clock::now();
      auto body = false;
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 20ms, {},
                                                         [&] {
                                                            body = true;
                                                            instance.backend.call(host, "env", "wait");
                                                         }),
                        wasm::exceptions::timeout);
      BOOST_CHECK(std::chrono::steady_clock::now() - began < 500ms);
      BOOST_CHECK(body != start);
      host.on_tick = {};
      instance.backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(instance.backend, host); });
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_cancellation_interrupts_start_and_export_and_allows_reuse, Impl,
                              implementations) {
   for (const auto start : {false, true}) {
      auto instance = prepared_instance<Impl>{start};
      auto stop = std::stop_source{};
      const auto fallback = std::chrono::steady_clock::now() + 2s;
      auto host = invocation_host{[&] {
         stop.request_stop();
         return std::chrono::steady_clock::now() < fallback ? 1U : 0U;
      }};
      const auto began = std::chrono::steady_clock::now();
      auto body = false;
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 3s, stop.get_token(),
                                                         [&] {
                                                            body = true;
                                                            instance.backend.call(host, "env", "wait");
                                                         }),
                        wasm::exceptions::canceled);
      BOOST_CHECK(std::chrono::steady_clock::now() - began < 1s);
      BOOST_CHECK(body != start);
      host.on_tick = {};
      instance.backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(instance.backend, host); });
   }
}

BOOST_AUTO_TEST_CASE(invocation_uses_one_budget_for_start_and_body) {
   auto instance = prepared_instance{true};
   auto ticks = 0;
   auto host = invocation_host{[&] {
      ++ticks;
      std::this_thread::sleep_for(300ms);
      return 0U;
   }};
   auto body = false;
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 500ms, {},
                                                      [&] {
                                                         body = true;
                                                         instance.backend.call(host, "env", "wait");
                                                      }),
                     wasm::exceptions::timeout);
   BOOST_TEST(body);
   BOOST_TEST(ticks == 2);
}

BOOST_AUTO_TEST_CASE(invocation_host_and_native_failures_preserve_types_and_poison_instance) {
   for (auto kind = 0; kind != 3; ++kind) {
      auto instance = prepared_instance{};
      auto host = invocation_host{[&]() -> std::uint32_t {
         if (kind == 0)
            throw wasm::exceptions::interpreter{"host exception using the guest trap type"};
         throw std::runtime_error{"host failure"};
      }};
      auto body = [&] {
         if (kind == 2)
            throw std::runtime_error{"native body failure"};
         instance.backend.call(host, "env", "wait");
      };
      if (kind == 0) {
         BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, body), wasm::exceptions::interpreter);
      } else {
         BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, body), std::runtime_error);
      }
      BOOST_CHECK_THROW(instance.backend.call(host, "env", "read"), wasm::exceptions::interpreter);
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
      auto shared = prepared_instance<>::backend_type{};
      BOOST_CHECK_THROW(shared.share(instance.backend), wasm::exceptions::unusable);
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_body_vm_error_siblings_remain_reusable, Impl, implementations) {
   auto verify = []<typename Error> {
      static_assert(!std::is_base_of_v<wasm::exceptions::interpreter, Error>);
      auto instance = prepared_instance<Impl>{};
      auto host = invocation_host{};
      const auto expected = Error{"reusable VM error from native body"};
      BOOST_CHECK_EXCEPTION(
          instance.backend.with_invocation(&host, {}, 2s, {},
                                           [&] {
                                              instance.backend.call(host, "env", "mutate");
                                              BOOST_TEST(instance.backend.get_context().current_linear_memory() == 2U);
                                              throw expected;
                                           }),
          Error,
          [&](const Error& error) { return error.code() == expected.code() && error.message() == expected.message(); });
      BOOST_CHECK_NO_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [&] {
         check_fresh(instance.backend, host);
         instance.backend.call(host, "env", "wait");
      }));
   };
   verify.template operator()<wasm::exceptions::stack_memory>();
   verify.template operator()<wasm::exceptions::pointer_out_of_bounds>();
   verify.template operator()<wasm::exceptions::invalid_element>();
   verify.template operator()<wasm::exceptions::exit>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_host_vm_error_siblings_keep_first_failure_and_poison, Impl, implementations) {
   auto verify = []<typename Error> {
      auto instance = prepared_instance<Impl>{};
      auto stop = std::stop_source{};
      auto calls = 0U;
      const auto expected = Error{"first VM error from host callback"};
      auto host = invocation_host{[&]() -> std::uint32_t {
         ++calls;
         stop.request_stop();
         throw expected;
      }};
      auto caught = false;
      BOOST_CHECK_EXCEPTION(instance.backend.with_invocation(&host, {}, 2s, stop.get_token(),
                                                             [&] {
                                                                try {
                                                                   instance.backend.call(host, "env", "wait");
                                                                } catch (const Error&) {
                                                                   caught = true;
                                                                }
                                                                instance.backend.get_context().reset();
                                                                throw std::logic_error{"later native body failure"};
                                                             }),
                            Error, [&](const Error& error) {
                               return error.code() == expected.code() && error.message() == expected.message();
                            });
      BOOST_TEST(caught);
      BOOST_TEST(calls == 1U);
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
      auto shared = typename prepared_instance<Impl>::backend_type{};
      BOOST_CHECK_THROW(shared.share(instance.backend), wasm::exceptions::unusable);
   };
   verify.template operator()<wasm::exceptions::stack_memory>();
   verify.template operator()<wasm::exceptions::pointer_out_of_bounds>();
   verify.template operator()<wasm::exceptions::invalid_element>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_unlisted_native_errors_still_poison, Impl, implementations) {
   auto verify = []<typename Error> {
      auto instance = prepared_instance<Impl>{};
      auto host = invocation_host{};
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {},
                                                         [&] {
                                                            instance.backend.call(host, "env", "mutate");
                                                            throw Error{"unlisted native body failure"};
                                                         }),
                        Error);
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
      auto shared = typename prepared_instance<Impl>::backend_type{};
      BOOST_CHECK_THROW(shared.share(instance.backend), wasm::exceptions::unusable);
   };
   verify.template operator()<std::runtime_error>();
   verify.template operator()<wasm::exceptions::allocation>();
}

BOOST_AUTO_TEST_CASE(invocation_host_failure_is_not_hidden_by_cancellation_or_body_catch) {
   auto instance = prepared_instance{};
   auto stop = std::stop_source{};
   auto host = invocation_host{[&]() -> std::uint32_t {
      stop.request_stop();
      throw std::runtime_error{"host failure during cancellation"};
   }};
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, stop.get_token(),
                                                      [&] {
                                                         try {
                                                            instance.backend.call(host, "env", "wait");
                                                         } catch (const std::runtime_error&) {
                                                         }
                                                      }),
                     std::runtime_error);
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_first_host_failure_survives_raw_context_reset_clear_and_reinitialize, Impl,
                              implementations) {
   for (unsigned mutation = 0; mutation != 3; ++mutation) {
      auto instance = prepared_instance<Impl>{};
      auto first_calls = 0U;
      auto second_calls = 0U;
      auto first = invocation_host{[&]() -> std::uint32_t {
         ++first_calls;
         throw recorded_host_failure{17};
      }};
      auto second = invocation_host{[&]() -> std::uint32_t {
         ++second_calls;
         throw recorded_host_failure{23};
      }};
      auto body_returned = false;
      BOOST_CHECK_EXCEPTION(instance.backend.with_invocation(
                                &first, {}, 2s, {},
                                [&] {
                                   auto& context = instance.backend.get_context();
                                   const auto index = instance.backend.get_module().get_exported_function("wait");
                                   BOOST_CHECK_EXCEPTION(context.call(&first, index, {}), recorded_host_failure,
                                                         [](const auto& error) { return error.identity == 17; });
                                   if (mutation != 0)
                                      context.clear_host_failure();
                                   context.reset();
                                   BOOST_TEST(!context.initialized());
                                   BOOST_CHECK(!context.host_failure());
                                   if (mutation == 2) {
                                      context.initialize(&second);
                                      BOOST_CHECK_EXCEPTION(context.call(&second, index, {}), recorded_host_failure,
                                                            [](const auto& error) { return error.identity == 23; });
                                      context.clear_host_failure();
                                      context.reset();
                                      second.on_tick = [&] {
                                         ++second_calls;
                                         return 0U;
                                      };
                                      context.initialize(&second);
                                      check_fresh(instance.backend, second);
                                      instance.backend.call(second, "env", "wait");
                                   }
                                   body_returned = true;
                                }),
                            recorded_host_failure, [](const auto& error) { return error.identity == 17; });
      BOOST_TEST(body_returned);
      BOOST_TEST(first_calls == 1U);
      BOOST_TEST(second_calls == (mutation == 2 ? 2U : 0U));
      BOOST_CHECK_THROW(instance.backend.with_invocation(&first, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
      auto shared = typename prepared_instance<Impl>::backend_type{};
      BOOST_CHECK_THROW(shared.share(instance.backend), wasm::exceptions::unusable);
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_failed_start_keeps_original_host_failure_and_strict_poison, Impl,
                              implementations) {
   auto instance = prepared_instance<Impl>{true};
   auto body_entered = false;
   auto host = invocation_host{[]() -> std::uint32_t { throw recorded_host_failure{31}; }};
   BOOST_CHECK_EXCEPTION(instance.backend.with_invocation(&host, {}, 2s, {}, [&] { body_entered = true; }),
                         recorded_host_failure, [](const auto& error) { return error.identity == 31; });
   BOOST_TEST(!body_entered);
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(legacy_timed_host_failure_reinitializes_fresh_cached_instance_after_scope_close, Impl,
                              implementations) {
   for (const auto start : {false, true}) {
      for (const auto typed : {false, true}) {
         auto instance = prepared_instance<Impl>{start};
         auto& backend = instance.backend;
         auto& module = backend.get_module();
         auto context = typename Impl::template context<host_functions>{module, 251};
         auto memory = wasm::wasm_allocator{};
         backend.set_context(&context);
         backend.set_wasm_allocator(&memory);
         auto first_calls = 0U;
         auto first = invocation_host{[&]() -> std::uint32_t {
            ++first_calls;
            throw recorded_host_failure{41};
         }};
         auto run = [&](auto&& body) {
            if (typed)
               backend.timed_run(std::chrono::steady_clock::now() + 2s, {}, std::forward<decltype(body)>(body));
            else
               backend.timed_run(wasm::watchdog{2s}, std::forward<decltype(body)>(body));
         };
         BOOST_CHECK_EXCEPTION(run([&] {
                                  backend.initialize(&first);
                                  backend.call(first, "env", "mutate");
                                  backend.call(first, "env", "wait");
                               }),
                               recorded_host_failure, [](const auto& error) { return error.identity == 41; });
         BOOST_TEST(first_calls == 1U);
         const auto closed_failure = context.host_failure();
         BOOST_REQUIRE(closed_failure);
         auto stop = std::stop_source{};
         stop.request_stop();
         auto entered = false;
         BOOST_CHECK_THROW(
             backend.timed_run(std::chrono::steady_clock::now() + 2s, {stop.get_token()}, [&] { entered = true; }),
             wasm::exceptions::canceled);
         BOOST_TEST(!entered);
         BOOST_CHECK(context.host_failure() == closed_failure);
         BOOST_CHECK_NO_THROW(run([&] { entered = true; }));
         BOOST_TEST(entered);
         entered = false;
         BOOST_CHECK_THROW(backend.timed_run(std::chrono::steady_clock::now() - 1ms, {}, [&] { entered = true; }),
                           wasm::exceptions::timeout);
         BOOST_TEST(!entered);
         BOOST_CHECK(context.host_failure() == closed_failure);
         BOOST_TEST(first_calls == 1U);
         context.reset();
         BOOST_TEST(!context.initialized());
         BOOST_CHECK(context.linear_memory() == nullptr);
         BOOST_CHECK(!context.host_failure());
         auto next_calls = 0U;
         auto next = invocation_host{[&] {
            ++next_calls;
            return 0U;
         }};
         for (auto round = 0; round != 2; ++round) {
            BOOST_CHECK_NO_THROW(run([&] {
               // The second round retires a live, mutated instance through
               // initialize itself, without an intervening allocator reset.
               backend.initialize(&next);
               BOOST_CHECK(&backend.get_module() == &module);
               BOOST_CHECK(&backend.get_context() == &context);
               BOOST_CHECK(!context.host_failure());
               check_fresh(backend, next);
               backend.call(next, "env", "wait");
               backend.call(next, "env", "mutate");
               BOOST_TEST(context.current_linear_memory() == 2U);
               BOOST_TEST(backend.call_with_return(next, "env", "read")->to_ui32() == 198U);
            }));
         }
         BOOST_TEST(next_calls == (start ? 4U : 2U));
         BOOST_TEST(first_calls == 1U);
         context.reset();
         backend.set_context(nullptr);
      }
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(legacy_nested_timed_scopes_keep_first_failure_and_release_lifo, Impl, implementations) {
   auto instance = prepared_instance<Impl>{};
   auto& backend = instance.backend;
   auto context = typename Impl::template context<host_functions>{backend.get_module(), 251};
   auto memory = wasm::wasm_allocator{};
   backend.set_context(&context);
   backend.set_wasm_allocator(&memory);
   auto identity = 51U;
   auto host = invocation_host{[&]() -> std::uint32_t { throw recorded_host_failure{identity}; }};
   auto inner_returned = false;
   auto outer_returned = false;
   BOOST_CHECK_EXCEPTION(
       backend.timed_run(wasm::watchdog{2s},
                         [&] {
                            backend.initialize(&host);
                            BOOST_CHECK_EXCEPTION(
                                backend.timed_run(
                                    wasm::watchdog{2s},
                                    [&] {
                                       const auto index = backend.get_module().get_exported_function("wait");
                                       BOOST_CHECK_EXCEPTION(context.call(&host, index, {}), recorded_host_failure,
                                                             [](const auto& error) { return error.identity == 51; });
                                       context.clear_host_failure();
                                       context.reset();
                                       identity = 52;
                                       context.initialize(&host);
                                       BOOST_CHECK_EXCEPTION(context.call(&host, index, {}), recorded_host_failure,
                                                             [](const auto& error) { return error.identity == 52; });
                                       context.reset();
                                       inner_returned = true;
                                    }),
                                recorded_host_failure, [](const auto& error) { return error.identity == 51; });
                            context.reset();
                            outer_returned = true;
                         }),
       recorded_host_failure, [](const auto& error) { return error.identity == 51; });
   BOOST_TEST(inner_returned);
   BOOST_TEST(outer_returned);
   host.on_tick = [] { return 0U; };
   BOOST_CHECK_NO_THROW(backend.timed_run(wasm::watchdog{2s}, [&] {
      backend.initialize(&host);
      check_fresh(backend, host);
      backend.call(host, "env", "wait");
   }));
   context.reset();
   backend.set_context(nullptr);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_independent_compiled_instances_isolate_cancellation_and_deadline, Impl,
                              implementations) {
   for (const auto cancel : {false, true}) {
      auto interrupted = prepared_instance<Impl>{};
      auto neighbor = prepared_instance<Impl>{};
      BOOST_CHECK(&interrupted.backend.get_module() != &neighbor.backend.get_module());
      auto neighbor_entered = std::binary_semaphore{0};
      auto entered = std::binary_semaphore{0};
      auto interrupted_done = std::atomic<bool>{false};
      auto stop = std::stop_source{};
      auto outcome = 0;
      auto neighbor_value = 0U;
      auto neighbor_failure = std::exception_ptr{};
      auto fallback = std::chrono::steady_clock::time_point{};
      auto first_tick = true;
      auto host = invocation_host{[&] {
         if (std::exchange(first_tick, false))
            entered.release();
         return std::chrono::steady_clock::now() < fallback ? 1U : 0U;
      }};
      auto neighbor_fallback = std::chrono::steady_clock::time_point{};
      auto neighbor_first_tick = true;
      auto neighbor_host = invocation_host{[&] {
         if (std::exchange(neighbor_first_tick, false))
            neighbor_entered.release();
         return !interrupted_done.load(std::memory_order_acquire) &&
                        std::chrono::steady_clock::now() < neighbor_fallback
                    ? 1U
                    : 0U;
      }};
      auto sibling = std::jthread{[&] {
         try {
            neighbor.backend.with_invocation(&neighbor_host, {}, 3s, {}, [&] {
               neighbor_fallback = std::chrono::steady_clock::now() + 2s;
               neighbor.backend.call(neighbor_host, "env", "wait");
               if (!interrupted_done.load(std::memory_order_acquire))
                  throw std::runtime_error{"test isolation rendezvous expired"};
               neighbor_value = neighbor.backend.call_with_return(neighbor_host, "env", "read")->to_ui32();
            });
         } catch (...) {
            neighbor_failure = std::current_exception();
         }
      }};
      auto worker = std::jthread{[&] {
         try {
            if (!neighbor_entered.try_acquire_for(2s))
               throw std::runtime_error{"test neighbor admission rendezvous expired"};
            fallback = std::chrono::steady_clock::now() + 1s;
            interrupted.backend.with_invocation(&host, {}, cancel ? 2s : 20ms, stop.get_token(),
                                                [&] { interrupted.backend.call(host, "env", "wait"); });
         } catch (const wasm::exceptions::canceled&) {
            outcome = 1;
         } catch (const wasm::exceptions::timeout&) {
            outcome = 2;
         } catch (...) {
            outcome = 3;
         }
         interrupted_done.store(true, std::memory_order_release);
      }};
      const auto active = entered.try_acquire_for(2s);
      if (cancel)
         stop.request_stop();
      worker.join();
      sibling.join();
      BOOST_REQUIRE(active);
      BOOST_TEST(outcome == (cancel ? 1 : 2));
      BOOST_CHECK(!neighbor_failure);
      BOOST_TEST(neighbor_value == 18U);
   }
}

BOOST_AUTO_TEST_CASE(watchdog_stop_deadline_and_completion_arbitrate_once_and_join) {
   auto calls = std::atomic<unsigned>{0};
   auto finished = std::atomic<bool>{false};
   auto stop = std::stop_source{};
   auto reason = wasm::watchdog::interruption::deadline;
   stop.request_stop();
   {
      auto guard = wasm::watchdog{2s, stop.get_token()}.scoped_run([&](wasm::watchdog::interruption value) {
         calls.fetch_add(1);
         reason = value;
         std::this_thread::sleep_for(5ms);
         finished.store(true);
      });
   }
   BOOST_TEST(calls.load() == 1U);
   BOOST_TEST(finished.load());
   BOOST_CHECK(reason == wasm::watchdog::interruption::canceled);

   for (auto round = 0; round != 16; ++round) {
      calls.store(0);
      auto racing_stop = std::stop_source{};
      auto requester = std::jthread{[&] {
         std::this_thread::sleep_for(1ms);
         racing_stop.request_stop();
      }};
      {
         auto guard = wasm::watchdog{1ms, racing_stop.get_token()}.scoped_run([&] { calls.fetch_add(1); });
         std::this_thread::sleep_for(2ms);
      }
      requester.join();
      BOOST_TEST(calls.load() == 1U);
   }

   calls.store(0);
   auto completed_stop = std::stop_source{};
   {
      auto guard = wasm::watchdog{2s, completed_stop.get_token()}.scoped_run([&] { calls.fetch_add(1); });
   }
   completed_stop.request_stop();
   BOOST_TEST(calls.load() == 0U);
}
