#include "details/engine_memory.hxx"
#include "details/engine_module.hxx"
#include "details/engine_thread_scope.hxx"
#include "instruction_profile_fixture.hxx"

#include <wasm_loader.h>
#include <aot_jit_profile.h>
#include <aot_jit_profile_metadata.h>
#include <wasm_exec_env.h>
#include <wasm_runtime.h>
#include <wasm_runtime_common.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <sys/time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

namespace vm = forge::vm::wasm::interpret::detail;

namespace {
void require(bool value, const char* text) {
   if (!value)
      throw std::runtime_error(text);
}

void checked(LLVMErrorRef error) {
   if (!error)
      return;
   char* message = LLVMGetErrorMessage(error);
   std::string text{message};
   LLVMDisposeErrorMessage(message);
   throw std::runtime_error(text);
}

// Dedicated subprocess only: no production sampler, worker or signal owner is
// introduced. The handler reads the finalized immutable map without allocating.
constexpr std::size_t sample_capacity = 8192;
std::array<std::uintptr_t, sample_capacity> sampled_pcs{};
std::array<std::uint32_t, sample_capacity> sampled_offsets{};
volatile std::sig_atomic_t sample_count = 0;
const AOTCompContext* sample_context = nullptr;

void sample_handler(int, siginfo_t*, void* raw) {
   std::uintptr_t pc{};
   const auto* context = static_cast<const ucontext_t*>(raw);
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
   const auto index = sample_count;
   if (index < static_cast<std::sig_atomic_t>(sample_capacity)) {
      sampled_pcs[index] = pc;
      sampled_offsets[index] = aot_jit_profile_translate(sample_context, pc);
      sample_count = index + 1;
   }
}

class sampler {
 public:
   explicit sampler(const AOTCompContext* context) {
      require(getitimer(ITIMER_PROF, &_prior_timer) == 0, "Cannot inspect profiling timer");
      require(!_prior_timer.it_value.tv_sec && !_prior_timer.it_value.tv_usec,
              "Profiling probe requires an exclusive subprocess timer");
      struct sigaction action{};
      action.sa_sigaction = sample_handler;
      action.sa_flags = SA_SIGINFO;
      sigemptyset(&action.sa_mask);
      require(sigaction(SIGPROF, &action, &_prior_action) == 0, "Cannot install sampling handler");
      sample_count = 0;
      sample_context = context;
      struct itimerval timer{};
      timer.it_interval.tv_usec = 1000;
      timer.it_value = timer.it_interval;
      if (setitimer(ITIMER_PROF, &timer, nullptr) != 0) {
         sigaction(SIGPROF, &_prior_action, nullptr);
         sample_context = nullptr;
         throw std::runtime_error("Cannot arm sampling timer");
      }
   }
   ~sampler() {
      // Stop delivery before destroying the compiler or its instruction map.
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
      sigaction(SIGPROF, &_prior_action, nullptr);
      sample_context = nullptr;
      setitimer(ITIMER_PROF, &_prior_timer, nullptr);
      sigprocmask(SIG_SETMASK, &prior, nullptr);
   }
   sampler(const sampler&) = delete;
   sampler& operator=(const sampler&) = delete;

 private:
   struct sigaction _prior_action{};
   struct itimerval _prior_timer{};
};

const AOTJitProfileInterval* rows(const WASMModule* module, std::uint32_t& count) {
   return aot_jit_profile_intervals(module ? module->comp_ctx : nullptr, &count);
}
std::uint32_t translate(const WASMModule* module, std::uintptr_t pc) {
   return aot_jit_profile_translate(module ? module->comp_ctx : nullptr, pc);
}
enum class refusal { none, early_heap, early_cancel, late_cancel, mapping };

// Test-only wrapper of the real owner callbacks. The first check with a profile
// present runs after DIBuilder construction, before its first function record.
// Reserving remaining owner capacity refuses that WAMR allocation, not LLVM's
// excluded transient heap. No production allocator/fault fallback is added.
class observed_budget {
 public:
   observed_budget(std::shared_ptr<vm::engine_load_budget> owner, refusal point)
       : owner(std::move(owner)), point(point) {}
   ~observed_budget() {
      release_reservation();
   }
   void attach(const vm::engine_module& source) {
      original = source.native_load_budget();
      module = reinterpret_cast<WASMModule*>(source.get());
   }
   WASMLoadBudget callbacks() {
      WASMLoadBudget result{};
      result.context = this;
      result.begin_mapping = [](void* p, std::uint64_t retained, std::uint64_t peak) {
         auto& self = *static_cast<observed_budget*>(p);
         ++self.mapping_attempts;
         return self.original.begin_mapping(self.original.context, retained, peak);
      };
      result.complete_mapping = [](void* p, void* token, void* address) {
         auto& self = *static_cast<observed_budget*>(p);
         self.original.complete_mapping(self.original.context, token, address);
      };
      result.unmap = [](void* p, void* address, std::uint64_t retained) {
         auto& self = *static_cast<observed_budget*>(p);
         return self.original.unmap(self.original.context, address, retained);
      };
      result.before_protect = [](void* p) {
         auto& self = *static_cast<observed_budget*>(p);
         return self.original.before_protect(self.original.context);
      };
      result.allocation_failed = [](void* p) {
         auto& self = *static_cast<observed_budget*>(p);
         self.original.allocation_failed(self.original.context);
      };
      result.protection_failed = [](void* p) {
         auto& self = *static_cast<observed_budget*>(p);
         self.original.protection_failed(self.original.context);
      };
      result.check = [](void* p) { return static_cast<observed_budget*>(p)->check(); };
      return result;
   }
   void release_reservation() {
      owner->release(reserved);
      reserved = 0;
   }
   bool triggered = false;
   bool published_during_compile = false;
   bool inspected_noinline = false;
   bool kept_noinline = true;
   std::uint32_t mapping_attempts = 0;

 private:
   bool check() {
      std::uint32_t count{};
      if (rows(module, count) || count)
         published_during_compile = true;
      auto* context = module ? module->comp_ctx : nullptr;
      if (context && context->jit_instruction_profile) {
         if (!inspected_noinline) {
            const auto kind = LLVMGetEnumAttributeKindForName("noinline", 8);
            for (std::uint32_t i = 0; i < context->func_ctx_count; ++i) {
               kept_noinline = kept_noinline && LLVMGetEnumAttributeAtIndex(context->func_ctxes[i]->func,
                                                                            LLVMAttributeFunctionIndex, kind);
               kept_noinline = kept_noinline && LLVMGetEnumAttributeAtIndex(context->func_ctxes[i]->precheck_func,
                                                                            LLVMAttributeFunctionIndex, kind);
            }
            inspected_noinline = true;
         }
         if (!triggered && (point == refusal::early_heap || point == refusal::early_cancel ||
                            (point == refusal::late_cancel && context->jit_stack_sizes))) {
            triggered = true;
            if (point == refusal::early_heap) {
               reserved = owner->limit_bytes() - owner->current_bytes();
               if (!owner->reserve(reserved))
                  return false;
            } else {
               owner->fail(vm::engine_load_budget::failure::cancelled);
               return false;
            }
         }
      }
      return original.check(original.context);
   }
   std::shared_ptr<vm::engine_load_budget> owner;
   refusal point;
   WASMLoadBudget original{};
   WASMModule* module{};
   std::uint64_t reserved = 0;
};

class instance {
 public:
   explicit instance(wasm_module_t module) {
      const vm::engine_memory::scope memory{_memory};
      InstantiationArgs2* raw{};
      require(wasm_runtime_instantiation_args_create(&raw), "Cannot create instance arguments");
      const std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)> args{
          raw, wasm_runtime_instantiation_args_destroy};
      wasm_runtime_instantiation_args_set_default_stack_size(args.get(), 1024 * 1024);
      wasm_runtime_instantiation_args_set_defer_start(args.get(), true);
      std::array<char, 512> error{};
      _instance = wasm_runtime_instantiate_ex2(module, args.get(), error.data(), error.size());
      if (!_instance)
         throw std::runtime_error(std::string("Profile instantiate: ") + error.data());
      const auto cleanup = [this](void*) { destroy(); };
      std::unique_ptr<void, decltype(cleanup)> guard{this, cleanup};
      _memory.bind_instance(_instance);
      require(wasm_runtime_set_running_mode(_instance, Mode_LLVM_JIT), "LLVM mode was rejected");
      require(wasm_runtime_get_running_mode(_instance) == Mode_LLVM_JIT, "Engine substituted another mode");
      _env = wasm_runtime_create_exec_env(_instance, 1024 * 1024);
      require(_env != nullptr, "Cannot create profile execution environment");
      _env->forge_max_call_depth = 64;
      require(wasm_runtime_initialize_with_exec_env(_instance, _env), "Profile fixture initialization failed");
      guard.release();
   }
   ~instance() {
      destroy();
   }
   instance(const instance&) = delete;
   instance& operator=(const instance&) = delete;
   std::uint32_t invoke(const char* name, std::uint32_t count) {
      const vm::engine_memory::scope memory{_memory};
      auto* function = wasm_runtime_lookup_function(_instance, name);
      require(function != nullptr, "Profile fixture export is missing");
      require(wasm_runtime_call_wasm(_env, function, 1, &count), "Native profile execution failed");
      require(_env->forge_call_depth == 0, "Native profile execution leaked logical depth");
      return count;
   }

 private:
   void destroy() {
      const vm::engine_memory::scope memory{_memory};
      if (_env)
         wasm_runtime_destroy_exec_env(_env);
      if (_instance)
         wasm_runtime_deinstantiate(_instance);
      _memory.bind_instance(nullptr);
   }
   vm::engine_memory _memory = vm::engine_memory::system();
   wasm_module_inst_t _instance{};
   wasm_exec_env_t _env{};
};

void compile(WASMModule* module, const WASMLoadBudget& hooks, std::uint64_t limit, bool profile = true) {
   std::array<char, 512> diagnostic{};
   const bool success =
       profile ? wasm_loader_compile_llvm_jit_profile_ex(module, &hooks, limit, diagnostic.data(), diagnostic.size())
               : wasm_loader_compile_llvm_jit_ex(module, &hooks, limit, diagnostic.data(), diagnostic.size());
   if (!success)
      throw std::runtime_error(std::string("Profile loader compile: ") + diagnostic.data());
}
void no_map(const WASMModule* module) {
   std::uint32_t count = UINT32_MAX;
   require(!rows(module, count) && !count, "Unpublished/disabled profile returned native intervals");
   require(translate(module, 0) == UINT32_MAX && translate(module, UINTPTR_MAX) == UINT32_MAX &&
               translate(module, reinterpret_cast<std::uintptr_t>(&no_map)) == UINT32_MAX,
           "Unpublished/disabled map classified an unknown PC");
}
std::set<std::uint32_t> validate_map(const WASMModule* module, const instruction_profile_fixture::image& fixture) {
   std::uint32_t count{};
   const auto* map = rows(module, count);
   require(map && count > 1, "Native map is absent or function-only");
   std::set<std::uint32_t> offsets;
   for (std::uint32_t i = 0; i < count; ++i) {
      const auto& row = map[i];
      require(row.begin < row.end && (!i || map[i - 1].end <= row.begin), "Native intervals overlap or wrap");
      require(std::find(fixture.opcodes.begin(), fixture.opcodes.end(), row.wasm_offset) != fixture.opcodes.end(),
              "Map offset is not an original-file WASM opcode");
      require(row.wasm_offset >= fixture.code_section_offset, "Map returned a Code-relative offset");
      require(translate(module, row.begin) == row.wasm_offset && translate(module, row.end - 1) == row.wasm_offset,
              "Native range lookup lost an instruction boundary");
      if (i && map[i - 1].end < row.begin)
         require(translate(module, map[i - 1].end) == UINT32_MAX, "Native gap inherited its preceding row");
      offsets.insert(row.wasm_offset);
      std::cout << "native=[0x" << std::hex << row.begin << ",0x" << row.end << ") wasm=0x" << row.wasm_offset
                << std::dec << '\n';
   }
   std::size_t first_function_offsets{};
   for (const auto offset : fixture.function_opcodes.front())
      first_function_offsets += offsets.contains(offset);
   require(first_function_offsets >= 3, "Map does not distinguish same-function instructions");
   for (const auto& function : fixture.function_opcodes)
      require(std::any_of(function.begin(), function.end(), [&](auto offset) { return offsets.contains(offset); }),
              "A guest function has no original-file instruction locations");
   for (std::uint32_t i = 0; i < module->function_count; ++i) {
      require(module->func_ptrs[i] && module->func_ptrs_compiled[i], "Native function table is incomplete");
      require(translate(module, reinterpret_cast<std::uintptr_t>(module->func_ptrs[i])) == UINT32_MAX,
              "Native wrapper prologue was attributed to WASM");
      LLVMOrcExecutorAddress body{};
      const auto name = std::string(AOT_FUNC_INTERNAL_PREFIX) + std::to_string(i);
      checked(LLVMOrcLLLazyJITLookup(module->comp_ctx->orc_jit, &body, name.c_str()));
      require(body && translate(module, body) == UINT32_MAX, "Guest body prologue was attributed to WASM");
   }
   require(translate(module, reinterpret_cast<std::uintptr_t>(&validate_map)) == UINT32_MAX &&
               translate(module, 0) == UINT32_MAX && translate(module, UINTPTR_MAX) == UINT32_MAX &&
               translate(module, map[count - 1].end) == UINT32_MAX,
           "Host/outside PC was attributed to WASM");
   return offsets;
}

void positive(bool multiple, bool sample) {
   const auto fixture = instruction_profile_fixture::stripped(multiple);
   const auto unchanged = fixture.wasm;
   auto runtime = vm::engine_runtime::acquire();
   const vm::engine_thread_scope thread;
   auto budget = vm::engine_load_budget::create(16 * 1024 * 1024);
   const auto root = budget->current_bytes();
   observed_budget observed{budget, refusal::none};
   {
      const vm::engine_load_budget::scope scope{budget};
      vm::engine_module source{runtime, fixture.wasm, {}, true, false, budget};
      observed.attach(source);
      auto* module = reinterpret_cast<WASMModule*>(source.get());
      require(module->module_type == Wasm_Module_Bytecode && !module->comp_ctx && !module->func_ptrs,
              "Source admission compiled native code");
      no_map(module);
      const auto hooks = observed.callbacks();
      compile(module, hooks, budget->limit_bytes());
      require(!observed.published_during_compile, "Partial profile escaped before loader publication");
      require(observed.inspected_noinline && observed.kept_noinline,
              "Profile compilation weakened the native frame/noinline contract");
      const auto offsets = validate_map(module, fixture);
      const auto retained = budget->current_bytes();
      {
         observed_budget other_observed{budget, refusal::none};
         auto other = std::make_unique<vm::engine_module>(runtime, fixture.wasm, nullptr, true, false, budget);
         other_observed.attach(*other);
         auto* second = reinterpret_cast<WASMModule*>(other->get());
         const auto other_hooks = other_observed.callbacks();
         compile(second, other_hooks, budget->limit_bytes());
         std::uint32_t first_count{}, second_count{};
         const auto* first_rows = rows(module, first_count);
         const auto* second_rows = rows(second, second_count);
         require(first_count && second_count && translate(module, second_rows[0].begin) == UINT32_MAX &&
                     translate(second, first_rows[0].begin) == UINT32_MAX,
                 "Different native objects shared an arena-based instruction map");
         // Registered-thread teardown frees owner-prefixed records, without
         // changing the live object's map or depending on caller TLS.
         std::exception_ptr thread_error;
         std::thread cleanup([&] {
            try {
               const vm::engine_thread_scope registered;
               other.reset();
            } catch (...) {
               thread_error = std::current_exception();
            }
         });
         cleanup.join();
         if (thread_error)
            std::rethrow_exception(thread_error);
      }
      require(budget->retry_cleanup() && budget->current_bytes() == retained,
              "Independent object teardown retained owner storage");
      validate_map(module, fixture);
      {
         instance execution{source.get()};
         require(execution.invoke("mix", 130) == instruction_profile_fixture::expected(130),
                 "Genuine LLVM JIT execution returned the wrong result");
         if (multiple)
            require(execution.invoke("outer", 130) == instruction_profile_fixture::expected(130) / 130,
                    "Cross-function native execution returned the wrong result");
         if (sample) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            std::uint32_t result{};
            {
               sampler samples{module->comp_ctx};
               do
                  result ^= execution.invoke("mix", 4000000);
               while (std::chrono::steady_clock::now() < deadline);
            }
            std::atomic_signal_fence(std::memory_order_seq_cst);
            std::set<std::uint32_t> sampled;
            for (std::sig_atomic_t i = 0; i < sample_count; ++i) {
               if (sampled_offsets[i] == UINT32_MAX)
                  continue;
               require(offsets.contains(sampled_offsets[i]) && translate(module, sampled_pcs[i]) == sampled_offsets[i],
                       "Signal-time translation disagrees with the published map");
               sampled.insert(sampled_offsets[i]);
            }
            require(sampled.size() >= 2, "Native samples did not distinguish same-function WASM instructions");
            std::cout << "samples=" << sample_count << " sampled_offsets=" << sampled.size()
                      << " native_result=" << result << '\n';
         }
      }
      require(budget->peak_bytes() <= budget->limit_bytes(), "Owned profiling storage escaped its bound");
   }
   require(budget->retry_cleanup() && budget->current_bytes() == root, "Module unload retained profile storage");
   require(fixture.wasm == unchanged, "Profiling mutated the caller's WASM bytes");
}

void failure(refusal point) {
   const auto fixture = instruction_profile_fixture::stripped();
   const auto unchanged = fixture.wasm;
   auto runtime = vm::engine_runtime::acquire();
   const vm::engine_thread_scope thread;
   vm::engine_load_budget::faults faults;
   if (point == refusal::mapping)
      faults.mapping = 1;
   auto budget = vm::engine_load_budget::create(16 * 1024 * 1024, {}, faults);
   const auto root = budget->current_bytes();
   observed_budget observed{budget, point};
   {
      const vm::engine_load_budget::scope scope{budget};
      vm::engine_module source{runtime, fixture.wasm, {}, true, false, budget};
      observed.attach(source);
      auto* module = reinterpret_cast<WASMModule*>(source.get());
      const auto baseline = budget->current_bytes();
      const auto hooks = observed.callbacks();
      std::array<char, 512> diagnostic{};
      require(!wasm_loader_compile_llvm_jit_profile_ex(module, &hooks, budget->limit_bytes(), diagnostic.data(),
                                                       diagnostic.size()),
              "Injected owner refusal published native code");
      require(point == refusal::mapping ? observed.mapping_attempts != 0 : observed.triggered,
              "The requested failure point was not reached");
      if (point == refusal::early_heap || point == refusal::early_cancel)
         require(!observed.mapping_attempts, "Early metadata refusal reached native mapping/finalization");
      require(!observed.published_during_compile, "Failed preparation exposed a partial map");
      require(!module->comp_ctx && !module->comp_data && !module->func_ptrs && !module->func_ptrs_compiled,
              "Failed compilation retained executable state");
      no_map(module);
      const auto expected = point == refusal::early_heap ? vm::engine_load_budget::failure::limit
                            : point == refusal::mapping  ? vm::engine_load_budget::failure::allocation
                                                         : vm::engine_load_budget::failure::cancelled;
      require(budget->status() == expected, "Refusal lost its typed owner category");
      observed.release_reservation();
      require(budget->retry_cleanup() && budget->current_bytes() == baseline,
              "Failed construction did not refund the complete owner charge");
      require(!wasm_loader_compile_llvm_jit_profile_ex(module, &hooks, budget->limit_bytes(), diagnostic.data(),
                                                       diagnostic.size()),
              "Retry ignored an unavailable owner");
      no_map(module);
      require(budget->current_bytes() == baseline, "Rejected retry retained owner storage");
      std::cout << "refusal=" << static_cast<int>(point) << " diagnostic=" << diagnostic.data() << '\n';
   }
   require(budget->current_bytes() == root, "Failed module unload retained owner storage");
   require(fixture.wasm == unchanged, "Failure/retry mutated the caller WASM bytes");
   positive(false, false);
}

void metadata_validation() {
   using line_table = llvm::DWARFDebugLine::LineTable;
   // These are LLVM-owned parsed structures, not a second DWARF decoder or
   // corrupted-emission injection. Production extract uses this same predicate.
   const std::array<NativeRange, 3> ranges{{{100, 200}, {300, 400}, {700, 800}}};
   const auto append_sequence = [](line_table& table, std::uint64_t begin, std::uint64_t end) {
      llvm::DWARFDebugLine::Row first;
      first.Address.Address = begin;
      first.Line = 0;
      auto last = first;
      last.Address.Address = end;
      last.EndSequence = true;
      llvm::DWARFDebugLine::Sequence sequence;
      sequence.LowPC = begin;
      sequence.HighPC = end;
      sequence.FirstRowIndex = static_cast<unsigned>(table.Rows.size());
      sequence.LastRowIndex = sequence.FirstRowIndex + 2;
      sequence.Empty = false;
      table.appendRow(first);
      table.appendRow(last);
      table.appendSequence(sequence);
   };
   const auto reset_table = [&](line_table& table, std::uint64_t begin = 100, std::uint64_t end = 200) {
      table.clear();
      append_sequence(table, begin, end);
   };
   unsigned negatives = 0;
   const auto reject = [&](const line_table* table) {
      ProfileMetadata metadata;
      require(!metadata.accept_unit(table, ranges), "Incomplete parsed metadata was accepted");
      require(!metadata.complete(), "Rejected parsed metadata completed extraction");
      ++negatives;
   };
   ProfileMetadata no_units;
   require(!no_units.complete(), "Zero compile units completed extraction");
   ++negatives;
   reject(nullptr);
   line_table table;
   reset_table(table);
   table.Rows.clear();
   reject(&table);
   reset_table(table);
   table.Rows.pop_back();
   reject(&table);
   reset_table(table);
   table.Sequences.clear();
   reject(&table);
   reset_table(table);
   table.Sequences.front().Empty = true;
   reject(&table);
   reset_table(table);
   table.Sequences.front().HighPC = table.Sequences.front().LowPC;
   reject(&table);
   reset_table(table);
   table.Sequences.front().FirstRowIndex = table.Sequences.front().LastRowIndex;
   reject(&table);
   reset_table(table);
   table.Sequences.front().LastRowIndex = 3;
   reject(&table);
   reset_table(table);
   table.Sequences.front().FirstRowIndex = 1;
   reject(&table);
   reset_table(table);
   table.Rows.back().EndSequence = false;
   reject(&table);
   for (const auto range : std::array<NativeRange, 3>{{{500, 600}, {90, 100}, {400, 500}}}) {
      reset_table(table, range.begin, range.end);
      ProfileMetadata unrelated;
      require(unrelated.accept_unit(&table, ranges), "Unrelated valid sequence was malformed");
      require(!unrelated.complete(), "No guest sequence overlap completed extraction");
      ++negatives;
   }
   reset_table(table);
   ProfileMetadata sticky_failure;
   require(sticky_failure.accept_unit(&table, ranges) && sticky_failure.complete(),
           "Valid initial table did not complete");
   require(!sticky_failure.accept_unit(nullptr, ranges) && !sticky_failure.complete(),
           "A later rejected unit retained completion");
   require(!sticky_failure.accept_unit(&table, ranges) && !sticky_failure.complete(),
           "A later valid unit erased a prior rejection");
   ++negatives;

   unsigned positives = 0;
   ProfileMetadata line_zero;
   require(line_zero.accept_unit(&table, ranges) && line_zero.complete(),
           "Complete line-zero metadata required invented attribution");
   ++positives;
   table.Rows.front().Line = 38;
   ProfileMetadata attributed;
   require(attributed.accept_unit(&table, ranges) && attributed.complete(),
           "Positive source lines changed the structural predicate");
   ++positives;
   reset_table(table, 300, 400);
   append_sequence(table, 500, 600);
   append_sequence(table, 100, 200);
   ProfileMetadata unsorted;
   require(unsorted.accept_unit(&table, ranges) && unsorted.complete(),
           "Unsorted sequences, gaps or an unmapped function required fabricated coverage");
   ++positives;
   line_table unrelated_table;
   reset_table(unrelated_table, 500, 600);
   ProfileMetadata multiple_units;
   require(multiple_units.accept_unit(&unrelated_table, ranges) && !multiple_units.complete(),
           "A non-guest unit completed extraction before a guest sequence");
   require(multiple_units.accept_unit(&table, ranges) && multiple_units.complete(),
           "Global guest overlap was incorrectly required for every unit");
   ++positives;
   std::cout << "metadata_predicate_negatives=" << negatives << " positives=" << positives
             << " corrupted_emission_end_to_end=NOT_RUN\n";
}

void empty_or_plain(bool empty) {
   const auto fixture = instruction_profile_fixture::stripped();
   std::vector<fixtures::bytes> inputs;
   if (empty) {
      inputs.push_back({0, 0x61, 0x73, 0x6d, 1, 0, 0, 0});
      auto imported = inputs.front();
      fixtures::section(imported, 1, {1, 0x60, 0, 0});
      fixtures::bytes imports{1};
      fixtures::text(imports, "env");
      fixtures::text(imports, "external");
      imports.insert(imports.end(), {0, 0});
      fixtures::section(imported, 2, imports);
      inputs.push_back(std::move(imported));
   } else
      inputs.push_back(fixture.wasm);
   auto runtime = vm::engine_runtime::acquire();
   const vm::engine_thread_scope thread;
   auto budget = vm::engine_load_budget::create(16 * 1024 * 1024);
   const auto root = budget->current_bytes();
   for (const auto& code : inputs) {
      const auto unchanged = code;
      const vm::engine_load_budget::scope scope{budget};
      vm::engine_module source{runtime, code, {}, true, false, budget};
      auto* module = reinterpret_cast<WASMModule*>(source.get());
      no_map(module);
      const auto hooks = source.native_load_budget();
      compile(module, hooks, budget->limit_bytes(), empty);
      no_map(module);
      if (empty) {
         require(!module->comp_ctx && !module->comp_data && !module->func_ptrs,
                 "Empty compile created an unnecessary compiler owner");
         compile(module, hooks, budget->limit_bytes());
         no_map(module);
      } else {
         require(module->comp_ctx && !module->comp_ctx->jit_instruction_profile,
                 "Plain JIT silently enabled instruction profiling");
         instance execution{source.get()};
         require(execution.invoke("mix", 130) == instruction_profile_fixture::expected(130),
                 "Plain JIT control execution failed");
      }
      require(code == unchanged, "Plain/empty compile mutated source bytes");
   }
   require(budget->retry_cleanup() && budget->current_bytes() == root, "Plain/empty compile retained owner storage");
}
} // namespace

int main(int argc, char** argv) {
   try {
#ifdef FORGE_WAMR_PROFILE_PROOF_IDENTITY
      std::cout << "candidate_identity=" << FORGE_WAMR_PROFILE_PROOF_IDENTITY << '\n';
#endif
      const std::string_view scenario = argc > 1 ? argv[1] : "runtime";
      if (scenario == "runtime")
         positive(false, true);
      else if (scenario == "multi")
         positive(true, false);
      else if (scenario == "empty")
         empty_or_plain(true);
      else if (scenario == "plain")
         empty_or_plain(false);
      else if (scenario == "early_heap")
         failure(refusal::early_heap);
      else if (scenario == "early_cancel")
         failure(refusal::early_cancel);
      else if (scenario == "late_cancel")
         failure(refusal::late_cancel);
      else if (scenario == "mapping")
         failure(refusal::mapping);
      else if (scenario == "metadata")
         metadata_validation();
      else
         throw std::invalid_argument("Unknown instruction-profile scenario");
      std::cout << "Runtime instruction profile " << scenario << " PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "Runtime instruction profile FAILED: " << error.what() << '\n';
      return 1;
   }
}
