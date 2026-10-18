module;

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.vm.wasm.interpret.backend:runtime_backend;

import forge.vm.wasm.interpret.prepared_module;
import forge.vm.wasm.interpret.debug_info;
import forge.vm.wasm.interpret.watchdog;
import forge.vm.wasm.interpret.scope_guard;

namespace forge::vm::wasm::interpret {
namespace detail {

struct runtime_timing_scope {
   const prepared_module* module;
   bool typed;
   runtime_timing_scope* previous;
};
inline thread_local runtime_timing_scope* active_runtime_timing = nullptr;

template <typename Registered> struct runtime_host {
   using type = typename Registered::host_type_t;
};
template <> struct runtime_host<std::nullptr_t> {
   using type = standalone_function_t;
};
template <typename Registered> struct runtime_converter {
   using type = typename Registered::type_converter_t;
};
template <> struct runtime_converter<std::nullptr_t> {
   using type = type_converter<standalone_function_t>;
};

template <typename Registered> std::vector<host_binding> snapshot_host_bindings() {
   std::vector<host_binding> result;
   if constexpr (!std::is_same_v<Registered, std::nullptr_t>) {
      const auto& mappings = Registered::mappings::get();
      for (const auto& [name, index] : mappings.named_mapping) {
         if (index >= mappings.functions.size() || index >= mappings.host_functions.size())
            throw exceptions::link{"Invalid host registration index"};
         result.push_back({name.first, name.second, mappings.host_functions[index],
                           [function = mappings.functions[index]](void* host, execution_interface interface) {
                              using host_type = typename Registered::host_type_t;
                              using converter_type = typename Registered::type_converter_t;
                              if constexpr (!std::is_same_v<host_type, standalone_function_t>)
                                 detail::check<exceptions::interpreter>(host != nullptr,
                                                                        "Stateful host instance is required");
                              auto converter = converter_type{static_cast<host_type*>(host), std::move(interface)};
                              std::invoke(function, static_cast<host_type*>(host), converter);
                           }});
      }
   }
   return result;
}

} // namespace detail

template <typename Registered, typename Options, typename DebugInfo, bool ValidateOnly = false, bool Native = false,
          bool Profile = false>
class runtime_backend {
   using host_t = typename detail::runtime_host<Registered>::type;
   static constexpr auto mode = Profile  ? execution_mode::jit_profile
                                : Native ? execution_mode::jit
                                         : execution_mode::interpreter;
   static_assert(!Profile || Native);

 public:
   runtime_backend() = default;
   runtime_backend(wasm_code& code, wasm_allocator* memory, const Options& policy = {}) {
      prepare(code, memory, policy, true);
   }
   runtime_backend(wasm_code&& code, wasm_allocator* memory, const Options& policy = {}) {
      prepare(code, memory, policy, true);
   }
   runtime_backend(wasm_code& code, host_t& host, wasm_allocator* memory, const Options& policy = {}) {
      prepare(code, memory, policy, true, &host);
   }
   runtime_backend(wasm_code&& code, host_t& host, wasm_allocator* memory, const Options& policy = {}) {
      prepare(code, memory, policy, true, &host);
   }
   runtime_backend(wasm_code_ptr& code, std::size_t size, wasm_allocator* memory, const Options& policy = {},
                   bool = true, bool owns = true) {
      prepare({code.raw(), size}, memory, policy, owns);
   }
   runtime_backend(wasm_code_ptr& code, std::size_t size, host_t& host, wasm_allocator* memory,
                   const Options& policy = {}) {
      prepare({code.raw(), size}, memory, policy, true, &host);
   }
   void validate() const {
      detail::check<exceptions::interpreter>(static_cast<bool>(_module), "Backend has no module");
      _module->validate();
   }
   prepared_module& get_module() {
      return *_module;
   }
   runtime_context& get_context() {
      detail::check<exceptions::interpreter>(_context != nullptr, "Backend has no execution context");
      return *_context;
   }
   const DebugInfo& get_debug() const {
      return _debug;
   }
   void share(const runtime_backend& source) {
      auto source_lock = acquire(source._active);
      auto target_lock = acquire(_active);
      usable();
      source.usable();
      detail::check<exceptions::invocation_state>(!_module && !_context, "Share target must be empty");
      _module = source._module;
      _policy = source._policy;
      _initial = source._initial;
      if constexpr (Profile && std::is_same_v<DebugInfo, profile_instr_map>)
         if (_module)
            _debug = _module->get_debug();
   }
   void set_context(runtime_context* context) {
      detail::check<exceptions::invocation_state>(!_owns, "Backend owns its context");
      detail::check<exceptions::invocation_state>(!context || (_module && context->matches(*_module, mode)),
                                                  "Execution context belongs to a different module or mode");
      _context = context;
   }
   void set_wasm_allocator(wasm_allocator* memory) {
      _memory = memory;
      if (_context) {
         _context->set_wasm_allocator(memory);
         _context->reset();
      }
   }
   void reset_max_call_depth() {
      _policy.max_call_depth = _initial.max_call_depth;
      get_context().set_max_call_depth(_policy.max_call_depth);
   }
   void reset_max_pages() {
      _policy.max_pages = _initial.max_pages;
      get_context().set_max_pages(_policy.max_pages);
   }
   runtime_backend& initialize(host_t* host, const Options& options) {
      const auto policy = resolve_options(options);
      _policy.max_call_depth = policy.max_call_depth;
      _policy.max_pages = policy.max_pages;
      return initialize(host);
   }
   runtime_backend& initialize(host_t* host = nullptr) {
      usable();
      _module->check_limits(_policy);
      auto& context = get_context();
      context.set_max_call_depth(_policy.max_call_depth);
      context.set_max_pages(_policy.max_pages);
      context.initialize(host, token());
      return *this;
   }
   runtime_backend& initialize(host_t& host) {
      return initialize(&host);
   }
   void exit(const std::error_code& error) {
      get_context().exit(error);
   }

   template <typename F>
   void with_invocation(host_t* host, const Options& policy, std::chrono::steady_clock::duration budget,
                        std::stop_token stop, F&& body) {
      static_assert(!ValidateOnly);
      auto lock = acquire(_active);
      usable();
      detail::check<exceptions::invocation_state>(
          _module && !_owns && !_context && !_memory,
          "Invocation requires a prepared backend without attached context or memory");
      detail::check<exceptions::shared_instance>(_module.use_count() == 1,
                                                 "Invocation requires exclusive module ownership");
      if constexpr (!std::is_same_v<host_t, standalone_function_t>)
         detail::check<exceptions::invocation_state>(host != nullptr, "Stateful invocation requires a host instance");
      detail::check<exceptions::canceled>(!stop.stop_requested(), "Execution canceled");
      detail::check<exceptions::timeout>(budget > std::chrono::steady_clock::duration::zero(), "Execution timed out");
      auto memory = wasm_allocator{};
      auto context = runtime_context{*_module, _policy.max_call_depth, mode};
      context.set_wasm_allocator(&memory);
      _context = &context;
      _memory = &memory;
      auto detach = scope_guard{[&] {
         _context = nullptr;
         _memory = nullptr;
      }};
      const auto failures = runtime_context::failure_scope{context};
      try {
         timed_run(watchdog{budget, stop}, [&] {
            initialize(host, policy);
            std::invoke(std::forward<F>(body));
            if (const auto error = failures.failure())
               std::rethrow_exception(error);
         });
      } catch (...) {
         if (const auto error = failures.failure()) {
            _unusable = true;
            std::rethrow_exception(error);
         }
         try {
            throw;
         } catch (const exceptions::interpreter&) {
            throw;
         } catch (const exceptions::memory&) {
            throw;
         } catch (const exceptions::stack_memory&) {
            throw;
         } catch (const exceptions::pointer_out_of_bounds&) {
            throw;
         } catch (const exceptions::invalid_element&) {
            throw;
         } catch (const exceptions::canceled&) {
            throw;
         } catch (const exceptions::interrupted&) {
            throw;
         } catch (const exceptions::timeout&) {
            throw;
         } catch (const exceptions::exit&) {
            throw;
         } catch (const exceptions::profile&) {
            throw;
         } catch (...) {
            _unusable = true;
            throw;
         }
      }
   }
   template <typename Watchdog, typename F> void timed_run(Watchdog&& timer, F&& body) {
      timed_run_impl<false>(std::forward<Watchdog>(timer), std::forward<F>(body));
   }
   template <typename F>
   void timed_run(std::chrono::steady_clock::time_point deadline, interrupt_options options, F&& body) {
      timed_run_impl<true>(watchdog{deadline, options}, std::forward<F>(body));
   }
   template <typename... Args>
   auto call_with_return(host_t& host, std::string_view, std::string_view name, Args&&... args) {
      return invoke(&host, name, std::forward<Args>(args)...);
   }
   template <typename... Args> auto call_with_return(std::string_view, std::string_view name, Args&&... args) {
      hostless();
      return invoke(nullptr, name, std::forward<Args>(args)...);
   }
   template <typename... Args> bool call(host_t& host, std::string_view mod, std::string_view name, Args&&... args) {
      call_with_return(host, mod, name, std::forward<Args>(args)...);
      return true;
   }
   template <typename... Args> bool call(std::string_view mod, std::string_view name, Args&&... args) {
      call_with_return(mod, name, std::forward<Args>(args)...);
      return true;
   }
   template <typename... Args> bool call(host_t* host, std::uint32_t index, Args&&... args) {
      invoke(host, index, std::forward<Args>(args)...);
      return true;
   }
   template <typename... Args> bool call_indirect(host_t* host, std::uint32_t index, Args&&... args) {
      ready();
      ++_direct_depth;
      const auto frame = scope_guard{[&] { --_direct_depth; }};
      auto activity = get_context().scoped_activity();
      const auto values = convert_arguments(host, std::forward<Args>(args)...);
      get_context().call_indirect(host, index, values, token());
      return true;
   }
   template <typename... Args>
   bool operator()(host_t& host, std::string_view mod, std::string_view name, Args&&... args) {
      return call(host, mod, name, std::forward<Args>(args)...);
   }
   template <typename... Args> bool operator()(std::string_view mod, std::string_view name, Args&&... args) {
      return call(mod, name, std::forward<Args>(args)...);
   }
   template <typename Watchdog> void execute_all(Watchdog&& timer, host_t& host) {
      timed_run(std::forward<Watchdog>(timer), [&] {
         for (auto& name : _module->function_exports())
            call(host, "", name);
      });
   }
   template <typename Watchdog> void execute_all(Watchdog&& timer) {
      hostless();
      timed_run(std::forward<Watchdog>(timer), [&] {
         for (auto& name : _module->function_exports())
            call("", name);
      });
   }

 private:
   [[noreturn]] static void throw_interrupt(watchdog::interruption reason) {
      switch (reason) {
      case watchdog::interruption::canceled:
         throw exceptions::canceled{"Execution canceled"};
      case watchdog::interruption::deadline:
         throw exceptions::timeout{"Execution timed out"};
      case watchdog::interruption::interrupted:
         throw exceptions::interrupted{"Execution interrupted"};
      default:
         throw exceptions::execution_unavailable{"Invalid interruption reason"};
      }
   }
   template <bool Typed, typename Watchdog, typename F> void timed_run_impl(Watchdog&& timer, F&& body) {
      usable();
      for (auto* ancestor = detail::active_runtime_timing; ancestor; ancestor = ancestor->previous)
         if (_module && _module.get() == ancestor->module && (Typed || ancestor->typed))
            throw exceptions::execution_unavailable{"Overlapping typed timed scopes share a module"};
      auto failures = std::optional<runtime_context::failure_scope>{};
      if (_context)
         failures.emplace(*_context);
      auto active = detail::runtime_timing_scope{_module.get(), Typed, detail::active_runtime_timing};
      auto stop = std::stop_source{};
      auto* previous = _stop;
      auto restore = scope_guard{[&] {
         _stop = previous;
         detail::active_runtime_timing = active.previous;
      }};
      detail::active_runtime_timing = &active;
      _stop = &stop;
      auto reason = std::atomic<watchdog::interruption>{watchdog::interruption::none};
      auto failure = std::exception_ptr{};
      {
         auto guard = std::forward<Watchdog>(timer).scoped_run(
             [&](watchdog::interruption value = watchdog::interruption::deadline) noexcept {
                if (value == watchdog::interruption::none)
                   return;
                auto expected = watchdog::interruption::none;
                if (reason.compare_exchange_strong(expected, value, std::memory_order_acq_rel))
                   stop.request_stop();
             });
         try {
            if constexpr (Typed) {
               if (guard.enter())
                  std::invoke(std::forward<F>(body));
            } else
               std::invoke(std::forward<F>(body));
         } catch (...) {
            failure = std::current_exception();
         }
      }
      // All callback delivery is joined before observing borrowed context state
      // or retiring this scope's stop source and interruption classification.
      if (failures) {
         if (const auto error = failures->failure())
            std::rethrow_exception(error);
      } else if (_context && _context->host_failure()) {
         std::rethrow_exception(_context->host_failure());
      }
      const auto fired = reason.load(std::memory_order_acquire);
      if constexpr (Typed) {
         if (failure) {
            try {
               std::rethrow_exception(failure);
            } catch (const exceptions::canceled&) {
               if (fired == watchdog::interruption::none)
                  throw;
            }
         }
      }
      if (fired != watchdog::interruption::none)
         throw_interrupt(fired);
      if (failure)
         std::rethrow_exception(failure);
   }
   static auto acquire(std::atomic_flag& active) {
      detail::check<exceptions::busy>(!active.test_and_set(std::memory_order_acquire),
                                      "Backend invocation is already active");
      return scope_guard{[&active] { active.clear(std::memory_order_release); }};
   }
   void usable() const {
      detail::check<exceptions::unusable>(!_unusable, "Backend must be discarded after a host failure");
   }
   static void hostless() {
      detail::check<exceptions::interpreter>(std::is_same_v<host_t, standalone_function_t>,
                                             "Stateful host instance is required");
   }
   std::stop_token token() const {
      return _stop ? _stop->get_token() : std::stop_token{};
   }
   void prepare(std::span<const std::uint8_t> code, wasm_allocator* memory, const Options& policy, bool owns,
                host_t* host = nullptr) {
      _policy = resolve_options(policy);
      _initial = _policy;
      const auto use = ValidateOnly                                 ? prepared_module::purpose::validation_only
                       : std::is_same_v<Registered, std::nullptr_t> ? prepared_module::purpose::unlinked_execution
                                                                    : prepared_module::purpose::execution;
      auto bindings = detail::snapshot_host_bindings<Registered>();
      if constexpr (Native) {
         constexpr auto ceiling = std::uint64_t{1024} * 1024 * 1024;
         const auto allocation = prepared_module::preparation{{ceiling, ceiling}};
         _module = std::make_shared<prepared_module>(code, _policy, std::span{bindings}, use, allocation, mode);
         allocation.seal();
      } else {
         _module = std::make_shared<prepared_module>(code, _policy, std::move(bindings), use);
      }
      if (owns || ValidateOnly)
         _module->validate();
      if constexpr (Profile && std::is_same_v<DebugInfo, profile_instr_map>)
         _debug = _module->get_debug();
      _module->check_limits(_policy);
      _memory = memory;
      _owns = owns;
      if (owns && !ValidateOnly) {
         _owned_context = std::make_unique<runtime_context>(*_module, _policy.max_call_depth, mode);
         _context = _owned_context.get();
         _context->set_wasm_allocator(memory);
         if (memory || !_context->has_linear_memory())
            initialize(host);
      }
   }
   void ready() {
      detail::check<exceptions::interpreter>(_context != nullptr, "Backend has no execution context");
      usable();
      detail::check<exceptions::allocation>(_memory || !_context->has_linear_memory(),
                                            "Linear memory requires an allocator");
      detail::check<exceptions::interpreter>(_context->initialized(), "Backend is not initialized");
      // Legacy direct calls are independent. A timed invocation retains the
      // original host failure even if user code catches it inside the body.
      if (!_stop && !_direct_depth)
         _context->clear_host_failure();
   }
   template <typename Function, typename... Args> auto invoke(host_t* host, Function function, Args&&... args) {
      ready();
      ++_direct_depth;
      const auto frame = scope_guard{[&] { --_direct_depth; }};
      auto activity = get_context().scoped_activity();
      const auto values = convert_arguments(host, std::forward<Args>(args)...);
      return get_context().call(host, function, values, token());
   }
   template <typename... Args> auto convert_arguments(host_t* host, Args&&... args) {
      auto operands = operand_stack{};
      using converter_t = typename detail::runtime_converter<Registered>::type;
      auto converter =
          converter_t{host, execution_interface{get_context().linear_memory(), get_context().memory_size(), &operands}};
      return std::array<operand_stack_elem, sizeof...(Args)>{
          detail::resolve_result(converter, std::forward<Args>(args))...};
   }
   std::shared_ptr<prepared_module> _module;
   std::unique_ptr<runtime_context> _owned_context;
   runtime_context* _context{};
   wasm_allocator* _memory{};
   options _policy{};
   options _initial{};
   DebugInfo _debug;
   bool _owns = false;
   bool _unusable = false;
   std::size_t _direct_depth = 0;
   mutable std::atomic_flag _active = ATOMIC_FLAG_INIT;
   std::stop_source* _stop{};
};

} // namespace forge::vm::wasm::interpret
