#include <boost/test/unit_test.hpp>
#include "fixtures.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if FORGE_VM_WASM_INTERPRET_HAS_JIT
#include <csignal>
#include <pthread.h>
#include <stdexcept>
#include <sys/time.h>
#include <unwind.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

import forge.vm.wasm.interpret.backend;

namespace {
namespace vm = forge::vm::wasm::interpret;
using profiled_backend = vm::backend<std::nullptr_t, vm::jit_profile, vm::default_options, vm::profile_instr_map>;

struct profile_fixture {
   fixtures::bytes wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   std::array<std::vector<std::uint32_t>, 2> opcodes;
   std::uint32_t code_section = 0;

   explicit profile_fixture(bool with_cancel_hook = false) {
      fixtures::bytes padding;
      fixtures::text(padding, "padding");
      padding.resize(300, 0);
      fixtures::section(wasm, 0, padding);
      fixtures::section(wasm, 1,
                        with_cancel_hook ? fixtures::bytes{2, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 0, 0}
                                         : fixtures::bytes{1, 0x60, 1, 0x7f, 1, 0x7f});
      if (with_cancel_hook)
         fixtures::section(wasm, 2, {1, 3, 'e', 'n', 'v', 7, 'e', 'n', 't', 'e', 'r', 'e', 'd', 0, 1});
      fixtures::section(wasm, 3, with_cancel_hook ? fixtures::bytes{3, 0, 0, 1} : fixtures::bytes{2, 0, 0});
      const auto first_function = static_cast<std::uint8_t>(with_cancel_hook);
      fixtures::bytes exports{static_cast<std::uint8_t>(with_cancel_hook ? 3 : 2)};
      fixtures::text(exports, "mix");
      exports.insert(exports.end(), {0, first_function});
      fixtures::text(exports, "other");
      exports.insert(exports.end(), {0, static_cast<std::uint8_t>(first_function + 1)});
      if (with_cancel_hook) {
         fixtures::text(exports, "cancel");
         exports.insert(exports.end(), {0, 3});
      }
      fixtures::section(wasm, 7, exports);
      fixtures::bytes code{static_cast<std::uint8_t>(with_cancel_hook ? 3 : 2)};
      for (std::uint32_t function = 0; function != 2; ++function) {
         fixtures::bytes body{1, 1, 0x7f};
         const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
            opcodes[function].push_back(static_cast<std::uint32_t>(body.size()));
            body.insert(body.end(), bytes.begin(), bytes.end());
         };
         emit({0x41, 1});
         emit({0x21, 1});
         emit({0x03, 0x40});
         emit({0x20, 1});
         emit({0x41, static_cast<std::uint8_t>(function ? 5 : 3)});
         emit({0x6c});
         emit({0x41, static_cast<std::uint8_t>(function ? 11 : 7)});
         emit({0x73});
         emit({0x20, 0});
         emit({0x6e});
         emit({0x21, 1});
         // The second quotient depends on the first and feeds the next iteration.
         emit({0x20, 0});
         emit({0x20, 1});
         emit({0x41, 1});
         emit({0x72});
         emit({0x6e});
         emit({0x21, 1});
         emit({0x20, 0});
         emit({0x41, 1});
         emit({0x6b});
         emit({0x22, 0});
         emit({0x0d, 0});
         emit({0x0b});
         emit({0x20, 1});
         emit({0x0b});
         fixtures::size(code, body.size());
         for (auto& offset : opcodes[function])
            offset += static_cast<std::uint32_t>(code.size());
         code.insert(code.end(), body.begin(), body.end());
      }
      if (with_cancel_hook) {
         // Guest entry is observable before cancellation; the loop cannot win
         // a race by completing before the installed stop callback runs.
         const fixtures::bytes cancel{0, 0x10, 0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b};
         fixtures::size(code, cancel.size());
         code.insert(code.end(), cancel.begin(), cancel.end());
      }
      wasm.push_back(10);
      fixtures::size(wasm, code.size());
      code_section = static_cast<std::uint32_t>(wasm.size());
      for (auto& function : opcodes)
         for (auto& offset : function)
            offset += code_section;
      wasm.insert(wasm.end(), code.begin(), code.end());
   }

   static std::uint32_t expected(std::uint32_t count, bool other = false) {
      std::uint32_t result = 1;
      do {
         result = ((result * (other ? 5U : 3U)) ^ (other ? 11U : 7U)) / count;
         result = count / (result | 1U);
      } while (--count);
      return result;
   }
};

#if FORGE_VM_WASM_INTERPRET_HAS_JIT
struct sample {
   std::uintptr_t pc;
   std::uint32_t offset;
   std::uint32_t foreign;
   std::uint32_t pinned;
};
struct sample_buffer {
   std::array<sample, 8192> values{};
   volatile std::sig_atomic_t count = 0;
   std::uintptr_t pinned_pc = 0;
};
thread_local sample_buffer* active_samples = nullptr;
thread_local const vm::profile_instr_map* active_map = nullptr;
thread_local const vm::profile_instr_map* foreign_map = nullptr;

void sample_handler(int, siginfo_t*, void* raw) {
   if (!active_samples)
      return;
   const auto* context = static_cast<const ucontext_t*>(raw);
   std::uintptr_t pc;
#if defined(__APPLE__) && defined(__aarch64__)
   pc = context->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
   pc = context->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
   pc = context->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
   pc = context->uc_mcontext.gregs[REG_RIP];
#else
#error Unsupported native sampling fixture platform
#endif
   const auto index = active_samples->count;
   if (index < 8192) {
      active_samples->values[index] = {pc, active_map->translate(reinterpret_cast<const void*>(pc)),
                                       foreign_map ? foreign_map->translate(reinterpret_cast<const void*>(pc))
                                                   : UINT32_MAX,
                                       active_map->translate(reinterpret_cast<const void*>(active_samples->pinned_pc))};
      active_samples->count = index + 1;
   }
}

// Test-only, exclusive subprocess timer. No production sampler is introduced.
class sampling_scope {
 public:
   sampling_scope(sample_buffer& samples, const vm::profile_instr_map& map,
                  const vm::profile_instr_map* foreign = nullptr) {
      if (active_samples || getitimer(ITIMER_PROF, &_prior_timer) != 0 || _prior_timer.it_value.tv_sec ||
          _prior_timer.it_value.tv_usec)
         throw std::runtime_error("Public profile test requires an exclusive sampling timer");
      struct sigaction action{};
      action.sa_sigaction = sample_handler;
      action.sa_flags = SA_SIGINFO;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGPROF, &action, &_prior_action) != 0)
         throw std::runtime_error("Cannot install public profile sampling handler");
      active_samples = &samples;
      active_map = &map;
      foreign_map = foreign;
      struct itimerval timer{};
      timer.it_interval.tv_usec = 1000;
      timer.it_value = timer.it_interval;
      if (setitimer(ITIMER_PROF, &timer, nullptr) != 0) {
         active_samples = nullptr;
         active_map = nullptr;
         foreign_map = nullptr;
         sigaction(SIGPROF, &_prior_action, nullptr);
         throw std::runtime_error("Cannot arm public profile sampling timer");
      }
   }
   ~sampling_scope() {
      sigset_t blocked{}, prior{};
      sigemptyset(&blocked);
      sigaddset(&blocked, SIGPROF);
      sigprocmask(SIG_BLOCK, &blocked, &prior);
      struct itimerval disabled{};
      setitimer(ITIMER_PROF, &disabled, nullptr);
      sigset_t pending{};
      sigpending(&pending);
      if (sigismember(&pending, SIGPROF)) {
         int consumed{};
         sigwait(&blocked, &consumed);
      }
      active_samples = nullptr;
      active_map = nullptr;
      foreign_map = nullptr;
      sigaction(SIGPROF, &_prior_action, nullptr);
      setitimer(ITIMER_PROF, &_prior_timer, nullptr);
      sigprocmask(SIG_SETMASK, &prior, nullptr);
   }
   sampling_scope(const sampling_scope&) = delete;
   sampling_scope& operator=(const sampling_scope&) = delete;

 private:
   struct sigaction _prior_action{};
   struct itimerval _prior_timer{};
};

template <typename Invoke>
sample_buffer collect(const vm::profile_instr_map& map, const vm::profile_instr_map* foreign, Invoke&& invoke) {
   sample_buffer result;
   {
      const sampling_scope sampling{result, map, foreign};
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
      do {
         invoke();
      } while (std::chrono::steady_clock::now() < until);
   }
   return result;
}

sample verify_samples(const sample_buffer& samples, const profile_fixture& fixture, std::size_t function) {
   std::set<std::uint32_t> offsets;
   std::size_t mapped_samples = 0;
   sample known{};
   for (std::sig_atomic_t i = 0; i < samples.count; ++i) {
      const auto value = samples.values[i];
      if (value.offset == UINT32_MAX)
         continue;
      BOOST_TEST(value.foreign == UINT32_MAX);
      BOOST_CHECK(std::ranges::find(fixture.opcodes[function], value.offset) != fixture.opcodes[function].end());
      BOOST_TEST(value.offset > fixture.code_section);
      offsets.insert(value.offset);
      ++mapped_samples;
      known = value;
   }
   std::ostringstream diagnostic;
   if (offsets.size() < 2U) {
      // Build failure evidence after sampling has stopped. Keep different
      // attributions of the same native PC distinct in the diagnostic.
      std::map<std::pair<std::uintptr_t, std::uint32_t>, std::size_t> counts;
      for (std::sig_atomic_t i = 0; i < samples.count; ++i)
         ++counts[{samples.values[i].pc, samples.values[i].offset}];
      for (const auto& [address, count] : counts)
         diagnostic << "\n pc=" << reinterpret_cast<const void*>(address.first) << " offset=" << address.second
                    << " samples=" << count;
   }
   BOOST_TEST_CONTEXT("function=" << function << " total_samples=" << samples.count << " mapped_samples="
                                  << mapped_samples << " distinct_offsets=" << offsets.size() << diagnostic.str()) {
      BOOST_TEST(samples.count > 0);
      BOOST_REQUIRE_GE(offsets.size(), 2U);
   }
   return known;
}
#endif
} // namespace

BOOST_AUTO_TEST_CASE(profile_public_legacy_builder_moves_without_dangling_observers) {
   std::array<std::uint8_t, 32> code{};
   std::array<std::uint8_t, 16> wasm{};
   vm::profile_instr_map::builder builder;
   builder.on_code_start(code.data(), wasm.data());
   builder.on_instr_start(code.data() + 4, wasm.data() + 3);
   builder.on_code_end(code.data() + 16, wasm.data() + 16);
   vm::profile_instr_map source;
   source.set(std::move(builder));
   auto moved = std::move(source);
   BOOST_TEST(source.translate(code.data() + 4) == UINT32_MAX);
   BOOST_TEST(moved.translate(code.data() + 4) == 3U);
   source = std::move(moved);
   BOOST_TEST(moved.translate(code.data() + 4) == UINT32_MAX);
   BOOST_TEST(source.translate(code.data() + 15) == 3U);
   BOOST_TEST(source.translate(code.data() + 16) == UINT32_MAX);
}

#if FORGE_VM_WASM_INTERPRET_HAS_JIT
BOOST_AUTO_TEST_CASE(profile_public_unwinds_live_guest_frames_and_reuses_registered_code) {
   struct unwind_host {
      const vm::profile_instr_map* map = nullptr;
      unsigned frames = 0;
      unsigned guest_frames = 0;

      std::uint32_t capture() {
         frames = guest_frames = 0;
         _Unwind_Backtrace(
             [](_Unwind_Context* context, void* argument) {
                auto& self = *static_cast<unwind_host*>(argument);
                ++self.frames;
                const auto pc = _Unwind_GetIP(context);
                if (pc && self.map->translate(reinterpret_cast<const void*>(pc - 1)) != UINT32_MAX)
                   ++self.guest_frames;
                return _URC_NO_REASON;
             },
             this);
         return 41;
      }
   };
   using functions = vm::registered_host_functions<unwind_host>;
   using unwind_backend = vm::backend<functions, vm::jit_profile, vm::default_options, vm::profile_instr_map>;
   functions::add<&unwind_host::capture>("env", "capture");
   fixtures::bytes wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(wasm, 1, {1, 0x60, 0, 1, 0x7f});
   fixtures::section(wasm, 2, {1, 3, 'e', 'n', 'v', 7, 'c', 'a', 'p', 't', 'u', 'r', 'e', 0, 0});
   fixtures::section(wasm, 3, {1, 0});
   fixtures::section(wasm, 7, {1, 3, 'r', 'u', 'n', 0, 1});
   // The addition after the import prevents replacing the guest frame with a tail call.
   fixtures::section(wasm, 10, {1, 7, 0, 0x10, 0, 0x41, 1, 0x6a, 0x0b});
   for (unsigned iteration = 0; iteration != 4; ++iteration) {
      auto host = unwind_host{};
      auto backend = unwind_backend{wasm, host, nullptr};
      auto map = backend.get_module().get_debug();
      host.map = &map;
      for (unsigned call = 0; call != 2; ++call) {
         BOOST_TEST(backend.call_with_return(host, "env", "run")->to_ui32() == 42U);
         BOOST_TEST(host.frames > 0U);
         BOOST_TEST(host.guest_frames > 0U);
      }
   }
}

BOOST_AUTO_TEST_CASE(profile_public_samples_two_functions_and_independent_objects_without_flooring_gaps) {
   profile_fixture fixture;
   const auto original = fixture.wasm;
   auto first = std::make_unique<profiled_backend>(fixture.wasm, nullptr);
   auto second = std::make_unique<profiled_backend>(fixture.wasm, nullptr);
   const auto& first_map = first->get_debug();
   const auto& second_map = second->get_debug();
   BOOST_TEST(first->call_with_return("env", "mix", 130U)->to_ui32() == profile_fixture::expected(130));
   BOOST_TEST(first->call_with_return("env", "other", 130U)->to_ui32() == profile_fixture::expected(130, true));
   const auto mix = collect(first_map, &second_map, [&] { first->call("env", "mix", 4000000U); });
   const auto known = verify_samples(mix, fixture, 0);
   const auto other = collect(first_map, &second_map, [&] { first->call("env", "other", 4000000U); });
   verify_samples(other, fixture, 1);
   const auto independent = collect(second_map, &first_map, [&] { second->call("env", "mix", 4000000U); });
   const auto second_known = verify_samples(independent, fixture, 0);
   BOOST_TEST(first_map.translate(nullptr) == UINT32_MAX);
   BOOST_TEST(first_map.translate(reinterpret_cast<const void*>(UINTPTR_MAX)) == UINT32_MAX);
   BOOST_TEST(first_map.translate(fixture.wasm.data()) == UINT32_MAX);
   BOOST_CHECK_THROW(first->get_module().get_debug().relocate(nullptr), vm::exceptions::profile);

   // Find the actual end of a mapped native interval, not a fabricated PC map.
   auto boundary = known.pc;
   while (boundary - known.pc < 65536 && first_map.translate(reinterpret_cast<const void*>(boundary)) != UINT32_MAX)
      ++boundary;
   BOOST_REQUIRE_LT(boundary - known.pc, 65536U);
   BOOST_TEST(first_map.translate(reinterpret_cast<const void*>(boundary)) == UINT32_MAX);
   BOOST_TEST(first_map.translate(reinterpret_cast<const void*>(boundary - 1)) != UINT32_MAX);

   first.reset();
   BOOST_TEST(second_map.translate(reinterpret_cast<const void*>(second_known.pc)) == second_known.offset);
   BOOST_TEST(second->call_with_return("env", "mix", 130U)->to_ui32() == profile_fixture::expected(130));
   BOOST_CHECK(fixture.wasm == original);
}

BOOST_AUTO_TEST_CASE(profile_public_share_reuses_the_same_map_and_execution_mode) {
   profile_fixture fixture;
   auto source = profiled_backend{fixture.wasm, nullptr};
   auto shared = profiled_backend{};
   shared.share(source);
   BOOST_CHECK(&source.get_module() == &shared.get_module());
   auto context = vm::runtime_context{shared.get_module(), 251, vm::execution_mode::jit_profile};
   shared.set_context(&context);
   shared.initialize();
   const auto samples = collect(source.get_debug(), nullptr, [&] { shared.call("env", "mix", 4000000U); });
   const auto known = verify_samples(samples, fixture, 0);
   BOOST_TEST(shared.get_debug().translate(reinterpret_cast<const void*>(known.pc)) == known.offset);
   for (unsigned i = 0; i != 3; ++i) {
      shared.initialize();
      BOOST_TEST(shared.call_with_return("env", "other", 130U)->to_ui32() == profile_fixture::expected(130, true));
   }
   shared.set_context(nullptr);
}

BOOST_AUTO_TEST_CASE(profile_public_value_observer_survives_backend_destruction_during_thread_sampling) {
   class signal_mask {
    public:
      explicit signal_mask(int operation, bool drain_before_restore = false) : _drain(drain_before_restore) {
         sigemptyset(&_signal);
         sigaddset(&_signal, SIGPROF);
         if (pthread_sigmask(operation, &_signal, &_prior) != 0)
            throw std::runtime_error("Cannot set lifetime-test profiling signal mask");
      }
      ~signal_mask() {
         if (_drain) {
            // The consumer has stopped the timer and joined. Consume any
            // process/main pending SIGPROF before unblocking the restored handler.
            sigset_t pending{};
            if (sigpending(&pending) != 0)
               std::terminate();
            while (sigismember(&pending, SIGPROF) == 1) {
               int consumed{};
               if (sigwait(&_signal, &consumed) != 0 || sigpending(&pending) != 0)
                  std::terminate();
            }
         }
         if (pthread_sigmask(SIG_SETMASK, &_prior, nullptr) != 0)
            std::terminate();
      }
      signal_mask(const signal_mask&) = delete;
      signal_mask& operator=(const signal_mask&) = delete;

    private:
      sigset_t _signal{};
      sigset_t _prior{};
      bool _drain;
   };
   profile_fixture fixture;
   auto backend = std::make_unique<profiled_backend>(fixture.wasm, nullptr);
   const auto initial = collect(backend->get_debug(), nullptr, [&] { backend->call("env", "mix", 4000000U); });
   const auto known = verify_samples(initial, fixture, 0);
   auto observer = backend->get_module().get_debug();
   sample_buffer after_destruction;
   after_destruction.pinned_pc = known.pc;
   std::latch sampler_ready{1};
   std::latch backend_destroyed{1};
   std::exception_ptr failure;
   bool child_mask_restored = false;
   sigset_t original_mask{};
   BOOST_REQUIRE_EQUAL(pthread_sigmask(SIG_SETMASK, nullptr, &original_mask), 0);
   {
      // The child inherits this block. Only it enables delivery, after the
      // handler/observer are installed; main cannot consume its process timer.
      const signal_mask main_block{SIG_BLOCK, true};
      auto consumer = std::thread{[pin = std::move(observer), &after_destruction, &sampler_ready, &backend_destroyed,
                                   &failure, &child_mask_restored]() mutable {
         bool announced = false;
         try {
            {
               const sampling_scope sampling{after_destruction, pin};
               const signal_mask child_delivery{SIG_UNBLOCK};
               sampler_ready.count_down();
               announced = true;
               backend_destroyed.wait();
               after_destruction.count = 0;
               const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
               while (std::chrono::steady_clock::now() < until) {
               }
               // Re-block first; sampling_scope then disables/drains the timer
               // before restoring its prior handler, leaving the child blocked.
            }
            sigset_t restored{};
            child_mask_restored =
                pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0 && sigismember(&restored, SIGPROF) == 1;
         } catch (...) {
            failure = std::current_exception();
            if (!announced)
               sampler_ready.count_down();
         }
         // sampling_scope has stopped/joined delivery before this value pin dies.
         pin = vm::profile_instr_map{};
      }};
      sampler_ready.wait();
      backend.reset();
      backend_destroyed.count_down();
      consumer.join();
   }
   if (failure)
      std::rethrow_exception(failure);
   sigset_t restored_mask{};
   BOOST_REQUIRE_EQUAL(pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask), 0);
   BOOST_TEST(sigismember(&restored_mask, SIGPROF) == sigismember(&original_mask, SIGPROF));
   BOOST_TEST(child_mask_restored);
   BOOST_TEST(observer.translate(reinterpret_cast<const void*>(known.pc)) == UINT32_MAX);
   BOOST_REQUIRE_GT(after_destruction.count, 0);
   for (std::sig_atomic_t i = 0; i < after_destruction.count; ++i)
      BOOST_TEST(after_destruction.values[i].pinned == known.offset);
}

BOOST_AUTO_TEST_CASE(profile_public_observer_pins_bindings_storage_through_cross_thread_unload) {
   profile_fixture fixture;
   const auto allocation = vm::prepared_module::preparation{{UINT64_C(67108864), UINT64_C(67108864)}};
   const auto baseline = allocation.current_bytes();
   vm::profile_instr_map retained;
   sample_buffer samples;
   std::optional<sampling_scope> sampling;
   sample known{};
   {
      std::vector<vm::host_binding> bindings{
          {std::string(256, 'm'), std::string(256, 'n'), {}, [](void*, vm::execution_interface) {}}};
      auto module = vm::prepared_module{fixture.wasm,        vm::options{},
                                        std::span{bindings}, vm::prepared_module::purpose::execution,
                                        allocation,          vm::execution_mode::jit_profile};
      auto context = vm::runtime_context{module, 251};
      context.initialize(nullptr);
      retained = module.get_debug();
      const std::array<vm::operand_stack_elem, 1> arguments{vm::i32_const_t{4000000U}};
      sampling.emplace(samples, retained);
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
      do {
         context.call(nullptr, "mix", arguments);
      } while (std::chrono::steady_clock::now() < until);
   }
   // The backend/module wrapper and context are gone while the consumer's
   // observer is still published to the signal handler.
   sampling.reset();
   known = verify_samples(samples, fixture, 0);
   BOOST_TEST(allocation.current_bytes() > baseline);
   bool observed = false;
   auto consumer = std::thread{[pin = std::move(retained), &observed, known]() mutable {
      observed = pin.translate(reinterpret_cast<const void*>(known.pc)) == known.offset;
      pin = vm::profile_instr_map{};
   }};
   consumer.join();
   BOOST_TEST(observed);
   BOOST_TEST(retained.translate(reinterpret_cast<const void*>(known.pc)) == UINT32_MAX);
   BOOST_TEST(allocation.retry_cleanup());
   BOOST_TEST(allocation.current_bytes() == baseline);
}

BOOST_AUTO_TEST_CASE(profile_public_canceled_execution_keeps_the_map_and_reuses_native_code) {
   struct cancel_host {
      std::stop_source& stop;
      unsigned calls = 0;
      void entered() {
         ++calls;
         stop.request_stop();
      }
   };
   using functions = vm::registered_host_functions<cancel_host>;
   using cancel_backend = vm::backend<functions, vm::jit_profile, vm::default_options, vm::profile_instr_map>;
   functions::add<&cancel_host::entered>("env", "entered");
   profile_fixture fixture{true};
   auto stop = std::stop_source{};
   auto host = cancel_host{stop};
   auto backend = cancel_backend{fixture.wasm, host, nullptr};
   const auto* original_module = &backend.get_module();
   BOOST_TEST(!stop.stop_requested());
   // The guest's host-entry callback requests stop synchronously. There is no
   // waiting helper thread to hang on early failure; the watchdog bounds the loop.
   BOOST_CHECK_THROW(backend.timed_run(vm::watchdog{std::chrono::seconds{5}, stop.get_token()},
                                       [&] { backend.call(host, "env", "cancel"); }),
                     vm::exceptions::canceled);
   BOOST_TEST(host.calls == 1U);
   BOOST_TEST(stop.stop_requested());
   BOOST_TEST(&backend.get_module() == original_module);
   BOOST_TEST(backend.call_with_return(host, "env", "mix", 130U)->to_ui32() == profile_fixture::expected(130));
   const auto samples = collect(backend.get_debug(), nullptr, [&] { backend.call(host, "env", "other", 4000000U); });
   verify_samples(samples, fixture, 1);
   BOOST_TEST(host.calls == 1U);
}

BOOST_AUTO_TEST_CASE(profile_public_plain_jit_never_exposes_an_instruction_map) {
   profile_fixture fixture;
   using plain_backend = vm::backend<std::nullptr_t, vm::jit, vm::default_options, vm::profile_instr_map>;
   auto plain = plain_backend{fixture.wasm, nullptr};
   const auto samples = collect(plain.get_debug(), nullptr, [&] { plain.call("env", "mix", 4000000U); });
   BOOST_REQUIRE_GT(samples.count, 0);
   for (std::sig_atomic_t i = 0; i < samples.count; ++i)
      BOOST_TEST(samples.values[i].offset == UINT32_MAX);
   auto unprofiled = plain.get_module().get_debug();
   for (std::sig_atomic_t i = 0; i < samples.count; ++i)
      BOOST_TEST(unprofiled.translate(reinterpret_cast<const void*>(samples.values[i].pc)) == UINT32_MAX);
   BOOST_CHECK_THROW(vm::runtime_context(plain.get_module(), 251, vm::execution_mode::jit_profile),
                     vm::exceptions::profile);
}

BOOST_AUTO_TEST_CASE(profile_public_empty_module_has_no_native_pc_map) {
   auto empty = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   auto backend = profiled_backend{empty, nullptr};
   BOOST_CHECK_NO_THROW(backend.validate());
   BOOST_TEST(backend.get_debug().translate(nullptr) == UINT32_MAX);
   BOOST_TEST(backend.get_debug().translate(empty.data()) == UINT32_MAX);
   auto observer = backend.get_module().get_debug();
   BOOST_TEST(observer.translate(reinterpret_cast<const void*>(UINTPTR_MAX)) == UINT32_MAX);
}
#else
BOOST_AUTO_TEST_CASE(profile_public_requires_the_explicit_llvm_runtime_profile) {
   profile_fixture fixture;
   BOOST_CHECK_THROW(profiled_backend(fixture.wasm, nullptr), vm::exceptions::execution_unavailable);
}
#endif

BOOST_AUTO_TEST_CASE(profile_public_canceled_preparation_does_not_publish_or_retain_storage) {
   profile_fixture fixture;
   auto stop = std::stop_source{};
   const auto allocation = vm::prepared_module::preparation{{UINT64_C(67108864), UINT64_C(67108864)}, stop.get_token()};
   const auto baseline = allocation.current_bytes();
   stop.request_stop();
   std::span<vm::host_binding> bindings;
   BOOST_CHECK_THROW(vm::prepared_module(fixture.wasm, vm::options{}, bindings, vm::prepared_module::purpose::execution,
                                         allocation, vm::execution_mode::jit_profile),
                     vm::exceptions::canceled);
   BOOST_TEST(allocation.retry_cleanup());
   BOOST_TEST(allocation.current_bytes() == baseline);
#if FORGE_VM_WASM_INTERPRET_HAS_JIT
   const auto fresh = vm::prepared_module::preparation{{UINT64_C(67108864), UINT64_C(67108864)}};
   const auto fresh_baseline = fresh.current_bytes();
   {
      auto module = vm::prepared_module{fixture.wasm, vm::options{},
                                        bindings,     vm::prepared_module::purpose::execution,
                                        fresh,        vm::execution_mode::jit_profile};
      auto context = vm::runtime_context{module, 251};
      context.initialize(nullptr);
      const std::array<vm::operand_stack_elem, 1> arguments{vm::i32_const_t{130U}};
      BOOST_TEST(context.call(nullptr, "mix", arguments)->to_ui32() == profile_fixture::expected(130));
   }
   BOOST_TEST(fresh.retry_cleanup());
   BOOST_TEST(fresh.current_bytes() == fresh_baseline);
#endif
}
