#include <boost/test/unit_test.hpp>
#include <boost/mpl/list.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

import forge.vm.wasm.interpret.backend;

namespace {

namespace wasm = forge::vm::wasm::interpret;

#if defined(FORGE_VM_WASM_INTERPRET_TEST_FP_FAULTS)
extern "C" void context_exit_probe_fault(unsigned enter, unsigned restore);
extern "C" unsigned context_exit_probe_denials();
#endif

#if FORGE_VM_WASM_INTERPRET_HAS_JIT && !defined(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
using implementations = boost::mpl::list<wasm::interpreter, wasm::jit>;
#else
using implementations = boost::mpl::list<wasm::interpreter>;
#endif

template <typename Impl> struct host;
template <typename Impl> using functions = wasm::registered_host_functions<host<Impl>>;
template <typename Impl> using backend = wasm::backend<functions<Impl>, Impl>;

enum class action { api, arbitrary, replacement, foreign, first_error };
struct foreign_control_flow {};
struct first_host_error {};

template <typename Impl> struct host {
   backend<Impl>* engine = nullptr;
   action selected = action::api;
   bool nested = false;
   unsigned calls = 0;
   std::exception_ptr first_failure;
   std::stop_source* cancel_on_exit = nullptr;
#if defined(FORGE_VM_WASM_INTERPRET_TEST_FP_FAULTS)
   bool restore_on_host = false;
   bool nested_fp = false;
#endif

   void stop() {
      ++calls;
#if defined(FORGE_VM_WASM_INTERPRET_TEST_FP_FAULTS)
      if (restore_on_host) {
         restore_on_host = false;
         context_exit_probe_fault(0, 1);
      }
      if (nested_fp) {
         nested_fp = false;
         context_exit_probe_fault(1, 0);
         try {
            engine->call_with_return(*this, "env", "run");
         } catch (const wasm::exceptions::execution_unavailable&) {
            first_failure = std::current_exception();
         }
      }
#endif
      switch (selected) {
      case action::api:
         if (cancel_on_exit)
            cancel_on_exit->request_stop();
         engine->get_context().exit();
         return;
      case action::arbitrary:
         throw wasm::exceptions::exit{"arbitrary public exit"};
      case action::replacement:
         try {
            engine->get_context().exit();
         } catch (const wasm::exceptions::exit&) {
         }
         throw wasm::exceptions::exit{"replacement public exit"};
      case action::foreign:
         throw foreign_control_flow{};
      case action::first_error:
         if (nested) {
            try {
               throw first_host_error{};
            } catch (...) {
               first_failure = std::current_exception();
               throw;
            }
         }
         nested = true;
         try {
            engine->call_with_return(*this, "env", "call_exit");
         } catch (const first_host_error&) {
         }
         engine->get_context().exit();
         return;
      }
   }
};

template <typename Impl> std::vector<std::uint8_t> fixture(bool start) {
   static const bool registered = [] {
      functions<Impl>::template add<&host<Impl>::stop>("h", "stop");
      return true;
   }();
   static_cast<void>(registered);
   auto code = std::vector<std::uint8_t>{
       0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x00, 0x01, 0x7f,
       0x02, 0x0a, 0x01, 0x01, 'h',  0x04, 's',  't',  'o',  'p',  0x00, 0x00, 0x03, 0x04, 0x03, 0x00, 0x01, 0x00,
       0x05, 0x04, 0x01, 0x01, 0x01, 0x01, 0x06, 0x06, 0x01, 0x7f, 0x01, 0x41, 0x00, 0x0b, 0x07, 0x13, 0x02, 0x03,
       'r',  'u',  'n',  0x00, 0x02, 0x09, 'c',  'a',  'l',  'l',  '_',  'e',  'x',  'i',  't',  0x00, 0x03};
   if (start)
      code.insert(code.end(), {0x08, 0x01, 0x01});
   const auto bodies =
       std::vector<std::uint8_t>{0x0a, 0x16, 0x03, 0x09, 0x00, 0x41, 0x07, 0x24, 0x00, 0x10, 0x00, 0x00,
                                 0x0b, 0x04, 0x00, 0x23, 0x00, 0x0b, 0x05, 0x00, 0x10, 0x00, 0x00, 0x0b};
   code.insert(code.end(), bodies.begin(), bodies.end());
   return code;
}

template <typename Impl> bool exercise(std::string_view name, bool start, action selected, std::string_view expected) {
   auto code = fixture<Impl>(start);
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto memory = wasm::wasm_allocator{};
   const bool prepared = selected == action::first_error;
   auto engine = backend<Impl>{pointer, code.size(), nullptr, wasm::default_options{}, true, !prepared};
   if (!prepared)
      engine.set_wasm_allocator(&memory);
   auto receiver = host<Impl>{&engine, selected};
   auto observed = std::string_view{"not-entered"};
   auto run_value = std::uint32_t{UINT32_MAX};
   auto reused = false;
   auto start_once_verified = false;
   try {
      if (selected == action::first_error) {
         engine.with_invocation(&receiver, wasm::default_options{}, std::chrono::seconds{5}, {},
                                [&] { engine.call_with_return(receiver, "env", "call_exit"); });
      } else {
         engine.initialize(receiver);
         if (!start)
            engine.call_with_return(receiver, "env", "call_exit");
         observed = "returned";
         const auto result = engine.call_with_return(receiver, "env", "run");
         if (result) {
            run_value = result->to_ui32();
            reused = run_value == (start ? 7U : 0U);
         }
         if (selected == action::api) {
            start_once_verified = receiver.calls == 1;
            engine.initialize(receiver);
            const auto again = engine.call_with_return(receiver, "env", "run");
            reused = reused && again && again->to_ui32() == (start ? 7U : 0U);
            start_once_verified = start_once_verified && receiver.calls == (start ? 2U : 1U);
         }
      }
   } catch (const wasm::exceptions::exit&) {
      observed = "public-exit";
   } catch (const foreign_control_flow&) {
      observed = "foreign-control-flow";
   } catch (const first_host_error&) {
      observed = "first-host-error";
   } catch (const std::exception& error) {
      observed = "unexpected-std-exception";
      std::cout << "diagnostic=" << error.what() << '\n';
   } catch (...) {
      observed = "unexpected-exception";
   }
   const bool pass = observed == expected && (selected != action::api || (reused && start_once_verified)) &&
                     (selected != action::first_error || receiver.calls == 2);
   std::cout << (pass ? "PASS" : "FAIL") << " case=" << name << " expected=" << expected << " observed=" << observed
             << " host_calls=" << receiver.calls << " run_value=" << run_value << " reused=" << reused << '\n';
   return pass;
}

template <typename Impl> bool strict_exercise(std::string_view name, bool start, action selected) {
   auto code = fixture<Impl>(start);
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto engine = backend<Impl>{pointer, code.size(), nullptr, wasm::default_options{}, true, false};
   auto receiver = host<Impl>{&engine, selected};
   const bool normal = selected == action::api;
   const auto expected = normal                            ? "returned"
                         : selected == action::foreign     ? "foreign-control-flow"
                         : selected == action::first_error ? "first-host-error"
                                                           : "public-exit";
   auto observed = std::string_view{"not-entered"};
   auto observed_failure = std::exception_ptr{};
   unsigned bodies = 0;
   bool value_verified = false;
   bool reuse_verified = false;
   bool poison_verified = false;
   bool start_once_verified = true;
   try {
      engine.with_invocation(&receiver, wasm::default_options{}, std::chrono::seconds{5}, {}, [&] {
         ++bodies;
         start_once_verified = receiver.calls == (start ? 1U : 0U);
         if (!start) {
            try {
               engine.call_with_return(receiver, "env", "call_exit");
            } catch (...) {
               // A caught host exception must remain visible to the strict owner.
            }
         }
         if (normal) {
            const auto result = engine.call_with_return(receiver, "env", "run");
            value_verified = result && result->to_ui32() == (start ? 7U : 0U);
            start_once_verified = start_once_verified && receiver.calls == 1;
         }
      });
      observed = "returned";
   } catch (const wasm::exceptions::exit&) {
      observed = "public-exit";
      observed_failure = std::current_exception();
   } catch (const foreign_control_flow&) {
      observed = "foreign-control-flow";
      observed_failure = std::current_exception();
   } catch (const first_host_error&) {
      observed = "first-host-error";
      observed_failure = std::current_exception();
   } catch (const std::exception& error) {
      observed = "unexpected-std-exception";
      std::cout << "diagnostic=" << error.what() << '\n';
   } catch (...) {
      observed = "unexpected-exception";
   }
   try {
      engine.with_invocation(&receiver, wasm::default_options{}, std::chrono::seconds{5}, {}, [&] {
         const auto result = engine.call_with_return(receiver, "env", "run");
         reuse_verified = result && result->to_ui32() == (start ? 7U : 0U);
         start_once_verified = start_once_verified && receiver.calls == (start ? 2U : 1U);
      });
   } catch (const wasm::exceptions::unusable&) {
      poison_verified = true;
   } catch (...) {
   }
   const bool first_verified = selected != action::first_error || (receiver.calls == 2 && receiver.first_failure &&
                                                                   receiver.first_failure == observed_failure);
   const bool pass = observed == expected && bodies == (start && !normal ? 0U : 1U) && first_verified &&
                     (normal ? value_verified && reuse_verified && !poison_verified && start_once_verified
                             : poison_verified && !reuse_verified);
   std::cout << (pass ? "PASS" : "FAIL") << " case=" << name << " expected=" << expected << " observed=" << observed
             << " host_calls=" << receiver.calls << " bodies=" << bodies << " first_identity=" << first_verified
             << " value=" << value_verified << " reused=" << reuse_verified << " poisoned=" << poison_verified
             << " start_once=" << start_once_verified << '\n';
   return pass;
}

#if defined(FORGE_VM_WASM_INTERPRET_TEST_FP_FAULTS)
enum class denial { start_restore, function_enter, function_restore, nested_enter, initialize_enter, host_priority };

template <typename Impl>
bool fp_exercise(std::string_view name, denial at, bool reset, bool normal_exit_after = false, bool cancel = false) {
   const bool start = at == denial::start_restore;
   const bool host_priority = at == denial::host_priority;
   auto code = fixture<Impl>(start);
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto engine = backend<Impl>{pointer, code.size(), nullptr, wasm::default_options{}, true, false};
   auto receiver = host<Impl>{&engine, host_priority ? action::first_error : action::api};
   auto stop = std::stop_source{};
   const auto token = cancel ? stop.get_token() : std::stop_token{};
   receiver.cancel_on_exit = cancel ? &stop : nullptr;
   receiver.restore_on_host = start;
   receiver.nested_fp = at == denial::nested_enter;
   context_exit_probe_fault(0, 0);
   auto caught = std::exception_ptr{};
   auto observed = std::exception_ptr{};
   unsigned bodies = 0;
   bool recovered_inside = false;
   bool normal_exited_after = false;
   bool typed_refusal = false;
   bool first_host_error_preserved = false;
   bool poisoned = false;
   try {
      engine.with_invocation(&receiver, wasm::default_options{}, std::chrono::seconds{5}, token, [&] {
         ++bodies;
         if (at == denial::function_enter)
            context_exit_probe_fault(1, 0);
         if (at == denial::function_restore || host_priority)
            context_exit_probe_fault(0, 1);
         // One instantiate guard precedes the deferred initialize guard.
         if (at == denial::initialize_enter)
            context_exit_probe_fault(2, 0);
         try {
            if (at == denial::initialize_enter)
               engine.initialize(receiver);
            else
               engine.call_with_return(receiver, "env", "call_exit");
         } catch (const wasm::exceptions::execution_unavailable&) {
            caught = std::current_exception();
         } catch (const first_host_error&) {
            if (!host_priority)
               throw;
            caught = std::current_exception();
         }
         if (reset) {
            engine.get_context().reset();
            engine.initialize(receiver);
            const auto result = engine.call_with_return(receiver, "env", "run");
            recovered_inside = result && result->to_ui32() == 0;
         }
         if (normal_exit_after) {
            engine.call_with_return(receiver, "env", "call_exit");
            normal_exited_after = true;
         }
      });
   } catch (const wasm::exceptions::execution_unavailable&) {
      typed_refusal = true;
      observed = std::current_exception();
   } catch (const first_host_error&) {
      first_host_error_preserved = true;
      observed = std::current_exception();
   } catch (const std::exception& error) {
      std::cout << "diagnostic=" << error.what() << '\n';
   } catch (...) {
      std::cout << "diagnostic=unexpected-exception\n";
   }
   const auto calls = receiver.calls;
   try {
      engine.with_invocation(&receiver, wasm::default_options{}, std::chrono::seconds{5}, {}, [] {});
   } catch (const wasm::exceptions::unusable&) {
      poisoned = true;
   } catch (...) {
   }
   const auto expected_failure = receiver.first_failure ? receiver.first_failure : caught;
   const bool exact_failure = start ? observed != nullptr : expected_failure && observed == expected_failure;
   const bool counts = start ? bodies == 0 && calls == 1 : host_priority ? bodies == 1 && calls == 2 : bodies == 1;
   const bool pass =
       (host_priority ? first_host_error_preserved && !typed_refusal : typed_refusal && !first_host_error_preserved) &&
       exact_failure && poisoned && receiver.calls == calls && counts && context_exit_probe_denials() == 1 &&
       (!reset || recovered_inside) && (!normal_exit_after || normal_exited_after) &&
       (!cancel || stop.stop_requested());
   std::cout << (pass ? "PASS" : "FAIL") << " case=" << name << " fp_refusal=" << typed_refusal
             << " first_host_error=" << first_host_error_preserved << " exact_failure=" << exact_failure
             << " body_caught=" << bool(caught) << " reset_recovered=" << recovered_inside
             << " normal_exit_after=" << normal_exited_after << " poisoned=" << poisoned << " host_calls=" << calls
             << " bodies=" << bodies << " cancel_requested=" << stop.stop_requested()
             << " denials=" << context_exit_probe_denials() << '\n';
   return pass;
}
#endif

} // namespace

BOOST_AUTO_TEST_SUITE(context_exit)

#if !defined(FORGE_VM_WASM_INTERPRET_TEST_FP_FAULTS)

BOOST_AUTO_TEST_CASE_TEMPLATE(synchronous_cancel_before_start_exit_is_not_normal_success, Impl, implementations) {
   auto code = fixture<Impl>(true);
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto engine = backend<Impl>{pointer, code.size(), nullptr, wasm::default_options{}, true, false};
   auto receiver = host<Impl>{&engine, action::api};
   auto stop = std::stop_source{};
   receiver.cancel_on_exit = &stop;
   unsigned bodies = 0;
   BOOST_CHECK_THROW(
       engine.with_invocation(&receiver, {}, std::chrono::seconds{5}, stop.get_token(), [&] { ++bodies; }),
       wasm::exceptions::canceled);
   BOOST_TEST(stop.stop_requested());
   BOOST_TEST(receiver.calls == 1U);
   BOOST_TEST(bodies == 0U);
   BOOST_CHECK_THROW(engine.get_context(), wasm::exceptions::interpreter);
   receiver.cancel_on_exit = nullptr;
   engine.with_invocation(&receiver, {}, std::chrono::seconds{5}, {}, [&] {
      ++bodies;
      BOOST_TEST(engine.get_context().current_linear_memory() == 1U);
      const auto value = engine.call_with_return(receiver, "", "run");
      BOOST_REQUIRE(value.has_value());
      BOOST_TEST(value->to_ui32() == 7U);
   });
   BOOST_TEST(receiver.calls == 2U);
   BOOST_TEST(bodies == 1U);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(synchronous_cancel_before_function_exit_is_not_normal_success, Impl, implementations) {
   auto code = fixture<Impl>(false);
   auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
   auto engine = backend<Impl>{pointer, code.size(), nullptr, wasm::default_options{}, true, false};
   auto receiver = host<Impl>{&engine, action::api};
   auto stop = std::stop_source{};
   receiver.cancel_on_exit = &stop;
   unsigned continued = 0;
   BOOST_CHECK_THROW(engine.with_invocation(&receiver, {}, std::chrono::seconds{5}, stop.get_token(),
                                            [&] {
                                               engine.call_with_return(receiver, "", "call_exit");
                                               ++continued;
                                            }),
                     wasm::exceptions::canceled);
   BOOST_TEST(stop.stop_requested());
   BOOST_TEST(receiver.calls == 1U);
   BOOST_TEST(continued == 0U);
   BOOST_CHECK_THROW(engine.get_context(), wasm::exceptions::interpreter);
   receiver.cancel_on_exit = nullptr;
   engine.with_invocation(&receiver, {}, std::chrono::seconds{5}, {}, [&] {
      engine.call_with_return(receiver, "", "call_exit");
      BOOST_TEST(engine.get_context().current_linear_memory() == 1U);
      const auto value = engine.call_with_return(receiver, "", "run");
      BOOST_REQUIRE(value.has_value());
      BOOST_TEST(value->to_ui32() == 0U);
   });
   BOOST_TEST(receiver.calls == 2U);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(maximum_invocation_budget_keeps_cancel_exit_synchronous_and_reusable, Impl,
                              implementations) {
   constexpr unsigned repetitions = 32;
   constexpr auto budget = std::chrono::steady_clock::duration::max();
   for (const bool start : {false, true}) {
      auto code = fixture<Impl>(start);
      auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
      auto engine = backend<Impl>{pointer, code.size(), nullptr, wasm::default_options{}, true, false};
      auto receiver = host<Impl>{&engine, action::api};
      unsigned bodies = 0;
      for (unsigned iteration = 0; iteration != repetitions; ++iteration) {
         auto stop = std::stop_source{};
         receiver.cancel_on_exit = &stop;
         unsigned continued = 0;
         BOOST_CHECK_THROW(engine.with_invocation(&receiver, {}, budget, stop.get_token(),
                                                  [&] {
                                                     ++bodies;
                                                     if (!start)
                                                        engine.call_with_return(receiver, "", "call_exit");
                                                     ++continued; // C++ body, not guest instruction counting.
                                                  }),
                           wasm::exceptions::canceled);
         BOOST_TEST(stop.stop_requested());
         BOOST_TEST(receiver.calls == iteration + 1U);
         BOOST_TEST(bodies == (start ? 0U : iteration + 1U));
         BOOST_TEST(continued == 0U);
         BOOST_CHECK_THROW(engine.get_context(), wasm::exceptions::interpreter);
      }
      receiver.cancel_on_exit = nullptr;
      engine.with_invocation(&receiver, {}, budget, {}, [&] {
         if (!start)
            engine.call_with_return(receiver, "", "call_exit");
         BOOST_TEST(engine.get_context().current_linear_memory() == 1U);
         const auto value = engine.call_with_return(receiver, "", "run");
         BOOST_REQUIRE(value.has_value());
         BOOST_TEST(value->to_ui32() == (start ? 7U : 0U));
      });
      BOOST_TEST(receiver.calls == repetitions + 1U);
      BOOST_CHECK_THROW(engine.get_context(), wasm::exceptions::interpreter);
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_start_normal, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_start_normal", true, action::api, "returned"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_start_arbitrary, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_start_arbitrary", true, action::arbitrary, "public-exit"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_start_replacement, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_start_replacement", true, action::replacement, "public-exit"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_start_foreign, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_start_foreign", true, action::foreign, "foreign-control-flow"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_function_normal, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_function_normal", false, action::api, "returned"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_function_arbitrary, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_function_arbitrary", false, action::arbitrary, "public-exit"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_function_replacement, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_function_replacement", false, action::replacement, "public-exit"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(owned_function_foreign, Impl, implementations) {
   BOOST_REQUIRE(exercise<Impl>("owned_function_foreign", false, action::foreign, "foreign-control-flow"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(prepared_first_host_error_before_api_exit, Impl, implementations) {
   BOOST_REQUIRE(
       exercise<Impl>("prepared_first_host_error_before_api_exit", false, action::first_error, "first-host-error"));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_start_normal, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_start_normal", true, action::api));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_start_arbitrary, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_start_arbitrary", true, action::arbitrary));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_start_replacement, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_start_replacement", true, action::replacement));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_start_foreign, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_start_foreign", true, action::foreign));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_function_normal, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_function_normal", false, action::api));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_function_arbitrary, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_function_arbitrary", false, action::arbitrary));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_function_replacement, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_function_replacement", false, action::replacement));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_function_foreign, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_function_foreign", false, action::foreign));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(strict_first_host_error_before_api_exit, Impl, implementations) {
   BOOST_REQUIRE(strict_exercise<Impl>("strict_first_host_error_before_api_exit", false, action::first_error));
}

#else

BOOST_AUTO_TEST_CASE_TEMPLATE(start_api_exit_restore_denied, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("start_api_exit_restore_denied", denial::start_restore, false, false));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(function_api_exit_restore_caught, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("function_api_exit_restore_caught", denial::function_restore, false, false));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(function_api_exit_restore_caught_reset, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("function_api_exit_restore_caught_reset", denial::function_restore, true, false));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(function_enter_caught_reset_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("function_enter_caught_reset_exit", denial::function_enter, true, true));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(function_restore_caught_reset_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("function_restore_caught_reset_exit", denial::function_restore, true, true));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(nested_enter_caught_api_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("nested_enter_caught_api_exit", denial::nested_enter, false, false));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(initialize_enter_caught_reset_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("initialize_enter_caught_reset_exit", denial::initialize_enter, true, true));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(first_host_error_before_fp_and_api_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("first_host_error_before_fp_and_api_exit", denial::host_priority, false, false));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(start_restore_failure_before_synchronous_cancel_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("start_restore_failure_before_synchronous_cancel_exit", denial::start_restore, false,
                                   false, true));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(function_restore_failure_before_synchronous_cancel_exit, Impl, implementations) {
   BOOST_REQUIRE(fp_exercise<Impl>("function_restore_failure_before_synchronous_cancel_exit", denial::function_restore,
                                   false, false, true));
}

#endif

BOOST_AUTO_TEST_SUITE_END()
