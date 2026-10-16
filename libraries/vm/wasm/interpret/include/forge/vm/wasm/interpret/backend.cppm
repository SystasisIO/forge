module;

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.vm.wasm.interpret.backend;

export import forge.vm.wasm.interpret.allocator;
export import forge.vm.wasm.interpret.debug_info;
export import forge.vm.wasm.interpret.exceptions;
export import forge.vm.wasm.interpret.host_function;
export import forge.vm.wasm.interpret.options;
export import forge.vm.wasm.interpret.types;
export import forge.vm.wasm.interpret.watchdog;
import :bitcode_writer;
import :config;
import :debug_visitor;
import :execution_context;
import :interpret_visitor;
import :null_writer;
import :parser;
export import :signals;
#if defined(__x86_64__)
import :x86_64;
#endif

export namespace forge::vm::wasm::interpret {

// One deterministic FP/comparison/conversion operation over raw numeric slots,
// dispatched through the existing interpreter/SoftFloat implementation.
// Supports f32/f64 comparisons/arithmetic and FP/integer conversions including
// reinterpret; integer-only, constant, memory and control operations reject.
// Bad arity and unsupported operations have distinct typed categories. Existing
// conversion failures retain the interpreter exception category.
// Caller must already be on the host stack with signal recovery NULL. This
// primitive owns only the existing FP environment guard, not trap transport.
expr_value execute_numeric(opcodes operation, std::span<const expr_value> arguments);

} // namespace forge::vm::wasm::interpret

export namespace forge::vm::wasm::interpret {

// Snapshot exactly the traits the canonical parser and execution context use.
// Section-specific fallback values still inherit the source's section limit.
template <typename Options> options resolve_options(const Options& source) {
   auto result = options{};
   result.max_mutable_global_bytes = detail::get_max_mutable_global_bytes(source);
   result.max_table_elements = detail::get_max_table_elements(source);
   result.max_section_elements = detail::get_max_section_elements(source);
   result.max_type_section_elements = detail::get_max_type_section_elements(source);
   result.max_import_section_elements = detail::get_max_import_section_elements(source);
   result.max_function_section_elements = detail::get_max_function_section_elements(source);
   result.max_global_section_elements = detail::get_max_global_section_elements(source);
   result.max_export_section_elements = detail::get_max_export_section_elements(source);
   result.max_element_section_elements = detail::get_max_element_section_elements(source);
   result.max_data_section_elements = detail::get_max_data_section_elements(source);
   result.max_element_segment_elements = detail::get_max_element_segment_elements(source);
   result.max_data_segment_bytes = detail::get_max_data_segment_bytes(source);
   result.max_linear_memory_init = detail::get_max_linear_memory_init(source);
   result.max_func_local_bytes = detail::get_max_func_local_bytes(source);
   result.max_local_sets = detail::get_max_local_sets(source);
   result.max_nested_structures = detail::get_max_nested_structures(source);
   result.max_br_table_elements = detail::get_max_br_table_elements(source);
   result.max_symbol_bytes = detail::get_max_symbol_bytes(source);
   result.max_memory_offset = detail::get_max_memory_offset(source);
   result.max_code_bytes = detail::get_max_code_bytes(source);
   result.max_pages = detail::get_max_pages(source);
   result.max_call_depth = detail::get_max_call_depth(source);
   result.forbid_export_mutable_globals = detail::get_forbid_export_mutable_globals(source);
   result.allow_code_after_function_end = detail::get_allow_code_after_function_end(source);
   result.allow_u32_limits_flags = detail::get_allow_u32_limits_flags(source);
   result.allow_invalid_empty_local_set = detail::get_allow_invalid_empty_local_set(source);
   result.allow_zero_blocktype = detail::get_allow_zero_blocktype(source);
   result.max_func_local_bytes_flags = detail::get_max_func_local_bytes_flags(source);
   result.max_control_depth = detail::get_max_control_depth(source);
   result.max_legacy_nested_structures = detail::get_max_legacy_nested_structures(source);
   result.parse_custom_section_name = detail::get_parse_custom_section_name(source);
   if constexpr (requires { source.max_mutable_global_bytes; })
      result.check_mutable_global_bytes = detail::get_check_mutable_global_bytes(source);
   else
      result.check_mutable_global_bytes = false;
   if constexpr (requires { source.max_func_local_bytes; })
      result.check_func_local_bytes = detail::get_check_func_local_bytes(source);
   else
      result.check_func_local_bytes = false;
   if constexpr (requires { source.max_control_depth; })
      result.check_control_depth = detail::get_check_control_depth(source);
   else
      result.check_control_depth = false;
   if constexpr (requires { source.max_legacy_nested_structures; })
      result.check_legacy_nested_structures = detail::get_check_legacy_nested_structures(source);
   else
      result.check_legacy_nested_structures = false;
   return result;
}

#ifdef __x86_64__
struct jit {
   template <typename Host> using context = jit_execution_context<Host>;
   template <typename Host, typename Options, typename DebugInfo>
   using parser = binary_parser<machine_code_writer<jit_execution_context<Host>>, Options, DebugInfo>;
   static constexpr bool is_jit = true;
};

struct jit_profile {
   template <typename Host> using context = jit_execution_context<Host, true>;
   template <typename Host, typename Options, typename DebugInfo>
   using parser = binary_parser<machine_code_writer<context<Host>>, Options, DebugInfo>;
   static constexpr bool is_jit = true;
};
#endif

struct interpreter {
   template <typename Host> using context = execution_context<Host>;
   template <typename Host, typename Options, typename DebugInfo>
   using parser = binary_parser<bitcode_writer, Options, DebugInfo>;
   static constexpr bool is_jit = false;
};

struct null_backend {
   template <typename Host> using context = null_execution_context<Host>;
   template <typename Host, typename Options, typename DebugInfo>
   using parser = binary_parser<null_writer, Options, DebugInfo>;
   static constexpr bool is_jit = false;
};

template <typename T> struct maybe_unique_ptr {
   maybe_unique_ptr(T* ptr = nullptr, bool owns = true) : ptr(ptr), owns(owns) {}
   maybe_unique_ptr(const maybe_unique_ptr&) = delete;
   maybe_unique_ptr& operator=(const maybe_unique_ptr&) = delete;
   ~maybe_unique_ptr() {
      if (ptr && owns)
         delete ptr;
   }
   T& operator*() const {
      return *ptr;
   }
   T* operator->() const {
      return ptr;
   }
   T* get() const {
      return ptr;
   }
   void reset(T* new_ptr, bool new_owns = true) {
      if (ptr && owns)
         delete ptr;
      this->ptr = new_ptr;
      this->owns = new_owns;
   }

 private:
   T* ptr;

 public:
   bool owns;
};

template <typename HostFunctions = std::nullptr_t, typename Impl = interpreter, typename Options = default_options,
          typename DebugInfo = null_debug_info>
class backend {
   using host_t = detail::host_type_t<HostFunctions>;
   using context_t = typename Impl::template context<HostFunctions>;
   using parser_t = typename Impl::template parser<HostFunctions, Options, DebugInfo>;
   void construct(host_t* host = nullptr) {
      mod->finalize();
      if (ctx.owns) {
         ctx->set_wasm_allocator(memory_alloc);
      }
      // Now data required by JIT is finalized; create JIT module
      // such that memory used in parsing can be released.
      if constexpr (Impl::is_jit) {
         mod->make_jit_module();

         // Important. Release the memory used by parsing.
         mod->allocator.release_base_memory();
      }
      if (ctx.owns) {
         ctx->initialize_globals();
      }
      if constexpr (!std::is_same_v<HostFunctions, std::nullptr_t>)
         HostFunctions::resolve(*mod);
      // FIXME: should not hard code knowledge of null_backend here
      if (ctx.owns) {
         if constexpr (std::is_same_v<Impl, null_backend>) {
            validate();
         } else {
            if (memory_alloc || !ctx->has_linear_memory()) {
               initialize(host);
            }
         }
      }
   }

 public:
   backend() {}

   // Check instantiation constraints without creating a context or executing start.
   // Products with externally owned contexts use this before admitting prepared code.
   void validate() const {
      detail::check<exceptions::interpreter>(mod != nullptr, "backend has no module");
      mod->validate_instantiation();
   }

   backend(wasm_code&& code, host_t& host, wasm_allocator* alloc, const Options& options = Options{})
       : memory_alloc(alloc), mod(std::make_shared<module>()),
         ctx(new context_t{parse_module(code, options), detail::get_max_call_depth(options)}), mod_sharable{true},
         initial_max_call_depth(detail::get_max_call_depth(options)),
         initial_max_pages(detail::get_max_pages(options)) {
      ctx->set_max_pages(initial_max_pages);
      construct(&host);
   }
   backend(wasm_code&& code, wasm_allocator* alloc, const Options& options = Options{})
       : memory_alloc(alloc), mod(std::make_shared<module>()),
         ctx(new context_t{parse_module(code, options), detail::get_max_call_depth(options)}), mod_sharable{true},
         initial_max_call_depth(detail::get_max_call_depth(options)),
         initial_max_pages(detail::get_max_pages(options)) {
      ctx->set_max_pages(initial_max_pages);
      construct();
   }
   backend(wasm_code& code, host_t& host, wasm_allocator* alloc, const Options& options = Options{})
       : memory_alloc(alloc), mod(std::make_shared<module>()),
         ctx(new context_t{parse_module(code, options), detail::get_max_call_depth(options)}), mod_sharable{true},
         initial_max_call_depth(detail::get_max_call_depth(options)),
         initial_max_pages(detail::get_max_pages(options)) {
      ctx->set_max_pages(initial_max_pages);
      construct(&host);
   }
   backend(wasm_code& code, wasm_allocator* alloc, const Options& options = Options{})
       : memory_alloc(alloc), mod(std::make_shared<module>()),
         ctx(new context_t{(parse_module(code, options)), detail::get_max_call_depth(options)}), mod_sharable{true},
         initial_max_call_depth(detail::get_max_call_depth(options)),
         initial_max_pages(detail::get_max_pages(options)) {
      ctx->set_max_pages(initial_max_pages);
      construct();
   }
   backend(wasm_code_ptr& ptr, size_t sz, host_t& host, wasm_allocator* alloc, const Options& options = Options{})
       : memory_alloc(alloc), mod(std::make_shared<module>()),
         ctx(new context_t{parse_module2(ptr, sz, options, true), detail::get_max_call_depth(options)}),
         mod_sharable{true}, initial_max_call_depth(detail::get_max_call_depth(options)),
         initial_max_pages(detail::get_max_pages(options)) { // single parsing. original behavior {
      ctx->set_max_pages(initial_max_pages);
      construct(&host);
   }
   // Single-pass parsing is sufficient when the instantiated module is not cached and for JIT execution.
   // Interpreter integrations may use two-pass parsing to limit memory-mapping pressure. Execution contexts can be
   // supplied and reused by the caller; ctx.owns is false when the backend does not own its context.
   backend(wasm_code_ptr& ptr, size_t sz, wasm_allocator* alloc, const Options& options = Options{},
           bool single_parsing = true, bool exec_ctx_by_backend = true)
       : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(nullptr, exec_ctx_by_backend), mod_sharable{true},
         initial_max_call_depth(detail::get_max_call_depth(options)),
         initial_max_pages(detail::get_max_pages(options)) {
      if (ctx.owns) {
         ctx.reset(new context_t{parse_module2(ptr, sz, options, single_parsing), initial_max_call_depth});
         ctx->set_max_pages(initial_max_pages);
      } else {
         parse_module2(ptr, sz, options, single_parsing);
      }
      construct();
   }

   module& parse_module(wasm_code& code, const Options& options) {
      mod->allocator.use_default_memory();
      return parser_t{mod->allocator, options}.parse_module(code, *mod, debug);
   }

   module& parse_module2(wasm_code_ptr& ptr, size_t sz, const Options& options, bool single_parsing) {
      if (single_parsing) {
         mod->allocator.use_default_memory();
         return parser_t{mod->allocator, options}.parse_module2(ptr, sz, *mod, debug);
      } else {
         // To prevent large number of memory mappings used, two-passes of
         // parsing are performed.
         wasm_code_ptr orig_ptr = ptr;
         size_t largest_size = 0;

         // First pass: finds max size of memory required by parsing.
         {
            // Memory used by this pass is freed when going out of the scope
            ::forge::vm::wasm::interpret::module first_pass_module;
            first_pass_module.allocator.use_default_memory();
            parser_t{first_pass_module.allocator, options}.parse_module2(ptr, sz, first_pass_module, debug);
            first_pass_module.finalize();
            largest_size = first_pass_module.allocator.largest_used_size();
         }

         // Second pass: uses actual required memory for final parsing
         mod->allocator.use_fixed_memory(largest_size);
         return parser_t{mod->allocator, options}.parse_module2(orig_ptr, sz, *mod, debug);
      }
   }

   // Shares compiled module with another backend which never compiles
   // module itself.
   void share(const backend& from) {
      detail::check<exceptions::busy>(!from._invocation_active.test_and_set(std::memory_order_acquire),
                                      "cannot share an active invocation");
      auto source_admission = scope_guard{[&]() { from._invocation_active.clear(std::memory_order_release); }};
      detail::check<exceptions::busy>(!_invocation_active.test_and_set(std::memory_order_acquire),
                                      "cannot replace an active invocation");
      auto target_admission = scope_guard{[&]() { _invocation_active.clear(std::memory_order_release); }};
      detail::check<exceptions::unusable>(!from._invocation_unusable && !_invocation_unusable,
                                          "cannot share a discarded backend");
      assert(from.mod_sharable); // `from` backend's mod is sharable
      assert(!mod_sharable);     // `to` backend's mod must not be sharable
      mod = from.mod;
      ctx.owns = false;
      initialized = false;
      initial_max_call_depth = from.initial_max_call_depth;
      initial_max_pages = from.initial_max_pages;
   }

   void set_context(context_t* ctx_ptr) {
      // ctx cannot be set if it is created by the backend
      assert(!ctx.owns);
      ctx.reset(ctx_ptr, false);
      initialized = false;
   }

   inline void reset_max_call_depth() {
      // max_call_depth cannot be reset if ctx is created by the backend
      assert(!ctx.owns);
      ctx->set_max_call_depth(initial_max_call_depth);
   }

   inline void reset_max_pages() {
      // max_pages cannot be reset if ctx is created by the backend
      assert(!ctx.owns);
      ctx->set_max_pages(initial_max_pages);
   }

   template <typename... Args>
   inline auto operator()(host_t& host, const std::string_view& mod, const std::string_view& func, Args... args) {
      return call(host, mod, func, args...);
   }

   template <typename... Args>
   inline bool operator()(const std::string_view& mod, const std::string_view& func, Args... args) {
      return call(mod, func, args...);
   }

   // Only dynamic options matter.  Parser options will be ignored.
   inline backend& initialize(host_t* host, const Options& new_options) {
      detail::check<exceptions::unusable>(!_invocation_unusable,
                                          "backend must be discarded after a host/native failure");
      ctx->set_max_call_depth(detail::get_max_call_depth(new_options));
      ctx->set_max_pages(detail::get_max_pages(new_options));
      initialize(host);
      return *this;
   }

   inline backend& initialize(host_t* host = nullptr) {
      detail::check<exceptions::unusable>(!_invocation_unusable,
                                          "backend must be discarded after a host/native failure");
      initialized = false;
      ctx->reset();
      ctx->execute_start(host, interpret_visitor(*ctx));
      initialized = true;
      return *this;
   }

   inline backend& initialize(host_t& host) {
      return initialize(&host);
   }

   template <typename F>
   void with_invocation(host_t* host, const Options& options, std::chrono::steady_clock::duration budget,
                        std::stop_token stop, F&& body) {
      // Retire all admission/context/memory RAII before the existing adapter
      // transports an error to any ancestor guest recovery checkpoint.
      ::forge::vm::wasm::interpret::longjmp_on_exception([&] {
         static_assert(!std::is_same_v<Impl, null_backend>, "invocation requires an executing backend");
         // A one-shot per-instance try-lock also rejects reentrant calls on the invoking thread.
         detail::check<exceptions::busy>(!_invocation_active.test_and_set(std::memory_order_acquire),
                                         "backend invocation is already active");
         auto admission = scope_guard{[&]() { _invocation_active.clear(std::memory_order_release); }};
         detail::check<exceptions::unusable>(!_invocation_unusable,
                                             "backend must be discarded after a host/native failure");
         detail::check<exceptions::invocation_state>(
             mod != nullptr && !ctx.owns && ctx.get() == nullptr && memory_alloc == nullptr,
             "invocation requires a prepared backend with no attached context or memory");
         detail::check<exceptions::shared_instance>(mod.use_count() == 1,
                                                    "invocation requires exclusive compiled module ownership");
         if constexpr (!std::is_same_v<host_t, standalone_function_t>)
            detail::check<exceptions::invocation_state>(host != nullptr,
                                                        "stateful invocation requires a host instance");
         detail::check<exceptions::canceled>(!stop.stop_requested(), "execution canceled");
         detail::check<exceptions::timeout>(budget > std::chrono::steady_clock::duration::zero(),
                                            "execution timed out");
         validate();

         auto memory = wasm_allocator{};
         auto context = context_t{*mod, detail::get_max_call_depth(options)};
         auto detach = scope_guard{[&]() {
            initialized = false;
            ctx.reset(nullptr, false);
            memory_alloc = nullptr;
            context.set_wasm_allocator(nullptr);
         }};
         ctx.reset(&context, false);
         memory_alloc = &memory;
         context.set_wasm_allocator(&memory);
         try {
            // Require pre-entry admission without changing the upstream legacy
            // same-mapping nesting contract; absolute typed scopes stay exclusive.
            timed_run_impl<false, true>(watchdog{budget, stop}, [&]() {
               detail::check<exceptions::canceled>(!stop.stop_requested(), "execution canceled");
               initialize(host, options);
               detail::check<exceptions::canceled>(!stop.stop_requested(), "execution canceled");
               std::invoke(std::forward<F>(body));
            });
            if (context.has_host_failure()) {
               _invocation_unusable = true;
               if (context.host_failure())
                  std::rethrow_exception(context.host_failure());
               detail::fail<exceptions::unusable>("native failure crossed a host callback");
            }
         } catch (const detail::code_interruption&) {
            // Let the ancestor timed owner interpret its private transport.
            // A separately recorded actual host failure still forbids reuse.
            if (context.has_host_failure())
               _invocation_unusable = true;
            throw;
         } catch (...) {
            // A failed code protection transition outranks a racing host exception.
            if (_code_protection_failure)
               std::rethrow_exception(_code_protection_failure);
            // Host exceptions retain their original type, even if a deadline raced with them.
            if (context.has_host_failure()) {
               _invocation_unusable = true;
               if (context.host_failure())
                  std::rethrow_exception(context.host_failure());
            } else if (!is_reusable_guest_failure(std::current_exception())) {
               _invocation_unusable = true;
            }
            throw;
         }
      });
   }

   template <typename... Args> inline bool call_indirect(host_t* host, uint32_t func_index, Args&&... args) {
      require_execution_ready();
      if constexpr (Impl::is_jit) {
         ctx->execute_func_table(host, jit_visitor(*ctx), func_index, std::forward<Args>(args)...);
      } else if constexpr (wasm_debug) {
         ctx->execute_func_table(host, debug_visitor(*ctx), func_index, std::forward<Args>(args)...);
      } else {
         ctx->execute_func_table(host, interpret_visitor(*ctx), func_index, std::forward<Args>(args)...);
      }
      return true;
   }

   template <typename... Args> inline bool call(host_t* host, uint32_t func_index, Args&&... args) {
      require_execution_ready();
      if constexpr (Impl::is_jit) {
         ctx->execute(host, jit_visitor(*ctx), func_index, std::forward<Args>(args)...);
      } else if constexpr (wasm_debug) {
         ctx->execute(host, debug_visitor(*ctx), func_index, std::forward<Args>(args)...);
      } else {
         ctx->execute(host, interpret_visitor(*ctx), func_index, std::forward<Args>(args)...);
      }
      return true;
   }

   template <typename... Args>
   inline bool call(host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args) {
      require_execution_ready();
      if constexpr (wasm_debug) {
         ctx->execute(&host, debug_visitor(*ctx), func, std::forward<Args>(args)...);
      } else {
         ctx->execute(&host, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
      }
      return true;
   }

   template <typename... Args>
   inline bool call(const std::string_view& mod, const std::string_view& func, Args&&... args) {
      require_execution_ready();
      require_hostless_call();
      if constexpr (wasm_debug) {
         ctx->execute(nullptr, debug_visitor(*ctx), func, std::forward<Args>(args)...);
      } else {
         ctx->execute(nullptr, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
      }
      return true;
   }

   template <typename... Args>
   inline auto call_with_return(host_t& host, const std::string_view& mod, const std::string_view& func,
                                Args&&... args) {
      require_execution_ready();
      if constexpr (wasm_debug) {
         return ctx->execute(&host, debug_visitor(*ctx), func, std::forward<Args>(args)...);
      } else {
         return ctx->execute(&host, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
      }
   }

   template <typename... Args>
   inline auto call_with_return(const std::string_view& mod, const std::string_view& func, Args&&... args) {
      require_execution_ready();
      require_hostless_call();
      if constexpr (wasm_debug) {
         return ctx->execute(nullptr, debug_visitor(*ctx), func, std::forward<Args>(args)...);
      } else {
         return ctx->execute(nullptr, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
      }
   }

   template <typename Watchdog, typename F> inline void timed_run(Watchdog&& wd, F&& f) {
      timed_run_impl<false>(std::forward<Watchdog>(wd), std::forward<F>(f));
   }

   // Additive neutral interruption. Preexisting cancellation/deadline/external
   // requests never admit Work. Same-mapping nested timed scopes are rejected
   // before entry because page protection cannot represent independent owners.
   template <typename F>
   inline void timed_run(std::chrono::steady_clock::time_point deadline, interrupt_options options, F&& f) {
      timed_run_impl<true>(watchdog{deadline, options}, std::forward<F>(f));
   }

   template <typename Watchdog> inline void execute_all(Watchdog&& wd, host_t& host) {
      require_execution_ready();
      timed_run(std::forward<Watchdog>(wd), [&]() { execute_exports(&host); });
   }

   template <typename Watchdog> inline void execute_all(Watchdog&& wd) {
      require_execution_ready();
      require_hostless_call();
      timed_run(std::forward<Watchdog>(wd), [&]() { execute_exports(nullptr); });
   }

   inline void set_wasm_allocator(wasm_allocator* alloc) {
      memory_alloc = alloc;
      ctx->set_wasm_allocator(memory_alloc);
      initialized = false;
   }

   inline module& get_module() {
      return *mod;
   }
   inline void exit(const std::error_code& ec) {
      ctx->exit(ec);
   }
   inline auto& get_context() {
      return *ctx;
   }

   const DebugInfo& get_debug() const {
      return debug;
   }

 private:
   [[noreturn]] static void throw_interrupt(watchdog::interruption cause) {
      switch (cause) {
      case watchdog::interruption::canceled:
         throw exceptions::canceled{"execution canceled"};
      case watchdog::interruption::interrupted:
         throw exceptions::interrupted{"execution interrupted"};
      case watchdog::interruption::deadline:
         throw exceptions::timeout{"execution timed out"};
      case watchdog::interruption::none:
         std::abort();
      }
      std::abort();
   }

   template <bool Typed, bool Admit = Typed, typename Watchdog, typename F>
   inline void timed_run_impl(Watchdog&& wd, F&& f) {
      detail::check<exceptions::unusable>(!_invocation_unusable,
                                          "backend must be discarded after a host/native failure");
      const std::span<const std::byte> code = mod->allocator.get_code_span();
      for (auto* ancestor = detail::active_timed_run; ancestor; ancestor = ancestor->previous) {
         if ((Typed || ancestor->typed) && !code.empty() && ancestor->code.data() == code.data() &&
             ancestor->code.size() == code.size())
            throw exceptions::execution_unavailable{"overlapping timed scopes share code protection"};
      }
      auto active = detail::timed_run_scope{code, Typed, detail::active_timed_run};
      detail::active_timed_run = &active;
      const auto restore_scope = scope_guard{[&] { detail::active_timed_run = active.previous; }};
      // Callbacks borrow this execution-thread TLS count only until the guard
      // has joined. Restoration may fail; retirement itself never throws.
      auto& timeout_count = timed_run_timeout_count;
      auto cause = std::atomic<watchdog::interruption>{watchdog::interruption::none};
      const auto retire_timeout = scope_guard{[&]() noexcept {
         if (cause.load(std::memory_order_acquire) != watchdog::interruption::none) {
            [[maybe_unused]] const auto previous = timeout_count.fetch_sub(1, std::memory_order_acq_rel);
            assert(previous > 0U);
         }
      }};
      auto execution_failure = std::exception_ptr{};
      try {
         {
            auto guard = std::forward<Watchdog>(wd).scoped_run(
                [this, &cause,
                 &timeout_count](watchdog::interruption reason = watchdog::interruption::deadline) noexcept {
                   if (reason == watchdog::interruption::none)
                      return;
                   auto expected = watchdog::interruption::none;
                   if (!cause.compare_exchange_strong(expected, reason, std::memory_order_acq_rel))
                      return; // A custom watchdog cannot increment this owner twice.
                   timeout_count.fetch_add(1, std::memory_order_acq_rel);
                   try {
                      mod->allocator.disable_code();
                   } catch (...) {
                      // Unchanged fail-fast boundary: an endless guest cannot
                      // be safely interrupted without protecting its code.
                      std::terminate();
                   }
                });
            if constexpr (Admit) {
               if (guard.enter())
                  std::forward<F>(f)();
            } else {
               std::forward<F>(f)();
            }
         }
      } catch (...) {
         execution_failure = std::current_exception();
      }
      // The callback is terminal before checked restoration. Never throw from
      // a scope_guard destructor or lose a nested sticky restoration failure.
      const auto fired = cause.load(std::memory_order_acquire);
      if (fired != watchdog::interruption::none) {
         try {
            mod->allocator.enable_code(Impl::is_jit);
         } catch (...) {
            _invocation_unusable = true;
            if (!_code_protection_failure)
               _code_protection_failure = std::current_exception();
         }
      }
      if (_code_protection_failure)
         std::rethrow_exception(_code_protection_failure);
      if (execution_failure) {
         try {
            std::rethrow_exception(execution_failure);
         } catch (const detail::code_interruption& fault) {
            // Both APIs route private transport to its exact code owner; a
            // nested legacy scope must not steal an ancestor's interruption.
            if (fired != watchdog::interruption::none && fault.code.data() == code.data() &&
                fault.code.size() == code.size())
               throw_interrupt(fired);
            throw;
         } catch (const exceptions::memory&) {
            // Only the legacy API retains donor generic memory/timeout mapping.
            if constexpr (!Typed) {
               if (fired != watchdog::interruption::none)
                  throw_interrupt(fired);
            }
            throw;
         }
      }
      if (fired != watchdog::interruption::none)
         throw_interrupt(fired);
   }

   static bool is_reusable_guest_failure(const std::exception_ptr& error) {
      try {
         std::rethrow_exception(error);
      } catch (const exceptions::interpreter&) {
      } catch (const exceptions::memory&) {
      } catch (const exceptions::stack_memory&) {
      } catch (const exceptions::pointer_out_of_bounds&) {
      } catch (const exceptions::invalid_element&) {
      } catch (const exceptions::timeout&) {
      } catch (const exceptions::canceled&) {
      } catch (const exceptions::interrupted&) {
      } catch (const exceptions::exit&) {
      } catch (...) {
         return false;
      }
      return true;
   }

   static void require_hostless_call() {
      detail::check<exceptions::interpreter>(std::is_same_v<host_t, standalone_function_t>,
                                             "stateful host instance is required");
   }

   void require_execution_ready() const {
      detail::check<exceptions::interpreter>(ctx.get() != nullptr, "backend has no execution context");
      detail::check<exceptions::unusable>(!_invocation_unusable,
                                          "backend must be discarded after a host/native failure");
      detail::check<exceptions::allocation>((memory_alloc != nullptr || !ctx->has_linear_memory()),
                                            "linear memory requires an allocator");
      detail::check<exceptions::interpreter>(initialized, "backend is not initialized");
   }

   void execute_exports(host_t* host) {
      const auto execute = [&](const auto& exports) {
         for (std::size_t index = 0; index < exports.size(); ++index) {
            const auto& entry = exports[index];
            if (entry.kind == external_kind::Function) {
               const auto name =
                   std::string{reinterpret_cast<const char*>(entry.field_str.data()), entry.field_str.size()};
               if constexpr (Impl::is_jit)
                  ctx->execute(host, jit_visitor(*ctx), name);
               else
                  ctx->execute(host, interpret_visitor(*ctx), name);
            }
         }
      };

      if constexpr (Impl::is_jit)
         execute(mod->jit_mod->exports);
      else
         execute(mod->exports);
   }

   wasm_allocator* memory_alloc = nullptr; // non owning pointer
   std::shared_ptr<module> mod = nullptr;
   DebugInfo debug;
   maybe_unique_ptr<context_t> ctx = nullptr;
   bool mod_sharable = false; // true if mod is sharable (compiled by the backend)
   bool initialized = false;
   uint32_t initial_max_call_depth = 0;
   uint32_t initial_max_pages = 0;
   mutable std::atomic_flag _invocation_active = ATOMIC_FLAG_INIT;
   bool _invocation_unusable = false;
   std::exception_ptr _code_protection_failure;
};
} // namespace forge::vm::wasm::interpret
