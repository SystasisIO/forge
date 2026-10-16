module;

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <forge/vm/wasm/interpret/opcode_macros.hpp>
#include <iostream>
#include <limits>
#include <optional>
#include <signal.h>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <ucontext.h>
#include <utility>
#include <vector>

export module forge.vm.wasm.interpret.backend:execution_context;

import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.debug_info;
import forge.vm.wasm.interpret.exceptions;
import forge.vm.wasm.interpret.floating_point_environment;
import forge.vm.wasm.interpret.host_function;
import forge.vm.wasm.interpret.options;
import forge.vm.wasm.interpret.types;
import forge.vm.wasm.interpret.watchdog;
import :interpret_visitor;
import :signals;

// OSX requires _XOPEN_SOURCE to #include <ucontext.h>
#ifdef __APPLE__
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

namespace forge::vm::wasm::interpret {

struct null_host_functions {
   template <typename... A> void operator()(A&&...) const {
      detail::fail<exceptions::interpreter>("Should never get here because it's impossible to link a module "
                                            "that imports any host functions, when no host functions are available");
   }
};

namespace detail {
template <typename HostFunctions> struct host_type {
   using type = typename HostFunctions::host_type_t;
};
template <> struct host_type<std::nullptr_t> {
   using type = std::nullptr_t;
};

template <typename HF> using host_type_t = typename host_type<HF>::type;

template <typename HostFunctions> struct type_converter {
   using type = typename HostFunctions::type_converter_t;
};
template <> struct type_converter<std::nullptr_t> {
   using type =
       forge::vm::wasm::interpret::type_converter<std::nullptr_t, forge::vm::wasm::interpret::execution_interface>;
};

template <typename HF> using type_converter_t = typename type_converter<HF>::type;

template <typename HostFunctions> struct host_invoker {
   using type = HostFunctions;
};
template <> struct host_invoker<std::nullptr_t> {
   using type = null_host_functions;
};
template <typename HF> using host_invoker_t = typename host_invoker<HF>::type;

} // namespace detail

template <typename Derived, typename Host, bool IsJit> class execution_context_base {
   using host_type = detail::host_type_t<Host>;

 public:
   Derived& derived() {
      return static_cast<Derived&>(*this);
   }
   execution_context_base() {}
   execution_context_base(module* m) : _mod(m) {}

   inline void initialize_globals() {
      if constexpr (IsJit) {
         return initialize_globals_impl(*_mod->jit_mod);
      } else {
         return initialize_globals_impl(*_mod);
      }
   }

   inline bool has_linear_memory() const {
      if constexpr (IsJit) {
         return _mod->jit_mod->memories.size() != 0;
      } else {
         return _mod->memories.size() != 0;
      }
   }

   template <typename Module> inline void initialize_globals_impl(const Module& mod) {
      detail::check<exceptions::memory>((_globals.empty()), "initialize_globals called on non-empty _globals");
      _globals.reserve(mod.globals.size());
      for (uint32_t i = 0; i < mod.globals.size(); i++) {
         _globals.emplace_back(mod.globals[i].init);
      }
   }

   inline int32_t grow_linear_memory(uint32_t pages) {
      if constexpr (IsJit) {
         return grow_linear_memory_impl(*_mod->jit_mod, pages);
      } else {
         return grow_linear_memory_impl(*_mod, pages);
      }
   }

   template <typename Module> inline int32_t grow_linear_memory_impl(const Module& mod, uint32_t pages) {
      const auto size = static_cast<uint32_t>(_wasm_alloc->get_current_page());
      if (!mod.memories.size() || size > _max_pages || pages > _max_pages - size)
         return -1;

      if (mod.memories[0].limits.flags) {
         const auto maximum = mod.memories[0].limits.maximum;
         if (size > maximum || pages > maximum - size)
            return -1;
      }

      _wasm_alloc->alloc<char>(pages);
      return static_cast<int32_t>(size);
   }

   inline int32_t current_linear_memory() const {
      return _wasm_alloc->get_current_page();
   }
   inline void exit(std::error_code err = std::error_code()) {
      // FIXME: system_error?
      _error_code = err;
      throw exceptions::exit{"Exiting"};
   }

   inline void set_module(module* mod) {
      _mod = mod;
   }
   inline module& get_module() {
      return *_mod;
   }
   inline void set_wasm_allocator(wasm_allocator* alloc) {
      _wasm_alloc = alloc;
   }
   inline auto get_wasm_allocator() {
      return _wasm_alloc;
   }
   bool has_host_failure() const noexcept {
      return _host_failure != nullptr || _host_call_active;
   }
   std::exception_ptr host_failure() const noexcept {
      return _host_failure;
   }
   inline char* linear_memory() {
      return _linear_memory;
   }
   inline auto& get_operand_stack() {
      return _os;
   }
   inline const auto& get_operand_stack() const {
      return _os;
   }
   inline auto get_interface() {
      auto memory_size = std::size_t{};
      if (_linear_memory != nullptr && _wasm_alloc != nullptr && _wasm_alloc->get_current_page() >= 0) {
         memory_size = static_cast<std::size_t>(_wasm_alloc->get_current_page()) * page_size;
      }
      return execution_interface{_linear_memory, memory_size, &_os};
   }
   void set_max_pages(std::uint32_t max_pages) {
      _max_pages = std::min(max_pages, static_cast<std::uint32_t>(::forge::vm::wasm::interpret::max_pages));
   }

   inline std::error_code get_error_code() const {
      return _error_code;
   }

   template <typename Module> inline void reset(Module& mod) {
      detail::check<exceptions::interpreter>((_mod->error == nullptr), _mod->error);

      // Reset the capacity of underlying memory used by operand stack if it is
      // greater than initial_stack_size
      _os.reset_capacity();

      if (mod.memories.size()) {
         detail::check<exceptions::allocation>((_wasm_alloc != nullptr), "linear memory requires an allocator");
         _linear_memory = _wasm_alloc->get_base_ptr<char>();
         detail::check<exceptions::allocation>((mod.memories[0].limits.initial <= _max_pages),
                                               "Cannot allocate initial linear memory.");
         _wasm_alloc->reset(mod.memories[0].limits.initial);
      } else if (_wasm_alloc) {
         _linear_memory = _wasm_alloc->get_base_ptr<char>();
         _wasm_alloc->reset();
      } else {
         _linear_memory = nullptr;
      }

      _mod->validate_instantiation();
      for (uint32_t i = 0; i < mod.data.size(); i++) {
         const auto& data_seg = mod.data[i];
         uint32_t offset = data_seg.offset.value.i32; // force to unsigned
         auto addr = _linear_memory + offset;
         if (data_seg.data.size())
            std::memcpy((char*)(addr), data_seg.data.data(), data_seg.data.size());
      }

      // Globals can be different from one WASM code to another.
      // Need to clear _globals at the start of an execution.
      _globals.clear();
      _globals.reserve(mod.globals.size());
      for (uint32_t i = 0; i < mod.globals.size(); i++) {
         _globals.emplace_back(mod.globals[i].init);
      }
   }

   template <typename Visitor, typename... Args>
   inline std::optional<operand_stack_elem> execute(host_type* host, Visitor&& visitor, const std::string_view func,
                                                    Args&&... args) {
      uint32_t func_index = _mod->get_exported_function(func);
      return derived().execute(host, std::forward<Visitor>(visitor), func_index, std::forward<Args>(args)...);
   }

   template <typename Visitor, typename... Args> inline void execute_start(host_type* host, Visitor&& visitor) {
      if (_mod->start != std::numeric_limits<uint32_t>::max())
         derived().execute(host, std::forward<Visitor>(visitor), _mod->start);
   }

 protected:
   inline void invoke_host(host_type* host, std::uint32_t index) {
      // Own all host RAII and error tracking while VM recovery is suspended.
      // Genuine host faults stay foreign/fatal, never a guest recovery jump.
      ::forge::vm::wasm::interpret::longjmp_on_exception([&] {
         const auto environment = floating_point_environment{};
         const auto previous = _host_call_active;
         _host_call_active = true;
         try {
            _rhf(host, get_interface(), index);
         } catch (const detail::code_interruption&) {
            // Private ancestor transport is not a failure of this host.
            _host_call_active = previous;
            throw;
         } catch (...) {
            _host_call_active = previous;
            if (!_host_failure)
               _host_failure = std::current_exception();
            throw;
         }
         _host_call_active = previous;
      });
   }

   template <typename Func_type, typename... Args> static void type_check_args(const Func_type& ft, Args&&...) {
      detail::check<exceptions::interpreter>((sizeof...(Args) == ft.param_types.size()), "wrong number of arguments");
      uint32_t i = 0;
      detail::check<exceptions::interpreter>(
          ((... && (to_wasm_type_v<detail::type_converter_t<Host>, Args> == ft.param_types.at(i++)))),
          "unexpected argument type");
   }

   static void handle_signal(const native_signal_result& result, std::span<const std::byte> code) {
      switch (result.signal) {
      case SIGSEGV:
      case SIGBUS:
      case SIGFPE:
         break;
      default:
         /* TODO fix this */
         assert(!"??????");
      }
      if (result.fault == native_signal_fault::code && !code.empty() &&
          (result.signal == SIGSEGV || result.signal == SIGBUS))
         throw detail::code_interruption{code};
      throw exceptions::memory{"wasm memory out-of-bounds"};
   }

   char* _linear_memory = nullptr;
   module* _mod = nullptr;
   wasm_allocator* _wasm_alloc = nullptr;
   uint32_t _max_pages = max_pages;
   detail::host_invoker_t<Host> _rhf;
   std::error_code _error_code;
   operand_stack _os;
   std::vector<init_expr> _globals;

 private:
   std::exception_ptr _host_failure;
   bool _host_call_active = false;
};

struct jit_visitor {
   template <typename T> jit_visitor(T&&) {}
};

template <typename Host>
class null_execution_context : public execution_context_base<null_execution_context<Host>, Host, false> {
   using base_type = execution_context_base<null_execution_context<Host>, Host, false>;

 public:
   null_execution_context() {}
   null_execution_context(module& m, std::uint32_t max_call_depth) : base_type(&m) {}
};

template <bool EnableBacktrace> struct frame_info_holder {};
template <> struct frame_info_holder<true> {
   void* volatile _bottom_frame = nullptr;
   void* volatile _top_frame = nullptr;
};

template <typename Host, bool EnableBacktrace = false>
class jit_execution_context : public frame_info_holder<EnableBacktrace>,
                              public execution_context_base<jit_execution_context<Host, EnableBacktrace>, Host, true> {
   using base_type = execution_context_base<jit_execution_context<Host, EnableBacktrace>, Host, true>;
   using host_type = detail::host_type_t<Host>;

 public:
   using base_type::_error_code;
   using base_type::_globals;
   using base_type::_mod;
   using base_type::_rhf;
   using base_type::base_type;
   using base_type::execute;
   using base_type::get_interface;
   using base_type::get_operand_stack;
   using base_type::handle_signal;
   using base_type::linear_memory;

   jit_execution_context() {}

   jit_execution_context(module& m, std::uint32_t max_call_depth)
       : base_type(&m), _remaining_call_depth(max_call_depth) {}

   void set_max_call_depth(std::uint32_t max_call_depth) {
      _remaining_call_depth = max_call_depth;
   }

   inline native_value call_host_function(native_value* stack, uint32_t index) {
      const auto& ft = _mod->jit_mod->get_function_type(index);
      uint32_t num_params = ft.param_types.size();
#ifndef NDEBUG
      uint32_t original_operands = get_operand_stack().size();
#endif
      for (uint32_t i = 0; i < ft.param_types.size(); ++i) {
         switch (ft.param_types[i]) {
         case i32:
            get_operand_stack().push(i32_const_t{stack[num_params - i - 1].i32});
            break;
         case i64:
            get_operand_stack().push(i64_const_t{stack[num_params - i - 1].i64});
            break;
         case f32:
            get_operand_stack().push(f32_const_t{stack[num_params - i - 1].f32});
            break;
         case f64:
            get_operand_stack().push(f64_const_t{stack[num_params - i - 1].f64});
            break;
         default:
            assert(!"Unexpected type in param_types.");
         }
      }
      this->invoke_host(_host, _mod->jit_mod->import_functions[index]);
      native_value result{uint64_t{0}};
      // guarantee that the junk bits are zero, to avoid problems.
      auto set_result = [&result](auto val) { std::memcpy(&result, &val, sizeof(val)); };
      if (ft.return_count) {
         operand_stack_elem el = get_operand_stack().pop();
         switch (ft.return_type) {
         case i32:
            set_result(el.to_ui32());
            break;
         case i64:
            set_result(el.to_ui64());
            break;
         case f32:
            set_result(el.to_f32());
            break;
         case f64:
            set_result(el.to_f64());
            break;
         default:
            assert(!"Unexpected function return type.");
         }
      }

      assert(get_operand_stack().size() == original_operands);
      return result;
   }

   inline void reset() {
      base_type::reset(*(_mod->jit_mod));
      get_operand_stack().eat(0);
   }

   template <typename... Args>
   inline std::optional<operand_stack_elem> execute(host_type* host, jit_visitor, uint32_t func_index, Args&&... args) {
      // Only a trivial result carrier may outlive the NULL-recovery callback.
      // Argument/result conversions, saved state, FP scope and typed tails all
      // complete before the adapter restores an active ancestor destination.
      using result_type = std::optional<operand_stack_elem>;
      static_assert(std::is_trivially_copyable_v<result_type>);
      result_type returned;
      ::forge::vm::wasm::interpret::longjmp_on_exception([&] {
         returned = [&]() -> result_type {
            // Save the actual caller state before any custom argument
            // converter can mutate it. Guest entry recanonicalizes below.
            const auto entry_environment = floating_point_environment{};
            const auto imported_functions = _mod->jit_mod->get_imported_functions_size();
            const auto function_count = static_cast<std::size_t>(imported_functions) + _mod->jit_mod->functions.size();
            detail::check<exceptions::interpreter>((static_cast<std::size_t>(func_index) < function_count),
                                                   "cannot execute function, function not found");

            auto saved_host = _host;
            auto saved_os_size = get_operand_stack().size();
            auto g = scope_guard([&]() {
               _host = saved_host;
               get_operand_stack().eat(saved_os_size);
            });

            _host = host;

            const auto& ft = _mod->jit_mod->get_function_type(func_index);
            this->type_check_args(ft, std::forward<Args>(args)...); // args not modified by type_check_args
            native_value result;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
            // Calling execute() with no `args` (i.e. `execute(host_type,jit_visitor,uint32_t)`) results in a "statement
            // has no effect [-Werror=unused-value]" warning on this line. Dissable warning.
            native_value args_raw[] = {transform_arg(std::forward<Args>(args))...};
#pragma GCC diagnostic pop

            const auto environment = floating_point_environment{};
            try {
               if (func_index < imported_functions) {
                  std::reverse(args_raw + 0, args_raw + sizeof...(Args));
                  result = call_host_function(args_raw, func_index);
               } else {
                  detail::check<exceptions::interpreter>(_remaining_call_depth > 0, "stack overflow");
                  std::size_t maximum_stack_usage =
                      (_mod->maximum_stack + 2 /*frame ptr + return ptr*/) * (_remaining_call_depth + 1) +
                      sizeof...(Args) + 4 /* scratch space */;
                  stack_allocator alt_stack(maximum_stack_usage * sizeof(native_value));
                  // reserve 24 bytes for data accessed by inline assembly
                  void* stack = alt_stack.top();
                  if (stack) {
                     stack = static_cast<char*>(stack) - 24;
                  }
                  auto fn = reinterpret_cast<native_value (*)(void*, void*)>(
                      _mod->jit_mod->jit_code_offset[func_index - imported_functions] + _mod->allocator._code_base);

                  if constexpr (EnableBacktrace) {
                     sigset_t block_mask;
                     sigemptyset(&block_mask);
                     sigaddset(&block_mask, SIGPROF);
                     pthread_sigmask(SIG_BLOCK, &block_mask, nullptr);
                     auto restore = scope_guard{[this, &block_mask] {
                        this->_top_frame = nullptr;
                        this->_bottom_frame = nullptr;
                        pthread_sigmask(SIG_UNBLOCK, &block_mask, nullptr);
                     }};

                     ::forge::vm::wasm::interpret::invoke_with_signal_handler(
                         [&]() {
                            result = execute<sizeof...(Args)>(args_raw, fn, this, base_type::linear_memory(), stack);
                         },
                         &handle_signal, _mod->allocator, base_type::get_wasm_allocator());
                  } else {
                     ::forge::vm::wasm::interpret::invoke_with_signal_handler(
                         [&]() {
                            result = execute<sizeof...(Args)>(args_raw, fn, this, base_type::linear_memory(), stack);
                         },
                         &handle_signal, _mod->allocator, base_type::get_wasm_allocator());
                  }
               }
            } catch (exceptions::exit&) {
               return {};
            }

            if (!ft.return_count)
               return {};
            else
               switch (ft.return_type) {
               case i32:
                  return {i32_const_t{result.i32}};
               case i64:
                  return {i64_const_t{result.i64}};
               case f32:
                  return {f32_const_t{result.f32}};
               case f64:
                  return {f64_const_t{result.f64}};
               default:
                  assert(!"Unexpected function return type");
               }
            __builtin_unreachable();
         }();
      });
      return returned;
   }

   template <typename... Args>
   inline std::optional<operand_stack_elem> execute_func_table(host_type* host, jit_visitor visitor,
                                                               uint32_t table_index, Args&&... args) {
      detail::check<exceptions::interpreter>((table_index < _mod->jit_mod->table.size()), "call_indirect out of range");
      return execute(host, visitor, _mod->jit_mod->table[table_index], std::forward<Args>(args)...);
   }

#ifdef __x86_64__
   int backtrace(void** out, int count, void* uc) const {
      static_assert(EnableBacktrace);
      void* end = this->_top_frame;
      if (end == nullptr)
         return 0;
      void* rbp;
      int i = 0;
      if (this->_bottom_frame) {
         rbp = this->_bottom_frame;
      } else if (count != 0) {
         if (uc) {
#ifdef __APPLE__
            auto rip = reinterpret_cast<unsigned char*>(static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rip);
            rbp = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rbp);
            auto rsp = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rsp);
#elif defined __FreeBSD__
            auto rip = reinterpret_cast<unsigned char*>(static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rip);
            rbp = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rbp);
            auto rsp = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rsp);
#else
            auto rip = reinterpret_cast<unsigned char*>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
            rbp = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RBP]);
            auto rsp = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RSP]);
#endif
            out[i++] = rip;
            // If we were interrupted in the function prologue or epilogue,
            // avoid dropping the parent frame.
            auto code_base = reinterpret_cast<const unsigned char*>(_mod->allocator.get_code_start());
            auto code_end = code_base + _mod->allocator._code_size;
            if (rip >= code_base && rip < code_end && count > 1) {
               // function prologue
               if (*reinterpret_cast<const unsigned char*>(rip) == 0x55) {
                  if (rip != *static_cast<void**>(rsp)) { // Ignore fake frame set up for softfloat calls
                     out[i++] = *static_cast<void**>(rsp);
                  }
               } else if (rip[0] == 0x48 && rip[1] == 0x89 && (rip[2] == 0xe5 || rip[2] == 0x27)) {
                  if ((rip - 1) != static_cast<void**>(rsp)[1]) { // Ignore fake frame set up for softfloat calls
                     out[i++] = static_cast<void**>(rsp)[1];
                  }
               }
               // function epilogue
               else if (rip[0] == 0xc3) {
                  out[i++] = *static_cast<void**>(rsp);
               }
            }
         } else {
            rbp = __builtin_frame_address(0);
         }
      }
      while (i < count) {
         void* rip = static_cast<void**>(rbp)[1];
         if (rbp == end)
            break;
         out[i++] = rip;
         rbp = *static_cast<void**>(rbp);
      }
      return i;
   }

   static constexpr bool async_backtrace() {
      return EnableBacktrace;
   }
#endif

   inline int32_t get_global_i32(uint32_t index) {
      return _globals[index].value.i32;
   }

   inline int64_t get_global_i64(uint32_t index) {
      return _globals[index].value.i64;
   }

   inline uint32_t get_global_f32(uint32_t index) {
      return _globals[index].value.f32;
   }

   inline uint64_t get_global_f64(uint32_t index) {
      return _globals[index].value.f64;
   }

   inline void set_global_i32(uint32_t index, int32_t value) {
      _globals[index].value.i32 = value;
   }

   inline void set_global_i64(uint32_t index, int64_t value) {
      _globals[index].value.i64 = value;
   }

   inline void set_global_f32(uint32_t index, uint32_t value) {
      _globals[index].value.f32 = value;
   }

   inline void set_global_f64(uint32_t index, uint64_t value) {
      _globals[index].value.f64 = value;
   }

 protected:
   template <typename T> native_value transform_arg(T&& value) {
      // make sure that the garbage bits are always zero.
      native_value result;
      std::memset(&result, 0, sizeof(result));
      auto tc = detail::type_converter_t<Host>{_host, get_interface()};
      auto transformed_value = detail::resolve_result(tc, std::forward<T>(value)).data;
      std::memcpy(&result, &transformed_value, sizeof(transformed_value));
      return result;
   }

#ifdef __x86_64__
   /* TODO abstract this and clean this up a bit, this really doesn't belong here */
   template <int Count>
   static native_value execute(native_value* data, native_value (*fun)(void*, void*), jit_execution_context* context,
                               void* linear_memory, void* stack) {
      static_assert(sizeof(native_value) == 8, "8-bytes expected for native_value");
      native_value result;
      unsigned stack_check = context->_remaining_call_depth;
      // TODO refactor this whole thing to not need all of this, should be generated from the backend
      // currently ignoring register c++17 warning
      register void* stack_top asm("r12") = stack;
      // 0x1f80 is the default MXCSR value
#define ASM_CODE(before, after)                                                                                        \
   asm volatile(                                                                                                       \
       "test %[stack_top], %[stack_top]; "                                                                             \
       "jnz 3f; "                                                                                                      \
       "mov %%rsp, %[stack_top]; "                                                                                     \
       "sub $0x98, %%rsp; " /* red-zone + 24 bytes*/                                                                   \
       "mov %[stack_top], (%%rsp); "                                                                                   \
       "jmp 4f; "                                                                                                      \
       "3: "                                                                                                           \
       "mov %%rsp, (%[stack_top]); "                                                                                   \
       "mov %[stack_top], %%rsp; "                                                                                     \
       "4: "                                                                                                           \
       "stmxcsr 16(%%rsp); "                                                                                           \
       "mov $0x1f80, %%rax; "                                                                                          \
       "mov %%rax, 8(%%rsp); "                                                                                         \
       "ldmxcsr 8(%%rsp); "                                                                                            \
       "mov %[Count], %%rax; "                                                                                         \
       "test %%rax, %%rax; "                                                                                           \
       "jz 2f; "                                                                                                       \
       "1: "                                                                                                           \
       "movq (%[data]), %%r8; "                                                                                        \
       "lea 8(%[data]), %[data]; "                                                                                     \
       "pushq %%r8; "                                                                                                  \
       "dec %%rax; "                                                                                                   \
       "jnz 1b; "                                                                                                      \
       "2: " before "callq *%[fun]; " after "add %[StackOffset], %%rsp; "                                              \
       "ldmxcsr 16(%%rsp); "                                                                                           \
       "mov (%%rsp), %%rsp; "    /* Force explicit register allocation, because otherwise it's too hard to get the     \
                                    clobbers right. */                                                                 \
       : [result] "=&a"(result), /* output, reused as a scratch register */                                            \
         [data] "+d"(data), [fun] "+c"(fun), [stack_top] "+r"(stack_top) /* input only, but may be clobbered */        \
       : [context] "D"(context), [linear_memory] "S"(linear_memory), [StackOffset] "n"(Count * 8), [Count] "n"(Count), \
         "b"(stack_check)              /* input */                                                                     \
       : "memory", "cc", /* clobber */ /* call clobbered registers, that are not otherwise used */                     \
         /*"rax", "rcx", "rdx", "rsi", "rdi",*/ "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",      \
         "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "mm0", "mm1",   \
         "mm2", "mm3", "mm4", "mm5", "mm6", "mm6", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",         \
         "st(7)");
      if constexpr (!EnableBacktrace) {
         ASM_CODE("", "");
      } else {
         ASM_CODE("movq %%rbp, 8(%[context]); ", "xor %[fun], %[fun]; "
                                                 "mov %[fun], 8(%[context]); ");
      }
#undef ASM_CODE
      return result;
   }
#endif

   host_type* _host = nullptr;
   uint32_t _remaining_call_depth = 0;
};

template <typename Host> class execution_context : public execution_context_base<execution_context<Host>, Host, false> {
   using base_type = execution_context_base<execution_context<Host>, Host, false>;
   using host_type = detail::host_type_t<Host>;

 public:
   using base_type::_error_code;
   using base_type::_globals;
   using base_type::_mod;
   using base_type::_rhf;
   using base_type::get_interface;
   using base_type::get_operand_stack;
   using base_type::handle_signal;
   using base_type::linear_memory;

   execution_context() : base_type(), _halt(exit_t{}) {}

   execution_context(module& m, uint32_t max_call_depth)
       : base_type(&m), _base_allocator{max_call_depth * sizeof(activation_frame)},
         _as{max_call_depth, _base_allocator}, _halt(exit_t{}) {}

   void set_max_call_depth(uint32_t max_call_depth) {
      static_assert(std::is_trivially_move_assignable_v<call_stack>,
                    "This is seriously broken if call_stack move assignment might use the existing memory");
      std::size_t mem_size = max_call_depth * sizeof(activation_frame);
      if (mem_size > _base_allocator.mem_size) {
         _base_allocator = bounded_allocator{mem_size};
         _as = call_stack{max_call_depth, _base_allocator};
      } else if (max_call_depth != _as.capacity()) {
         _base_allocator.index = 0;
         _as = call_stack{max_call_depth, _base_allocator};
      }
   }

   inline void call(uint32_t index) {
      // TODO validate index is valid
      if (index < _mod->get_imported_functions_size()) {
         // TODO validate only importing functions
         const auto& ft = _mod->types[_mod->imports[index].type.func_t];
         type_check(ft);
         inc_pc();
         push_call(activation_frame{nullptr, 0});
         this->invoke_host(_state.host, _mod->import_functions[index]);
         pop_call();
      } else {
         // const auto& ft = _mod->types[_mod->functions[index - _mod->get_imported_functions_size()]];
         // type_check(ft);
         push_call(index);
         setup_locals(index);
         set_pc(_mod->get_function_pc(index));
      }
   }

   void print_stack() {
      std::cout << "STACK { ";
      for (int i = 0; i < get_operand_stack().size(); i++) {
         std::cout << "(" << i << ")";
         visit(overloaded{[&](i32_const_t el) { std::cout << "i32:" << el.data.ui << ", "; },
                          [&](i64_const_t el) { std::cout << "i64:" << el.data.ui << ", "; },
                          [&](f32_const_t el) { std::cout << "f32:" << el.data.f << ", "; },
                          [&](f64_const_t el) { std::cout << "f64:" << el.data.f << ", "; },
                          [&](auto el) { std::cout << "(INDEX " << el.index() << "), "; }},
               get_operand_stack().get(i));
      }
      std::cout << " }\n";
   }

   inline uint32_t table_elem(uint32_t i) {
      detail::check<exceptions::interpreter>((_mod->tables.size() > 0 && i < _mod->tables[0].table.size()),
                                             "call_indirect out of range");
      return _mod->tables[0].table[i];
   }
   inline void push_operand(operand_stack_elem el) {
      get_operand_stack().push(std::move(el));
   }
   inline operand_stack_elem get_operand(uint32_t index) const {
      return get_operand_stack().get(_last_op_index + index);
   }
   inline void eat_operands(uint32_t index) {
      get_operand_stack().eat(index);
   }
   inline void compact_operand(uint32_t index) {
      get_operand_stack().compact(index);
   }
   inline void set_operand(uint32_t index, const operand_stack_elem& el) {
      get_operand_stack().set(_last_op_index + index, el);
   }
   inline uint32_t current_operands_index() const {
      return get_operand_stack().current_index();
   }
   inline void push_call(activation_frame&& el) {
      _as.push(std::move(el));
   }
   inline activation_frame pop_call() {
      return _as.pop();
   }
   inline uint32_t call_depth() const {
      return _as.size();
   }
   template <bool Should_Exit = false> inline void push_call(uint32_t index) {
      opcode* return_pc = static_cast<opcode*>(&_halt);
      if constexpr (!Should_Exit)
         return_pc = _state.pc + 1;

      _as.push(activation_frame{return_pc, _last_op_index});
      _last_op_index = get_operand_stack().size() - _mod->get_function_type(index).param_types.size();
   }

   inline void apply_pop_call(uint32_t num_locals, uint16_t return_count) {
      const auto& af = _as.pop();
      _state.pc = af.pc;
      _last_op_index = af.last_op_index;
      if (return_count)
         compact_operand(get_operand_stack().size() - num_locals - 1);
      else
         eat_operands(get_operand_stack().size() - num_locals);
   }
   inline operand_stack_elem pop_operand() {
      return get_operand_stack().pop();
   }
   inline operand_stack_elem& peek_operand(size_t i = 0) {
      return get_operand_stack().peek(i);
   }
   inline operand_stack_elem get_global(uint32_t index) {
      detail::check<exceptions::interpreter>((index < _mod->globals.size()), "global index out of range");
      detail::check<exceptions::interpreter>((index < _globals.size()),
                                             "index for _globals out of range in get_global for interpreter");
      const auto& gl = _mod->globals[index];
      switch (gl.type.content_type) {
      case types::i32:
         return i32_const_t{_globals[index].value.i32};
      case types::i64:
         return i64_const_t{_globals[index].value.i64};
      case types::f32:
         return f32_const_t{_globals[index].value.f32};
      case types::f64:
         return f64_const_t{_globals[index].value.f64};
      default:
         throw exceptions::interpreter{"invalid global type"};
      }
   }

   inline void set_global(uint32_t index, const operand_stack_elem& el) {
      detail::check<exceptions::interpreter>((index < _mod->globals.size()), "global index out of range");
      detail::check<exceptions::interpreter>((index < _globals.size()), "index for _globals out of range");
      auto& gl = _mod->globals[index];
      detail::check<exceptions::interpreter>((gl.type.mutability), "global is not mutable");
      visit(overloaded{[&](const i32_const_t& i) {
                          detail::check<exceptions::interpreter>((gl.type.content_type == types::i32),
                                                                 "expected i32 global type");
                          _globals[index].value.i32 = i.data.ui;
                       },
                       [&](const i64_const_t& i) {
                          detail::check<exceptions::interpreter>((gl.type.content_type == types::i64),
                                                                 "expected i64 global type");
                          _globals[index].value.i64 = i.data.ui;
                       },
                       [&](const f32_const_t& f) {
                          detail::check<exceptions::interpreter>((gl.type.content_type == types::f32),
                                                                 "expected f32 global type");
                          _globals[index].value.f32 = f.data.ui;
                       },
                       [&](const f64_const_t& f) {
                          detail::check<exceptions::interpreter>((gl.type.content_type == types::f64),
                                                                 "expected f64 global type");
                          _globals[index].value.f64 = f.data.ui;
                       },
                       [](auto) { throw exceptions::interpreter{"invalid global type"}; }},
            el);
   }

   inline bool is_true(const operand_stack_elem& el) {
      bool ret_val = false;
      visit(overloaded{[&](const i32_const_t& i32) { ret_val = i32.data.ui; },
                       [&](auto) { throw exceptions::invalid_element{"should be an i32 type"}; }},
            el);
      return ret_val;
   }

   inline void type_check(const func_type& ft) {
      for (uint32_t i = 0; i < ft.param_types.size(); i++) {
         const auto& op = peek_operand((ft.param_types.size() - 1) - i);
         visit(overloaded{[&](const i32_const_t&) {
                             detail::check<exceptions::interpreter>((ft.param_types[i] == types::i32),
                                                                    "function param type mismatch");
                          },
                          [&](const f32_const_t&) {
                             detail::check<exceptions::interpreter>((ft.param_types[i] == types::f32),
                                                                    "function param type mismatch");
                          },
                          [&](const i64_const_t&) {
                             detail::check<exceptions::interpreter>((ft.param_types[i] == types::i64),
                                                                    "function param type mismatch");
                          },
                          [&](const f64_const_t&) {
                             detail::check<exceptions::interpreter>((ft.param_types[i] == types::f64),
                                                                    "function param type mismatch");
                          },
                          [&](auto) { throw exceptions::interpreter{"function param invalid type"}; }},
               op);
      }
   }

   inline opcode* get_pc() const {
      return _state.pc;
   }
   inline void set_relative_pc(uint32_t pc_offset) {
      _state.pc = _mod->code[0].code + pc_offset;
   }
   inline void set_pc(opcode* pc) {
      _state.pc = pc;
   }
   inline void inc_pc(uint32_t offset = 1) {
      _state.pc += offset;
   }
   inline void exit(std::error_code err = std::error_code()) {
      _error_code = err;
      _state.pc = &_halt;
      _state.exiting = true;
   }

   inline void reset() {
      base_type::reset(*_mod);
      _state = execution_state{};
      get_operand_stack().eat(_state.os_index);
      _as.eat(_state.as_index);
   }

   template <typename Visitor, typename... Args>
   inline std::optional<operand_stack_elem> execute_func_table(host_type* host, Visitor&& visitor, uint32_t table_index,
                                                               Args&&... args) {
      return execute(host, std::forward<Visitor>(visitor), table_elem(table_index), std::forward<Args>(args)...);
   }

   template <typename Visitor, typename... Args>
   inline std::optional<operand_stack_elem> execute(host_type* host, Visitor&& visitor, const std::string_view func,
                                                    Args&&... args) {
      uint32_t func_index = _mod->get_exported_function(func);
      return execute(host, std::forward<Visitor>(visitor), func_index, std::forward<Args>(args)...);
   }

   template <typename Visitor, typename... Args> inline void execute_start(host_type* host, Visitor&& visitor) {
      if (_mod->start != std::numeric_limits<uint32_t>::max())
         execute(host, std::forward<Visitor>(visitor), _mod->start);
   }

   template <typename Visitor, typename... Args>
   inline std::optional<operand_stack_elem> execute(host_type* host, Visitor&& visitor, uint32_t func_index,
                                                    Args&&... args) {
      using result_type = std::optional<operand_stack_elem>;
      static_assert(std::is_trivially_copyable_v<result_type>);
      result_type returned;
      ::forge::vm::wasm::interpret::longjmp_on_exception([&] {
         returned = [&]() -> result_type {
            const auto entry_environment = floating_point_environment{};
            detail::check<exceptions::interpreter>((func_index < _mod->get_functions_total()),
                                                   "cannot execute function, function not found");

            auto last_last_op_index = _last_op_index;

            // save the state of the original calling context
            execution_state saved_state = _state;

            _state.host = host;
            _state.as_index = _as.size();
            _state.os_index = get_operand_stack().size();

            auto cleanup = scope_guard([&]() {
               get_operand_stack().eat(_state.os_index);
               _as.eat(_state.as_index);
               _state = saved_state;

               _last_op_index = last_last_op_index;
            });

            this->type_check_args(_mod->get_function_type(func_index),
                                  std::forward<Args>(args)...); // args not modified
            push_args(std::forward<Args>(args)...);
            push_call<true>(func_index);

            const auto environment = floating_point_environment{};
            if (func_index < _mod->get_imported_functions_size()) {
               this->invoke_host(_state.host, _mod->import_functions[func_index]);
            } else {
               _state.pc = _mod->get_function_pc(func_index);
               setup_locals(func_index);
               ::forge::vm::wasm::interpret::invoke_with_signal_handler(
                   [&]() { execute(std::forward<Visitor>(visitor)); }, &handle_signal, _mod->allocator,
                   base_type::get_wasm_allocator());
            }

            if (_mod->get_function_type(func_index).return_count && !_state.exiting) {
               return pop_operand();
            } else {
               return {};
            }
         }();
      });
      return returned;
   }

   inline void jump(uint32_t pop_info, uint32_t new_pc) {
      set_relative_pc(new_pc);
      if ((pop_info & 0x80000000u)) {
         const auto& op = pop_operand();
         eat_operands(get_operand_stack().size() - ((pop_info & 0x7FFFFFFFu) - 1));
         push_operand(op);
      } else {
         eat_operands(get_operand_stack().size() - pop_info);
      }
   }

   // This isn't async-signal-safe.  Cross fingers and hope for the best.
   // It's only used for profiling.
   int backtrace(void** data, int limit, void* uc) const {
      int out = 0;
      if (limit != 0) {
         data[out++] = _state.pc;
      }
      for (int i = 0; out < limit && i < _as.size(); ++i) {
         data[out++] = _as.get_back(i).pc;
      }
      return out;
   }

 private:
   template <typename... Args> void push_args(Args&&... args) {
      auto tc = detail::type_converter_t<Host>{_state.host, get_interface()};
      (void)tc;
      (..., push_operand(detail::resolve_result(tc, std::forward<Args>(args))));
   }

   inline void setup_locals(uint32_t index) {
      const auto& fn = _mod->code[index - _mod->get_imported_functions_size()];
      for (uint32_t i = 0; i < fn.locals.size(); i++) {
         for (uint32_t j = 0; j < fn.locals[i].count; j++)
            switch (fn.locals[i].type) {
            case types::i32:
               push_operand(i32_const_t{(uint32_t)0});
               break;
            case types::i64:
               push_operand(i64_const_t{(uint64_t)0});
               break;
            case types::f32:
               push_operand(f32_const_t{(uint32_t)0});
               break;
            case types::f64:
               push_operand(f64_const_t{(uint64_t)0});
               break;
            default:
               throw exceptions::interpreter{"invalid function param type"};
            }
      }
   }

#define CREATE_TABLE_ENTRY(NAME, CODE) &&ev_label_##NAME,
#define CREATE_LABEL(NAME, CODE)                                                                                       \
   ev_label_##NAME                                                                                                     \
       : std::forward<Visitor>(visitor)(                                                                               \
             ev_variant->template get<forge::vm::wasm::interpret::FORGE_VM_WASM_INTERPRET_OPCODE_T(NAME)>());          \
   ev_variant = _state.pc;                                                                                             \
   goto* dispatch_table[ev_variant->index()];
#define CREATE_EXIT_LABEL(NAME, CODE) ev_label_##NAME : return;
#define CREATE_EMPTY_LABEL(NAME, CODE) ev_label_##NAME : throw exceptions::interpreter{"empty operand"};

   template <typename Visitor> void execute(Visitor&& visitor) {
      static void* dispatch_table[] = {
          FORGE_VM_WASM_INTERPRET_CONTROL_FLOW_OPS(CREATE_TABLE_ENTRY) FORGE_VM_WASM_INTERPRET_BR_TABLE_OP(
              CREATE_TABLE_ENTRY) FORGE_VM_WASM_INTERPRET_RETURN_OP(CREATE_TABLE_ENTRY)
              FORGE_VM_WASM_INTERPRET_CALL_OPS(CREATE_TABLE_ENTRY) FORGE_VM_WASM_INTERPRET_CALL_IMM_OPS(
                  CREATE_TABLE_ENTRY) FORGE_VM_WASM_INTERPRET_PARAMETRIC_OPS(CREATE_TABLE_ENTRY)
                  FORGE_VM_WASM_INTERPRET_VARIABLE_ACCESS_OPS(CREATE_TABLE_ENTRY)
                      FORGE_VM_WASM_INTERPRET_MEMORY_OPS(CREATE_TABLE_ENTRY)
                          FORGE_VM_WASM_INTERPRET_I32_CONSTANT_OPS(CREATE_TABLE_ENTRY)
                              FORGE_VM_WASM_INTERPRET_I64_CONSTANT_OPS(CREATE_TABLE_ENTRY)
                                  FORGE_VM_WASM_INTERPRET_F32_CONSTANT_OPS(CREATE_TABLE_ENTRY)
                                      FORGE_VM_WASM_INTERPRET_F64_CONSTANT_OPS(CREATE_TABLE_ENTRY)
                                          FORGE_VM_WASM_INTERPRET_COMPARISON_OPS(CREATE_TABLE_ENTRY)
                                              FORGE_VM_WASM_INTERPRET_NUMERIC_OPS(CREATE_TABLE_ENTRY)
                                                  FORGE_VM_WASM_INTERPRET_CONVERSION_OPS(CREATE_TABLE_ENTRY)
                                                      FORGE_VM_WASM_INTERPRET_EXIT_OP(CREATE_TABLE_ENTRY)
                                                          FORGE_VM_WASM_INTERPRET_EMPTY_OPS(CREATE_TABLE_ENTRY)
                                                              FORGE_VM_WASM_INTERPRET_ERROR_OPS(CREATE_TABLE_ENTRY) &&
          __ev_last};
      auto* ev_variant = _state.pc;
      goto* dispatch_table[ev_variant->index()];
      while (1) {
         FORGE_VM_WASM_INTERPRET_CONTROL_FLOW_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_BR_TABLE_OP(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_RETURN_OP(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_CALL_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_CALL_IMM_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_PARAMETRIC_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_VARIABLE_ACCESS_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_MEMORY_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_I32_CONSTANT_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_I64_CONSTANT_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_F32_CONSTANT_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_F64_CONSTANT_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_COMPARISON_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_NUMERIC_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_CONVERSION_OPS(CREATE_LABEL);
         FORGE_VM_WASM_INTERPRET_EXIT_OP(CREATE_EXIT_LABEL);
         FORGE_VM_WASM_INTERPRET_EMPTY_OPS(CREATE_EMPTY_LABEL);
         FORGE_VM_WASM_INTERPRET_ERROR_OPS(CREATE_LABEL);
      __ev_last:
         throw exceptions::interpreter{"should never reach here"};
      }
   }

#undef CREATE_EMPTY_LABEL
#undef CREATE_LABEL
#undef CREATE_TABLE_ENTRY

   struct execution_state {
      host_type* host = nullptr;
      uint32_t as_index = 0;
      uint32_t os_index = 0;
      opcode* pc = nullptr;
      bool exiting = false;
   };

   bounded_allocator _base_allocator = {(constants::max_call_depth + 1) * sizeof(activation_frame)};
   execution_state _state;
   uint32_t _last_op_index = 0;
   call_stack _as = {_base_allocator};
   opcode _halt;
};
} // namespace forge::vm::wasm::interpret
