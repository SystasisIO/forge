#include "wasm_export.h"
#include "details/engine_instance.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace {

namespace vm = forge::vm::wasm::interpret::detail;

void require(bool condition, const char* message) {
   if (!condition) {
      throw std::runtime_error(message);
   }
}

struct probe_state {
   std::atomic_bool entered{false};
};

struct start_state {
   std::uint32_t mode{};
   std::atomic_uint calls{};
};

std::uint32_t host_start(wasm_exec_env_t env) noexcept {
   auto state = static_cast<start_state*>(wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(env)));
   if (!state) return 2;
   state->calls.fetch_add(1, std::memory_order_release);
   return state->mode;
}

struct fp_snapshot {
   std::fenv_t environment;
   fp_snapshot() { require(std::fegetenv(&environment) == 0, "cannot save FP state"); }
   ~fp_snapshot() { std::fesetenv(&environment); }
};

void host_poison_fp(wasm_exec_env_t) noexcept {
   std::fesetround(FE_UPWARD);
   std::feraiseexcept(FE_INVALID);
}

std::uint32_t host_nested_fp(wasm_exec_env_t env) noexcept {
   auto inst = wasm_runtime_get_module_inst(env);
   auto function = wasm_runtime_lookup_function(inst, "nearest64_bits");
   if (!function || std::fesetround(FE_UPWARD) != 0) return 0;
   std::uint32_t cells[2]{0, 0x40040000U};
   const bool success = wasm_runtime_call_wasm(env, function, 2, cells);
   return success && cells[0] == 0 && cells[1] == 0x40000000U && std::fegetround() == FE_UPWARD;
}

std::uint32_t host_sum(wasm_exec_env_t env, std::uint32_t offset, std::uint32_t length) noexcept {
   auto instance = wasm_runtime_get_module_inst(env);
   if (!wasm_runtime_validate_app_addr(instance, offset, length)) {
      return 0;
   }
   auto bytes = static_cast<const std::uint8_t*>(wasm_runtime_addr_app_to_native(instance, offset));
   std::uint32_t result = 0;
   for (std::uint32_t i = 0; i < length; ++i) {
      result += bytes[i];
   }
   return result;
}

void host_entered(wasm_exec_env_t env) noexcept {
   auto state = static_cast<probe_state*>(wasm_runtime_get_user_data(env));
   if (state) {
      state->entered.store(true, std::memory_order_release);
   }
}

class runtime {
 public:
   runtime() : _engine(vm::engine_runtime::acquire()) {
      require(wasm_runtime_register_natives("probe", _symbols.data(), _symbols.size()), "probe import registration failed");
   }
   ~runtime() { wasm_runtime_unregister_natives("probe", _symbols.data()); }
   runtime(const runtime&) = delete;
   runtime& operator=(const runtime&) = delete;
   std::shared_ptr<vm::engine_runtime> get() const { return _engine; }

 private:
   std::shared_ptr<vm::engine_runtime> _engine;
   std::array<NativeSymbol, 5> _symbols{{
      {"sum", reinterpret_cast<void*>(host_sum), "(ii)i", nullptr},
      {"entered", reinterpret_cast<void*>(host_entered), "()", nullptr},
      {"poison_fp", reinterpret_cast<void*>(host_poison_fp), "()", nullptr},
      {"nested_fp", reinterpret_cast<void*>(host_nested_fp), "()i", nullptr},
      {"start", reinterpret_cast<void*>(host_start), "()i", nullptr}
   }};
};

class module {
 public:
   module(std::shared_ptr<vm::engine_runtime> engine, const char* path) {
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      require(input.good(), "cannot open fixture");
      const auto size = input.tellg();
      require(size > 0 && size <= 4 * 1024 * 1024, "fixture size outside probe budget");
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
      input.seekg(0);
      require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size)), "fixture read failed");
      const auto original = bytes;
      _module = std::make_shared<vm::engine_module>(std::move(engine), bytes);
      require(bytes == original, "loader modified caller-owned bytes");
   }
   module(const module&) = delete;
   module& operator=(const module&) = delete;
   std::shared_ptr<vm::engine_module> get() const { return _module; }

 private:
   std::shared_ptr<vm::engine_module> _module;
};

class instance {
 public:
   explicit instance(const module& source) : _engine(source.get(), 64 * 1024) {}
   instance(const instance&) = delete;
   instance& operator=(const instance&) = delete;
   wasm_module_inst_t get() const { return _engine.get(); }
   wasm_exec_env_t env() const { return _engine.env(); }
   void cancel() { _engine.cancel(); }

   bool invoke(const char* name, std::uint32_t count, std::span<std::uint32_t> cells) {
      return _engine.invoke(name, count, cells);
   }

   std::uint32_t call32(const char* name, std::initializer_list<std::uint32_t> arguments = {}) {
      require(arguments.size() <= 8, "probe argument overflow");
      std::array<std::uint32_t, 8> cells{};
      std::copy(arguments.begin(), arguments.end(), cells.begin());
      require(invoke(name, arguments.size(), cells), "unexpected guest trap");
      return cells[0];
   }

   std::uint64_t call64(const char* name, std::uint64_t a, std::uint64_t b = 0, bool unary = false) {
      std::array<std::uint32_t, 8> cells{
         static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(a >> 32),
         static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(b >> 32)};
      require(invoke(name, unary ? 2 : 4, cells), "unexpected 64-bit guest trap");
      return (static_cast<std::uint64_t>(cells[1]) << 32) | cells[0];
   }

   std::uint64_t floating(std::uint32_t op, std::uint64_t a, std::uint64_t b) {
      std::array<std::uint32_t, 8> cells{op, static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(a >> 32),
         static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(b >> 32)};
      require(invoke("fp64", 5, cells), "FP corpus trapped");
      return (static_cast<std::uint64_t>(cells[1]) << 32) | cells[0];
   }

 private:
   vm::engine_instance _engine;
};

void initialization(const module& source) {
   start_state skipped{0};
   std::stop_source pre_stop;
   pre_stop.request_stop();
   bool rejected = false;
   try { vm::engine_instance guest(source.get(), 65536, pre_stop.get_token(), &skipped); }
   catch (const vm::engine_instance::canceled&) { rejected = true; }
   require(rejected && skipped.calls == 0, "pre-start cancellation executed guest code");

   start_state spinning{1};
   std::stop_source stop;
   std::exception_ptr error;
   bool canceled = false;
   std::jthread worker([&] {
      try {
         const fp_snapshot saved;
         require(std::fesetround(FE_DOWNWARD) == 0, "cannot change start rounding");
         try { vm::engine_instance guest(source.get(), 65536, stop.get_token(), &spinning); }
         catch (const vm::engine_instance::canceled&) { canceled = true; }
         require(std::fegetround() == FE_DOWNWARD, "canceled start changed FP environment");
      } catch (...) { error = std::current_exception(); }
   });
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while (!spinning.calls.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
   stop.request_stop();
   worker.join();
   if (error) std::rethrow_exception(error);
   require(canceled && spinning.calls == 1, "real start loop was not canceled");

   start_state trapping{2};
   rejected = false;
   try { vm::engine_instance guest(source.get(), 65536, {}, &trapping); }
   catch (const std::runtime_error&) { rejected = true; }
   require(rejected && trapping.calls == 1, "start trap was not rejected");

   start_state normal{0};
   std::stop_source invocation_stop;
   vm::engine_instance guest(source.get(), 65536, invocation_stop.get_token(), &normal);
   require(normal.calls == 1, "start did not run exactly once");
   std::array<std::uint32_t, 1> result{};
   invocation_stop.request_stop();
   rejected = false;
   try { guest.invoke("answer", 0, result, invocation_stop.get_token()); }
   catch (const vm::engine_instance::canceled&) { rejected = true; }
   require(rejected, "stop between start and apply was lost");
   require(guest.invoke("answer", 0, result) && result[0] == 42 && normal.calls == 1,
           "healthy invocation after cancellation failed or reran start");
   require(!wasm_runtime_initialize(guest.get()) && normal.calls == 1, "start was executed twice");
}

void core(const module& source) {
   instance guest(source);
   require(guest.call32("add", {19, 23}) == 42, "i32 result mismatch");
   require(guest.call64("wide", 0x123456789abcdef0ULL, 0x1122334455667788ULL) ==
           (0x123456789abcdef0ULL ^ 0x1122334455667788ULL), "i64 result mismatch");
   require(guest.call32("imported") == 26, "host import result mismatch");
}

void native_abi(const module& source) {
   instance guest(source);
   std::array<std::uint32_t, 20> cells{2, std::bit_cast<std::uint32_t>(40.0f)};
   require(guest.invoke("native_f32", 2, cells) && std::bit_cast<float>(cells[0]) == 42.0f,
           "native f32 argument/result mismatch");
   const auto bits = std::bit_cast<std::uint64_t>(40.0);
   cells = {2, static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32)};
   const auto result = [&] { return std::bit_cast<double>((std::uint64_t{cells[1]} << 32) | cells[0]); };
   require(guest.invoke("native_f64", 3, cells) && result() == 42.0,
           "native f64 argument at odd cell offset/result mismatch");
   cells.fill(0);
   cells[0] = 2;
   for (std::uint32_t index = 0; index < 9; ++index) {
      const auto value = std::bit_cast<std::uint64_t>(double(index + 1));
      cells[1 + index * 2] = static_cast<std::uint32_t>(value);
      cells[2 + index * 2] = static_cast<std::uint32_t>(value >> 32);
   }
   require(guest.invoke("native_spill", 19, cells) && result() == 47.0,
           "native floating register/stack argument mismatch");
}

void memory(const module& source) {
   instance guest(source);
   require(guest.call32("pages") == 2, "unexpected initial memory");
   require(guest.call32("grow", {1}) == 2, "first memory.grow failed");
   require(guest.call32("grow", {1}) == 3, "second memory.grow failed");
   require(guest.call32("grow", {1}) == std::numeric_limits<std::uint32_t>::max(), "memory maximum not enforced");
   require(guest.call32("pages") == 4, "failed growth changed memory");
}

void traps(const module& source) {
   instance guest(source);
   std::array<std::uint32_t, 8> cells{1, 0, 0, 0};
   require(!guest.invoke("divide", 2, cells), "division by zero did not trap");
   require(wasm_runtime_get_exception(guest.get()) != nullptr, "trap has no diagnostic");
   cells = {0xffffffffU, 0, 0, 0};
   require(!guest.invoke("read_byte", 1, cells), "out-of-bounds read did not trap");
   require(guest.call32("add", {20, 22}) == 42, "sequential call after trap failed");
}

void host_pointer(const module& source) {
   instance guest(source);
   std::array<std::uint32_t, 8> cells{0xfffffff0U, 64, 0, 0};
   require(!guest.invoke("imported_range", 2, cells), "overflowing host range was accepted");
}

void isolation(const module& source) {
   instance a(source), b(source);
   require(a.call32("increment") == 1 && a.call32("increment") == 2, "instance state not retained");
   require(b.call32("increment") == 1, "instances share mutable guest state");
   instance fresh(source);
   require(fresh.call32("increment") == 1, "fresh instance did not reset guest state");
}

void call_buffers(const module& source) {
   vm::engine_instance guest(source.get(), 64 * 1024);
   const auto rejected = [&](auto&& call) {
      bool caught = false;
      try { call(); } catch (const std::invalid_argument&) { caught = true; }
      require(caught, "invalid call buffer accepted");
   };
   std::array<std::uint32_t, 4> cells{};
   rejected([&] { guest.invoke("wide", 2, cells); });
   rejected([&] { guest.invoke("wide", 4, std::span(cells).first(1)); });
   rejected([&] { guest.invoke("promote", 1, std::span(cells).first(1)); });
   rejected([&] { guest.invoke("absent", 0, cells); });
   cells = {20, 22, 0, 0};
   require(guest.invoke("add", 2, cells) && cells[0] == 42, "rejected call poisoned instance");
}

void ownership(const char* path) {
   auto source = std::make_unique<module>(vm::engine_runtime::acquire(), path);
   std::weak_ptr<vm::engine_module> loaded = source->get();
   auto guest = std::make_unique<instance>(*source);
   source.reset();
   require(!loaded.expired(), "instance lost its code owner");
   require(guest->call32("add", {20, 22}) == 42, "instance used unloaded code");
   guest.reset();
   require(loaded.expired(), "instance leaked its code owner");
}

void nested_fp(const module& source) {
   const fp_snapshot outer;
   instance guest(source);
   require(std::fesetround(FE_DOWNWARD) == 0, "cannot set host rounding");
   require(guest.call32("nested_boundary") == 1, "nested guest call leaked FP state");
   require(std::fegetround() == FE_DOWNWARD, "outer host rounding changed");
}

void cancel(const module& source) {
   instance a(source), b(source);
   probe_state state;
   wasm_runtime_set_user_data(a.env(), &state);
   std::atomic_bool finished{false};
   std::atomic_bool interrupted{false};
   std::exception_ptr worker_error;
   // A joinable test thread is bounded externally by the CTest process timeout.
   std::jthread worker([&] {
      try {
         const fp_snapshot saved;
         require(std::fesetround(FE_DOWNWARD) == 0, "cannot perturb worker rounding");
         std::feraiseexcept(FE_DIVBYZERO);
         const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
         std::array<std::uint32_t, 8> cells{};
         interrupted.store(!a.invoke("spin", 0, cells), std::memory_order_release);
         require(std::fegetround() == FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT) == flags,
                 "cancellation leaked FP state");
      } catch (...) {
         worker_error = std::current_exception();
      }
      finished.store(true, std::memory_order_release);
   });
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while (!state.entered.load(std::memory_order_acquire) && !finished.load(std::memory_order_acquire) &&
          std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
   }
   a.cancel();
   worker.join();
   if (worker_error) {
      std::rethrow_exception(worker_error);
   }
   require(state.entered.load(), "cancellation did not reach a running guest");
   require(interrupted.load(), "guest was not interrupted");
   require(b.call32("add", {20, 22}) == 42, "canceling A affected B");
   instance next(source);
   require(next.call32("add", {20, 22}) == 42, "runtime unusable after cancellation");
}

void numeric(const module& source) {
   instance guest(source);
   const auto output = [](const char* name, std::uint64_t value) {
      std::cout << "bits " << name << " " << std::hex << value << std::dec << '\n';
   };
   const auto exact = guest.call64("add64_bits", std::bit_cast<std::uint64_t>(1.5), std::bit_cast<std::uint64_t>(2.25));
   require(exact == std::bit_cast<std::uint64_t>(3.75), "ordinary f64 addition mismatch");
   output("finite64", exact);
   output("nan64", guest.call64("add64_bits", 0x7ff8000000000123ULL, 0x7ff8000000000456ULL));
   output("snan64", guest.call64("add64_bits", 0x7ff0000000000001ULL, 0x3ff0000000000000ULL));
   const auto zero = guest.call64("add64_bits", 0x8000000000000000ULL, 0x8000000000000000ULL);
   const auto small64 = guest.call64("add64_bits", 1, 1);
   const auto small32 = guest.call32("add32_bits", {1, 1});
   require(zero == 0x8000000000000000ULL, "negative zero was not preserved");
   require(small64 == 2 && small32 == 2, "subnormal arithmetic mismatch");
   output("negative_zero64", zero);
   output("subnormal64", small64);
   output("nan32", guest.call32("add32_bits", {0x7fc00123U, 0x7fc00456U}));
   output("subnormal32", small32);
}

void rounding(const module& source) {
   instance guest(source);
   struct environment_guard {
      std::fenv_t saved;
      environment_guard() { require(std::fegetenv(&saved) == 0, "cannot save FP environment"); }
      ~environment_guard() { std::fesetenv(&saved); }
   } guard;
   for (const auto mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
      require(std::fesetround(mode) == 0, "cannot set host rounding mode");
      const auto result = guest.call64("nearest64_bits", std::bit_cast<std::uint64_t>(2.5), 0, true);
      std::cout << "rounding " << mode << " result " << std::hex << result << std::dec << '\n';
      require(result == std::bit_cast<std::uint64_t>(2.0), "guest nearest depends on host rounding mode");
      require(std::fegetround() == mode, "guest changed host rounding mode");
   }
}

void fp_boundary(const module& source) {
   instance guest(source);
   fp_snapshot saved;
   require(std::fesetround(FE_DOWNWARD) == 0, "cannot set host FP mode");
   std::feclearexcept(FE_ALL_EXCEPT);
   std::feraiseexcept(FE_DIVBYZERO);
   const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
   require(guest.call64("after_host", std::bit_cast<std::uint64_t>(2.5), 0, true) ==
           std::bit_cast<std::uint64_t>(2.0), "host callback changed guest rounding");
   require(std::fegetround() == FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT) == flags,
           "host FP state leaked after callback");
   std::array<std::uint32_t, 8> cells{1, 0};
   require(!guest.invoke("divide", 2, cells), "expected trap");
   require(std::fegetround() == FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT) == flags,
           "host FP state leaked after trap");
}

void subnormal_environment(const module& source) {
   instance guest(source);
   fp_snapshot saved;
#if defined(__x86_64__)
   const auto control = _mm_getcsr() | 0x8040U;
   _mm_setcsr(control);
#else
   std::uint64_t control;
   __asm__ volatile("mrs %0, fpcr" : "=r"(control));
   control |= 1ULL << 24;
   __asm__ volatile("msr fpcr, %0" : : "r"(control) : "memory");
#endif
   require(guest.call64("add64_bits", 1, 1) == 2, "host flush-to-zero affected f64");
   require(guest.call32("add32_bits", {1, 1}) == 2, "host flush-to-zero affected f32");
#if defined(__x86_64__)
   require(_mm_getcsr() == control, "host MXCSR not restored");
#else
   std::uint64_t restored;
   __asm__ volatile("mrs %0, fpcr" : "=r"(restored));
   require(restored == control, "host FPCR not restored");
#endif
}

void concurrent_cancel(const module& source) {
   instance a(source), b(source);
   probe_state state_a, state_b;
   wasm_runtime_set_user_data(a.env(), &state_a);
   wasm_runtime_set_user_data(b.env(), &state_b);
   std::atomic_bool finished_a{false}, finished_b{false}, stopped_a{false}, stopped_b{false};
   std::exception_ptr error_a, error_b;
   const auto run = [](instance& guest, std::atomic_bool& finished, std::atomic_bool& stopped,
                       std::exception_ptr& error, int rounding) {
      try {
         const fp_snapshot saved;
         require(std::fesetround(rounding) == 0, "cannot perturb concurrent worker rounding");
         std::array<std::uint32_t, 8> cells{};
         stopped = !guest.invoke("spin", 0, cells);
         require(std::fegetround() == rounding, "concurrent call changed worker rounding");
      } catch (...) {
         error = std::current_exception();
      }
      finished = true;
   };
   std::jthread first(run, std::ref(a), std::ref(finished_a), std::ref(stopped_a), std::ref(error_a), FE_UPWARD);
   std::jthread second(run, std::ref(b), std::ref(finished_b), std::ref(stopped_b), std::ref(error_b), FE_DOWNWARD);
   const auto wait = [](auto predicate) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!predicate() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
   };
   wait([&] { return state_a.entered.load() && state_b.entered.load(); });
   bool rejected = false;
   try { a.call32("add", {1, 2}); } catch (const std::logic_error&) { rejected = true; }
   a.cancel();
   wait([&] { return finished_a.load(); });
   const bool isolated = finished_a.load() && !finished_b.load();
   b.cancel();
   first.join();
   second.join();
   if (error_a) std::rethrow_exception(error_a);
   if (error_b) std::rethrow_exception(error_b);
   require(rejected, "overlapping call was not rejected");
   require(state_a.entered && state_b.entered && isolated && stopped_a && stopped_b,
           "canceling A did not isolate the active B invocation");
   require(a.call32("add", {1, 2}) == 3 && b.call32("add", {3, 4}) == 7, "reuse after cancellation failed");
}

void corpus(const module& source) {
   instance guest(source);
   constexpr std::uint64_t canon64 = 0x7ff8000000000000ULL;
   constexpr std::uint32_t canon32 = 0x7fc00000U;
   const std::array<std::uint64_t, 15> inputs{
      0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL, 0x0010000000000000ULL,
      0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x4004000000000000ULL,
      0x7fefffffffffffffULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
      0x7ff0000000000001ULL, 0xfff0000000000001ULL, 0x7ff8000000000123ULL, 0xfff8000000000456ULL};
   const std::array<std::uint32_t, 15> small{
      0, 0x80000000U, 1, 0x80000001U, 0x00800000U, 0x3f800000U, 0xbf800000U,
      0x40200000U, 0x7f7fffffU, 0x7f800000U, 0xff800000U, 0x7f800001U,
      0xff800001U, 0x7fc00123U, 0xffc00456U};
   for (std::uint32_t op = 0; op < 14; ++op) {
      for (std::size_t i = 0; i < inputs.size(); ++i) {
         for (std::size_t j = 0; j < inputs.size(); ++j) {
            const auto a = inputs[i], b = inputs[j];
            const auto x = small[i], y = small[j];
            const auto r = guest.floating(op, a, b);
            const auto s = guest.call32("fp32", {op, x, y});
            if (op <= 10) {
               if ((r & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL) require(r == canon64, "noncanonical f64 NaN");
               if ((s & 0x7fffffffU) > 0x7f800000U) require(s == canon32, "noncanonical f32 NaN");
            } else {
               auto expected64 = op == 11 ? a & 0x7fffffffffffffffULL :
                  op == 12 ? a ^ 0x8000000000000000ULL : (a & 0x7fffffffffffffffULL) | (b & 0x8000000000000000ULL);
               auto expected32 = op == 11 ? x & 0x7fffffffU :
                  op == 12 ? x ^ 0x80000000U : (x & 0x7fffffffU) | (y & 0x80000000U);
               require(r == expected64 && s == expected32, "bitwise FP operation changed payload");
            }
            std::cout << "bits " << op << ':' << i << ':' << j << ' ' << std::hex << r << ':' << s << std::dec << '\n';
         }
      }
   }
   for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto demoted = guest.call32("demote", {static_cast<std::uint32_t>(inputs[i]), static_cast<std::uint32_t>(inputs[i] >> 32)});
      std::array<std::uint32_t, 8> cells{small[i]};
      require(guest.invoke("promote", 1, cells), "promote failed");
      const auto promoted = (static_cast<std::uint64_t>(cells[1]) << 32) | cells[0];
      if (i >= 11) require(demoted == canon32 && promoted == canon64, "FP conversion did not canonicalize NaN");
      std::cout << "bits convert:" << i << ' ' << std::hex << promoted << ':' << demoted << std::dec << '\n';
   }
}

} // namespace

int main(int argc, char** argv) {
   try {
      require(argc == 3, "usage: wamr_probe CASE FIXTURE");
      runtime engine;
      const fp_snapshot original_fp;
      int expected_rounding = std::fegetround();
      if (const auto requested = std::getenv("FORGE_WAMR_HOST_ROUNDING")) {
         const std::string mode(requested);
         if (mode == "up") expected_rounding = FE_UPWARD;
         else if (mode == "down") expected_rounding = FE_DOWNWARD;
         else if (mode == "zero") expected_rounding = FE_TOWARDZERO;
         else if (mode == "nearest") expected_rounding = FE_TONEAREST;
         else throw std::runtime_error("unknown host rounding mode");
         require(std::fesetround(expected_rounding) == 0, "cannot set requested host rounding");
      }
      module source(engine.get(), argv[2]);
      const std::string test = argv[1];
      if (test == "core") core(source);
      else if (test == "initialization") initialization(source);
      else if (test == "native_abi") native_abi(source);
      else if (test == "memory") memory(source);
      else if (test == "traps") traps(source);
      else if (test == "host_pointer") host_pointer(source);
      else if (test == "isolation") isolation(source);
      else if (test == "cancel") cancel(source);
      else if (test == "numeric") numeric(source);
      else if (test == "rounding") rounding(source);
      else if (test == "fp_boundary") fp_boundary(source);
      else if (test == "subnormal_environment") subnormal_environment(source);
      else if (test == "concurrent_cancel") concurrent_cancel(source);
      else if (test == "corpus") corpus(source);
      else if (test == "call_buffers") call_buffers(source);
      else if (test == "ownership") ownership(argv[2]);
      else if (test == "nested_fp") nested_fp(source);
      else throw std::runtime_error("unknown probe case");
      require(std::fegetround() == expected_rounding, "test did not preserve outer FP rounding");
      std::cout << "PASS " << test << '\n';
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   }
}
