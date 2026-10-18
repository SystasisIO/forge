module;

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <forge/exceptions/macros.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "details/engine_instance.hxx"
#include "details/engine_limits.hxx"
#include "details/engine_memory.hxx"
#include <wasm_runtime.h>

module forge.vm.wasm.interpret.prepared_module;

import forge.vm.wasm.interpret.scope_guard;

#include "details/prepared_module_impl.hxx"

static_assert(WASM_MEM_ALLOC_WITH_USAGE != 0, "The public runtime requires usage-aware allocation");

namespace forge::vm::wasm::interpret {
namespace {

[[noreturn]] void throw_trap(detail::engine_exception::reason reason) {
   using value = detail::engine_exception::reason;
   using kind = exceptions::guest_trap_kind;
   auto result = kind::unknown;
   switch (reason) {
   case value::memory:
      throw exceptions::memory{"WASM memory access trapped"};
   case value::physical_stack:
   case value::allocation:
      throw exceptions::execution_unavailable{"Local WASM execution resources are unavailable"};
   case value::unreachable:
      result = kind::unreachable;
      break;
   case value::table_bounds:
      result = kind::table_bounds;
      break;
   case value::indirect_type:
      result = kind::indirect_type;
      break;
   case value::integer_divide_by_zero:
      result = kind::integer_divide_by_zero;
      break;
   case value::integer_overflow:
      result = kind::integer_overflow;
      break;
   case value::invalid_conversion:
      result = kind::invalid_conversion;
      break;
   case value::aux_stack:
      result = kind::auxiliary_stack;
      break;
   default:
      break;
   }
   throw exceptions::guest_trap{result, "WASM guest execution trapped"};
}

} // namespace

struct prepared_module::context::impl {
   prepared_module& source;
   std::uint32_t depth;
   const execution_mode mode = execution_mode::interpreter;
   std::recursive_mutex gate;
   std::uint32_t pages = capacity_profile::max_pages;
   wasm_allocator* allocator{};
   std::optional<wasm_allocator::borrow> borrow;
   char* base{};
   bool live_allocation = false;
   detail::engine_memory memory{this, &allocate, &reallocate, &release};
   std::unique_ptr<detail::engine_instance> instance;
   std::exception_ptr failure;
   std::exception_ptr normal_exit;
   failure_scope* failures{};
   std::byte* native_stack_boundary{};
   std::size_t active_calls = 0;

   auto scoped_exit() {
      auto previous = std::exchange(normal_exit, {});
      return scope_guard{[this, previous = std::move(previous)]() mutable { normal_exit = std::move(previous); }};
   }

   void record_failure(const std::exception_ptr& error) noexcept {
      if (error)
         for (auto* scope = failures; scope; scope = scope->_previous)
            if (!scope->_failure)
               scope->_failure = error;
   }

   void retire() noexcept {
      instance.reset();
      borrow.reset();
      base = nullptr;
      memory.set_base(nullptr);
      live_allocation = false;
   }
   static void retire_owner(void* data, const void* token) noexcept {
      auto& self = *static_cast<impl*>(data);
      const auto lock = std::lock_guard{self.gate};
      if (self.borrow && self.borrow->identity() == token)
         self.retire();
   }

   static void* allocate(void* data, std::uint32_t size) {
      auto& self = *static_cast<impl*>(data);
      detail::check<exceptions::execution_unavailable>(self.borrow && !self.live_allocation && size,
                                                       "Invalid linear-memory allocation request");
      const auto pages = (static_cast<std::uint64_t>(size) + 65535) / 65536;
      self.borrow->alloc(pages);
      self.live_allocation = true;
      return self.base;
   }
   static void* reallocate(void* data, void* pointer, std::uint32_t size) {
      auto& self = *static_cast<impl*>(data);
      detail::check<exceptions::execution_unavailable>(
          self.borrow && size && (pointer == self.base || (!pointer && !self.live_allocation)),
          "Invalid linear-memory growth request");
      const auto pages = (static_cast<std::uint64_t>(size) + 65535) / 65536;
      const auto current = self.allocator->get_current_page();
      detail::check<exceptions::execution_unavailable>(current >= 0 && pages >= static_cast<std::uint32_t>(current),
                                                       "Linear memory cannot shrink during an invocation");
      self.borrow->alloc(pages - current);
      self.live_allocation = true;
      return self.base;
   }
   static void release(void* data, void* pointer) {
      auto& self = *static_cast<impl*>(data);
      detail::check<exceptions::execution_unavailable>(self.borrow && pointer == self.base,
                                                       "Linear-memory release has a different owner");
      self.live_allocation = false;
   }
};

prepared_module::context::context(prepared_module& source, std::uint32_t depth)
    : context(source, depth,
              source._impl->engine->llvm_jit_ready()
                  ? source._impl->engine->llvm_jit_profile_ready() ? execution_mode::jit_profile : execution_mode::jit
                  : execution_mode::interpreter) {}
prepared_module::context::context(prepared_module& source, std::uint32_t depth, execution_mode mode)
    : _impl(std::make_shared<impl>(source, depth, mode)) {
   if (mode != execution_mode::interpreter && mode != execution_mode::jit && mode != execution_mode::jit_profile)
      throw exceptions::profile{"Unsupported WASM execution mode"};
   if (mode != execution_mode::interpreter &&
       (source._impl->engine != source._impl->source || !source._impl->engine->llvm_jit_ready()))
      throw exceptions::execution_unavailable{"WASM JIT module has not completed preparation"};
   if (mode == execution_mode::jit_profile && !source._impl->engine->llvm_jit_profile_ready())
      FORGE_THROW_EXCEPTION(exceptions::profile, "WASM JIT module has no instruction profile");
}
prepared_module::context::~context() = default;
bool prepared_module::context::matches(const prepared_module& source, execution_mode mode) const noexcept {
   return std::addressof(_impl->source) == std::addressof(source) && _impl->mode == mode;
}
void prepared_module::context::set_wasm_allocator(wasm_allocator* value) {
   const auto lock = std::unique_lock{_impl->gate, std::try_to_lock};
   if (!lock.owns_lock())
      throw exceptions::busy{"Context is executing"};
   reset();
   _impl->allocator = value;
}
void prepared_module::context::set_max_call_depth(std::uint32_t value) {
   const auto lock = std::lock_guard{_impl->gate};
   if (value > capacity_profile::max_call_depth)
      throw exceptions::profile{"Call depth exceeds capacity profile v1"};
   _impl->depth = value;
}
void prepared_module::context::set_max_pages(std::uint32_t value) {
   const auto lock = std::lock_guard{_impl->gate};
   if (value != UINT32_MAX && value > capacity_profile::max_pages)
      throw exceptions::profile{"Page budget exceeds capacity profile v1"};
   _impl->pages = std::min(value, capacity_profile::max_pages);
}

void prepared_module::context::set_native_stack_boundary(std::byte* value) noexcept {
   _impl->native_stack_boundary = value;
}

void prepared_module::context::initialize(void* host, std::stop_token stop) {
   const auto lock = std::unique_lock{_impl->gate, std::try_to_lock};
   if (!lock.owns_lock())
      throw exceptions::busy{"Context is executing"};
   reset();
   if (_impl->source._impl->use == purpose::validation_only)
      throw exceptions::invocation_state{"Validation-only modules cannot execute"};
   if (stop.stop_requested())
      throw exceptions::canceled{"WASM invocation canceled"};
   if (has_linear_memory() && !_impl->allocator)
      throw exceptions::allocation{"Linear memory requires an allocator"};
   if (_impl->depth > capacity_profile::max_call_depth)
      throw exceptions::profile{"Call depth exceeds capacity profile v1"};
   _impl->source.validate();
   const auto cleanup = scope_guard{[&] {
      if (!_impl->instance) {
         _impl->borrow.reset();
         _impl->base = nullptr;
         _impl->memory.set_base(nullptr);
         _impl->live_allocation = false;
      }
   }};
   if (has_linear_memory()) {
      try {
         _impl->borrow.emplace(*_impl->allocator, std::weak_ptr<void>{_impl}, &impl::retire_owner);
      } catch (const std::bad_alloc&) {
         throw exceptions::execution_unavailable{"Memory owner registration allocation failed"};
      }
      _impl->base = _impl->allocator->get_base_ptr<char>();
      _impl->memory.set_base(_impl->base);
   }
   const auto capture_failure = scope_guard{[&] { _impl->record_failure(_impl->failure); }};
   const auto exit_scope = _impl->scoped_exit();
   try {
      auto operation = activity{*this, true};
      if (_impl->borrow)
         _impl->borrow->reset(0);
      _impl->instance = std::make_unique<detail::engine_instance>(
          _impl->source._impl->engine, _impl->source._impl->limits->interpreter_stack_bytes(_impl->depth), stop, host,
          _impl->pages, _impl->depth, &_impl->failure, _impl->native_stack_boundary, &_impl->memory,
          _impl->mode != execution_mode::interpreter ? Mode_LLVM_JIT : Mode_Interp, &_impl->normal_exit);
   } catch (const detail::engine_instance::canceled&) {
      throw exceptions::canceled{"WASM invocation canceled"};
   } catch (const exceptions::busy&) {
      throw;
   } catch (const detail::engine_instance::depth_exceeded&) {
      throw exceptions::guest_trap{exceptions::guest_trap_kind::logical_depth, "WASM logical call depth exceeded"};
   } catch (const detail::engine_instance::fp_failure&) {
      try {
         FORGE_THROW_EXCEPTION(exceptions::execution_unavailable, "WASM FP environment transition failed");
      } catch (...) {
         // Invocation scopes survive reset(), unlike instance/host state.
         _impl->record_failure(std::current_exception());
         throw;
      }
   } catch (const detail::engine_instance::initialization_trap& error) {
      throw_trap(error.reason);
   } catch (...) {
      if (_impl->failure)
         std::rethrow_exception(_impl->failure);
      throw exceptions::execution_unavailable{"Local WASM instantiation resources are unavailable"};
   }
}

void prepared_module::context::reset() {
   const auto lock = std::unique_lock{_impl->gate, std::try_to_lock};
   if (!lock.owns_lock() || _impl->active_calls)
      throw exceptions::busy{"Context is executing"};
   _impl->retire();
   _impl->failure = {};
   _impl->normal_exit = {};
}

prepared_module::context::failure_scope::failure_scope(context& context) : _impl(context._impl) {
   const auto lock = std::lock_guard{_impl->gate};
   _previous = _impl->failures;
   _impl->failures = this;
}
prepared_module::context::failure_scope::~failure_scope() {
   const auto lock = std::lock_guard{_impl->gate};
   assert(_impl->failures == this);
   _impl->failures = _previous;
}
std::exception_ptr prepared_module::context::failure_scope::failure() const noexcept {
   const auto lock = std::lock_guard{_impl->gate};
   return _failure;
}

prepared_module::context::activity::activity(context& context) : activity(context, false) {}
prepared_module::context::activity::activity(context& context, bool initializing)
    : _impl(context._impl), _lock(_impl->gate, std::try_to_lock) {
   if (!_lock.owns_lock())
      throw exceptions::busy{"Context is executing on another thread"};
   if (!initializing && !_impl->instance)
      throw exceptions::interpreter{"Backend is not initialized"};
   if (_impl->borrow)
      _memory.emplace(*_impl->borrow);
   ++_impl->active_calls;
}
prepared_module::context::activity::~activity() {
   --_impl->active_calls;
}
prepared_module::context::activity prepared_module::context::scoped_activity() {
   return activity{*this};
}
void prepared_module::context::initialize_globals() {
   // The immutable module owns global initializers. Reset stages their replay
   // for the next WAMR instantiation, without executing guest initialization.
   reset();
}
bool prepared_module::context::initialized() const noexcept {
   const auto lock = std::lock_guard{_impl->gate};
   return static_cast<bool>(_impl->instance);
}
bool prepared_module::context::has_linear_memory() const noexcept {
   return _impl->source.has_linear_memory();
}
std::exception_ptr prepared_module::context::host_failure() const noexcept {
   const auto lock = std::lock_guard{_impl->gate};
   return _impl->failure;
}
void prepared_module::context::clear_host_failure() noexcept {
   const auto lock = std::lock_guard{_impl->gate};
   _impl->failure = {};
}
void prepared_module::context::exit(const std::error_code&) {
   const auto lock = std::lock_guard{_impl->gate};
   try {
      FORGE_THROW_EXCEPTION(exceptions::exit, "WASM exited");
   } catch (const exceptions::exit&) {
      _impl->normal_exit = std::current_exception();
      throw;
   }
}

char* prepared_module::context::linear_memory() const {
   const auto lock = std::lock_guard{_impl->gate};
   if (!_impl->instance)
      return nullptr;
   auto* memory = wasm_runtime_get_memory(_impl->instance->get(), 0);
   if (!memory)
      return nullptr;
   auto* base = static_cast<char*>(wasm_memory_get_base_address(memory));
   return base ? base : _impl->base;
}
std::size_t prepared_module::context::memory_size() const {
   const auto lock = std::lock_guard{_impl->gate};
   if (!_impl->instance)
      return 0;
   const auto memory = wasm_runtime_get_memory(_impl->instance->get(), 0);
   return memory ? static_cast<std::size_t>(wasm_memory_get_cur_page_count(memory)) *
                       wasm_memory_get_bytes_per_page(memory)
                 : 0;
}
std::uint32_t prepared_module::context::current_linear_memory() const {
   return static_cast<std::uint32_t>(memory_size() / 65536);
}

std::optional<operand_stack_elem> prepared_module::context::call(void* host, std::string_view name,
                                                                 std::span<const operand_stack_elem> arguments,
                                                                 std::stop_token stop) {
   return call(host, _impl->source.get_exported_function(name), arguments, stop);
}

std::optional<operand_stack_elem> prepared_module::context::call(void* host, std::uint32_t index,
                                                                 std::span<const operand_stack_elem> arguments,
                                                                 std::stop_token stop) {
   auto operation = scoped_activity();
   if (!_impl->instance)
      throw exceptions::interpreter{"Backend is not initialized"};
   if (_impl->failure)
      std::rethrow_exception(_impl->failure);
   if (!_impl->depth)
      FORGE_THROW_EXCEPTION(exceptions::interpreter, "WASM call depth is zero");
   const auto type = _impl->source.get_function_type(index);
   if (type.param_types.size() != arguments.size())
      throw exceptions::interpreter{"WASM argument count mismatch"};
   std::vector<std::uint32_t> cells;
   for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto& value = arguments[i];
      std::uint64_t bits;
      const auto expected = type.param_types[i];
      if ((expected == types::i32 && !value.is_a<i32_const_t>()) ||
          (expected == types::i64 && !value.is_a<i64_const_t>()) ||
          (expected == types::f32 && !value.is_a<f32_const_t>()) ||
          (expected == types::f64 && !value.is_a<f64_const_t>()))
         throw exceptions::interpreter{"WASM argument type mismatch"};
      switch (type.param_types[i]) {
      case types::i32:
         cells.push_back(value.to_ui32());
         continue;
      case types::f32:
         cells.push_back(value.to_fui32());
         continue;
      case types::i64:
         bits = value.to_ui64();
         break;
      case types::f64:
         bits = value.to_fui64();
         break;
      default:
         throw exceptions::interpreter{"Unsupported WASM value kind"};
      }
      cells.push_back(static_cast<std::uint32_t>(bits));
      cells.push_back(static_cast<std::uint32_t>(bits >> 32));
   }
   const auto count = static_cast<std::uint32_t>(cells.size());
   cells.resize(std::max<std::size_t>(2, cells.size()));
   const auto exit_scope = _impl->scoped_exit();
   try {
      if (!_impl->instance->invoke(index, count, cells, stop, host)) {
         throw_trap(detail::engine_exception::inspect(_impl->instance->get()));
      }
   } catch (...) {
      _impl->failure = _impl->instance->host_failure();
      if (_impl->failure) {
         if (_impl->failure == _impl->normal_exit && _impl->instance->consume_normal_exit(_impl->normal_exit)) {
            _impl->failure = {};
            return {};
         }
         _impl->record_failure(_impl->failure);
         std::rethrow_exception(_impl->failure);
      }
      try {
         throw;
      } catch (const detail::engine_instance::canceled&) {
         throw exceptions::canceled{"WASM invocation canceled"};
      } catch (const detail::engine_instance::depth_exceeded&) {
         throw exceptions::guest_trap{exceptions::guest_trap_kind::logical_depth, "WASM logical call depth exceeded"};
      } catch (const detail::engine_instance::unavailable&) {
         throw exceptions::execution_unavailable{"Local WASM function metadata allocation failed"};
      } catch (const detail::engine_instance::fp_failure&) {
         try {
            FORGE_THROW_EXCEPTION(exceptions::execution_unavailable, "WASM FP environment transition failed");
         } catch (...) {
            _impl->record_failure(std::current_exception());
            throw;
         }
      }
   }
   if (!type.return_count)
      return {};
   const auto bits = std::uint64_t(cells[0]) | (std::uint64_t(cells[1]) << 32);
   switch (type.return_type) {
   case types::i32:
      return i32_const_t{cells[0]};
   case types::i64:
      return i64_const_t{bits};
   case types::f32:
      return f32_const_t{std::bit_cast<float>(cells[0])};
   case types::f64:
      return f64_const_t{std::bit_cast<double>(bits)};
   default:
      throw exceptions::interpreter{"Unsupported WASM result kind"};
   }
}

std::optional<operand_stack_elem> prepared_module::context::call_indirect(void* host, std::uint32_t index,
                                                                          std::span<const operand_stack_elem> arguments,
                                                                          std::stop_token stop) {
   auto operation = scoped_activity();
   if (!_impl->instance)
      throw exceptions::interpreter{"Backend is not initialized"};
   const auto instance = reinterpret_cast<WASMModuleInstance*>(_impl->instance->get());
   if (!instance->table_count || index >= instance->tables[0]->cur_size ||
       instance->tables[0]->elems[index] == UINT32_MAX)
      throw exceptions::guest_trap{exceptions::guest_trap_kind::table_bounds,
                                   "WASM table index is out of bounds or uninitialized"};
   return call(host, instance->tables[0]->elems[index], arguments, stop);
}

} // namespace forge::vm::wasm::interpret
