#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

import forge.vm.wasm.interpret.backend;

namespace {

namespace wasm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;

#if defined(__x86_64__) && !defined(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
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
   append_section(code, 7, {
       6, 4, 'r', 'e', 'a', 'd', 0, 1,
       6, 'm', 'u', 't', 'a', 't', 'e', 0, 3,
       4, 't', 'r', 'a', 'p', 0, 4,
       4, 'w', 'a', 'i', 't', 0, 5,
       8, 'i', 'n', 'd', 'i', 'r', 'e', 'c', 't', 0, 6,
       3, 'o', 'o', 'b', 0, 7,
   });
   if (with_start)
      append_section(code, 8, {5});
   append_section(code, 9, {1, 0, 0x41, 0, 0x0b, 1, 2});
   auto bodies = wasm::wasm_code{7};
   for (const auto& body : {
            wasm::wasm_code{0, 0x41, 0, 0x28, 2, 0, 0x23, 0, 0x6a, 0x0b}, // read: data + global
            wasm::wasm_code{0, 0x41, 42, 0x0b},                           // table function
            wasm::wasm_code{0, 0x41, 0xe3, 0, 0x24, 0, 0x41, 0, 0x41, 0xe3, 0,
                            0x36, 2, 0, 0x41, 1, 0x40, 0, 0x1a, 0x0b}, // mutate and grow
            wasm::wasm_code{0, 0, 0x0b},                                 // unreachable
            wasm::wasm_code{0, 3, 0x40, 0x10, 0, 0x0d, 0, 0x0b, 0x0b},  // loop while host tick != 0
            wasm::wasm_code{0, 0x41, 0, 0x11, 0, 0, 0x0b},               // call_indirect table[0]
            wasm::wasm_code{0, 0x41, 0xff, 0xff, 3, 0x28, 2, 0, 0x0b},   // load across memory bound
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

struct expires_then_unmaps {
   wasm::span<std::byte> code;
   int& unmap_result;

   struct guard {
      std::function<void()> expire;
      wasm::span<std::byte> code;
      int& unmap_result;

      ~guard() noexcept {
         expire();
         unmap_result = ::munmap(code.data(), code.size());
      }
   };

   template <typename F> guard scoped_run(F&& callback) {
      return {std::forward<F>(callback), code, unmap_result};
   }
};

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
   BOOST_CHECK_THROW(backend.with_invocation(&host, {}, 2s, {}, [&] {
      backend.call(host, "env", "mutate");
      backend.call(host, "env", "trap");
   }), wasm::exceptions::interpreter);
   backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(backend, host); });
   BOOST_CHECK_THROW(backend.with_invocation(&host, {}, 2s, {}, [&] {
      backend.call(host, "env", "oob");
   }), wasm::exceptions::memory);
   backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(backend, host); });
}

BOOST_AUTO_TEST_CASE(invocation_admission_rejects_shared_owned_and_canceled_before_start) {
   auto instance = prepared_instance{true};
   auto ticks = 0;
   auto host = invocation_host{[&] { ++ticks; return 0U; }};
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
      } catch (...) { failure = std::current_exception(); }
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
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 20ms, {}, [&] {
         body = true;
         instance.backend.call(host, "env", "wait");
      }), wasm::exceptions::timeout);
      BOOST_CHECK(std::chrono::steady_clock::now() - began < 500ms);
      BOOST_CHECK(body != start);
      host.on_tick = {};
      instance.backend.with_invocation(&host, {}, 2s, {}, [&] { check_fresh(instance.backend, host); });
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_cancellation_interrupts_start_and_export_and_allows_reuse, Impl, implementations) {
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
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 3s, stop.get_token(), [&] {
         body = true;
         instance.backend.call(host, "env", "wait");
      }), wasm::exceptions::canceled);
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
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 500ms, {}, [&] {
      body = true;
      instance.backend.call(host, "env", "wait");
   }), wasm::exceptions::timeout);
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

BOOST_AUTO_TEST_CASE(invocation_host_failure_is_not_hidden_by_cancellation_or_body_catch) {
   auto instance = prepared_instance{};
   auto stop = std::stop_source{};
   auto host = invocation_host{[&]() -> std::uint32_t {
      stop.request_stop();
      throw std::runtime_error{"host failure during cancellation"};
   }};
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, stop.get_token(), [&] {
      try { instance.backend.call(host, "env", "wait"); }
      catch (const std::runtime_error&) {}
   }), std::runtime_error);
   BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
}

BOOST_AUTO_TEST_CASE(code_access_restoration_reports_real_mapping_failure) {
   const auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
   auto allocator = wasm::growable_allocator{};
   allocator.use_fixed_memory(page_size);
   auto* start = allocator.start_code();
   static_cast<void>(allocator.alloc<std::byte>(1));
   allocator.end_code<false>(start);
   allocator.disable_code();
   const auto code = allocator.get_code_span();
   BOOST_REQUIRE(::munmap(code.data(), code.size()) == 0);
   BOOST_CHECK_THROW(allocator.enable_code(false), wasm::exceptions::allocation);
   BOOST_CHECK_THROW(allocator.enable_code(true), wasm::exceptions::allocation);
   BOOST_CHECK_THROW(allocator.disable_code(), wasm::exceptions::allocation);
}

BOOST_AUTO_TEST_CASE(invocation_restore_failure_outranks_guest_and_host_errors_and_forbids_reuse) {
   auto healthy = prepared_instance{};
   auto healthy_host = invocation_host{};
   for (auto failure = 0; failure != 5; ++failure) {
      auto instance = prepared_instance{};
      auto host = invocation_host{[]() -> std::uint32_t {
         throw wasm::exceptions::interpreter{"host exception using the guest trap type"};
      }};
      auto unmap_result = -1;
      const auto code = instance.backend.get_module().allocator.get_code_span();
      BOOST_REQUIRE(!code.empty());
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [&] {
         try {
            instance.backend.timed_run(expires_then_unmaps{code, unmap_result}, [&] {
               if (failure == 1)
                  instance.backend.call(host, "env", "trap");
               if (failure == 2)
                  instance.backend.call(host, "env", "wait");
               if (failure == 3)
                  throw std::runtime_error{"native failure racing code restoration"};
            });
         } catch (const wasm::exceptions::allocation&) {
            if (failure != 4)
               throw;
         }
      }), wasm::exceptions::allocation);
      BOOST_TEST(unmap_result == 0);
      BOOST_CHECK_THROW(instance.backend.with_invocation(&host, {}, 2s, {}, [] {}), wasm::exceptions::unusable);
      BOOST_CHECK_THROW(instance.backend.timed_run(wasm::watchdog{2s}, [] {}), wasm::exceptions::unusable);
      BOOST_CHECK_THROW(instance.backend.initialize(&host), wasm::exceptions::unusable);
      auto shared = prepared_instance<>::backend_type{};
      BOOST_CHECK_THROW(shared.share(instance.backend), wasm::exceptions::unusable);

      BOOST_CHECK_THROW(healthy.backend.with_invocation(&healthy_host, {}, 2s, {}, [&] {
         healthy.backend.call(healthy_host, "env", "oob");
      }), wasm::exceptions::memory);
      healthy.backend.with_invocation(&healthy_host, {}, 2s, {}, [&] { check_fresh(healthy.backend, healthy_host); });
   }
}

BOOST_AUTO_TEST_CASE(watchdog_disable_failure_is_fail_fast_not_a_timeout_or_unbounded_wait) {
   const auto child = ::fork();
   BOOST_REQUIRE(child >= 0);
   if (child == 0) {
      // Even a broken callback cannot leave this failure regression hanging.
      ::alarm(3);
      std::set_terminate([] { ::_exit(86); });
      try {
         auto instance = prepared_instance{};
         const auto code = instance.backend.get_module().allocator.get_code_span();
         if (::munmap(code.data(), code.size()) != 0)
            ::_exit(84);
         instance.backend.timed_run(wasm::watchdog{0s}, [] {});
      } catch (...) {
         ::_exit(85);
      }
      ::_exit(87);
   }
   auto status = 0;
   auto waited = pid_t{};
   do {
      waited = ::waitpid(child, &status, 0);
   } while (waited < 0 && errno == EINTR);
   BOOST_REQUIRE(waited == child);
   BOOST_REQUIRE(WIFEXITED(status));
   BOOST_TEST(WEXITSTATUS(status) == 86);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(invocation_independent_compiled_instances_isolate_cancellation_and_deadline, Impl, implementations) {
   for (const auto cancel : {false, true}) {
      auto interrupted = prepared_instance<Impl>{};
      auto neighbor = prepared_instance<Impl>{};
      BOOST_CHECK(interrupted.backend.get_module().allocator.get_code_start() !=
                  neighbor.backend.get_module().allocator.get_code_start());
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
                        std::chrono::steady_clock::now() < neighbor_fallback ? 1U : 0U;
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
         } catch (...) { neighbor_failure = std::current_exception(); }
      }};
      auto worker = std::jthread{[&] {
         try {
            if (!neighbor_entered.try_acquire_for(2s))
               throw std::runtime_error{"test neighbor admission rendezvous expired"};
            fallback = std::chrono::steady_clock::now() + 1s;
            interrupted.backend.with_invocation(&host, {}, cancel ? 2s : 20ms, stop.get_token(), [&] {
               interrupted.backend.call(host, "env", "wait");
            });
         } catch (const wasm::exceptions::canceled&) { outcome = 1; }
         catch (const wasm::exceptions::timeout&) { outcome = 2; }
         catch (...) { outcome = 3; }
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
