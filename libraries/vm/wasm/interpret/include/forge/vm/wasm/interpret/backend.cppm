module;

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
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
import :signals;
#if defined(__x86_64__)
import :x86_64;
#endif

export namespace forge::vm::wasm::interpret {

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
      detail::check<exceptions::interpreter>((mod->error == nullptr), mod->error);
      detail::validate_data_segments(*mod);
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
      ctx->set_max_call_depth(detail::get_max_call_depth(new_options));
      ctx->set_max_pages(detail::get_max_pages(new_options));
      initialize(host);
      return *this;
   }

   inline backend& initialize(host_t* host = nullptr) {
      initialized = false;
      ctx->reset();
      ctx->execute_start(host, interpret_visitor(*ctx));
      initialized = true;
      return *this;
   }

   inline backend& initialize(host_t& host) {
      return initialize(&host);
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
      // timed_run_has_timed_out -- declared in signal handling code because signal handler needs to inspect it on a
      // SEGV too -- is a thread local
      //  so that upon a SEGV the signal handling code can discern if the thread that caused the SEGV has a timed_run
      //  that has timed out. This thread local also need to be an atomic because the thread that a Watchdog callback
      //  will be called from may not be the same as the executing thread.
      std::atomic<bool>& _timed_out = timed_run_has_timed_out;
      std::atomic<std::size_t>& _timeout_count = timed_run_timeout_count;
      auto local_timed_out = std::atomic<bool>{false};
      auto reenable_code = scope_guard{[&]() {
         if (local_timed_out.load(std::memory_order_acquire)) {
            mod->allocator.enable_code(Impl::is_jit);
            const auto previous = _timeout_count.fetch_sub(1, std::memory_order_acq_rel);
            assert(previous > 0U);
            _timed_out.store(previous > 1U, std::memory_order_release);
         }
      }};
      try {
         {
            auto wd_guard =
                std::forward<Watchdog>(wd).scoped_run([this, &_timed_out, &_timeout_count, &local_timed_out]() {
                   local_timed_out.store(true, std::memory_order_release);
                   _timeout_count.fetch_add(1, std::memory_order_acq_rel);
                   _timed_out.store(true, std::memory_order_release);
                   mod->allocator.disable_code();
                });
            std::forward<F>(f)();
         }
         if (local_timed_out.load(std::memory_order_acquire))
            throw exceptions::timeout{"execution timed out"};
      } catch (exceptions::memory&) {
         if (local_timed_out.load(std::memory_order_acquire)) {
            throw exceptions::timeout{"execution timed out"};
         } else {
            throw;
         }
      }
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
   static void require_hostless_call() {
      detail::check<exceptions::interpreter>(std::is_same_v<host_t, standalone_function_t>,
                                             "stateful host instance is required");
   }

   void require_execution_ready() const {
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
};
} // namespace forge::vm::wasm::interpret
