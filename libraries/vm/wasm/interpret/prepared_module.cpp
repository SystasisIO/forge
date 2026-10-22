module;

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "details/engine_module.hxx"
#include "details/engine_limits.hxx"
#include <wasm_runtime.h>
#include <aot_runtime.h>

module forge.vm.wasm.interpret.prepared_module;

#include "details/prepared_module_impl.hxx"

namespace forge::vm::wasm::interpret {
namespace {

[[noreturn]] void budget_failure(detail::engine_load_budget::failure reason) {
   if (reason == detail::engine_load_budget::failure::cancelled)
      throw exceptions::canceled{"WASM preparation canceled"};
   throw exceptions::execution_unavailable{"WASM preparation storage is unavailable or exceeds its budget"};
}

} // namespace

std::shared_ptr<detail::engine_load_budget> prepared_module::impl::load_budget(const preparation& value) {
   return std::static_pointer_cast<detail::engine_load_budget>(value._owner);
}
void prepared_module::impl::check_budget(const std::shared_ptr<detail::engine_load_budget>& owner) {
   try {
      owner->check();
   } catch (const detail::engine_load_budget::error& error) {
      budget_failure(error.reason);
   }
}

prepared_module::preparation::preparation(limits policy, std::stop_token stop) : _limits(policy) {
   try {
      const auto root = detail::engine_load_budget::root_byte_size();
      if (root > policy.workspace_bytes || root > policy.resident_bytes)
         throw exceptions::execution_unavailable{"Preparation ceiling cannot hold its root owner"};
      _owner = detail::engine_load_budget::create(policy.workspace_bytes, std::move(stop));
   } catch (const detail::engine_load_budget::error& error) {
      budget_failure(error.reason);
   } catch (const std::bad_alloc&) {
      throw exceptions::execution_unavailable{"Preparation root allocation failed"};
   }
}
std::uint64_t prepared_module::preparation::root_byte_size() {
   try {
      return detail::engine_load_budget::root_byte_size();
   } catch (const detail::engine_load_budget::error& error) {
      budget_failure(error.reason);
   }
}
std::uint64_t prepared_module::preparation::minimum_workspace_bytes() {
   return root_byte_size();
}
std::size_t prepared_module::preparation::allocation_overhead() noexcept {
   return detail::engine_load_budget::allocation_overhead();
}
prepared_module::preparation::limits prepared_module::preparation::configured_limits() const noexcept {
   return _limits;
}
std::uint64_t prepared_module::preparation::current_bytes() const noexcept {
   return impl::load_budget(*this)->current_bytes();
}
std::uint64_t prepared_module::preparation::peak_bytes() const noexcept {
   return impl::load_budget(*this)->peak_bytes();
}
void prepared_module::preparation::check() const {
   impl::check_budget(impl::load_budget(*this));
}
std::uint64_t prepared_module::preparation::seal() const {
   auto owner = impl::load_budget(*this);
   try {
      owner->seal(_limits.resident_bytes);
   } catch (const detail::engine_load_budget::error& error) {
      budget_failure(error.reason);
   }
   return owner->current_bytes();
}
void* prepared_module::preparation::allocate(std::size_t bytes, std::size_t alignment) const {
   auto owner = impl::load_budget(*this);
   const detail::engine_load_budget::scope scope{owner};
   auto* result = detail::engine_load_budget::allocate(bytes, alignment);
   if (!result) {
      impl::check_budget(owner);
      throw exceptions::execution_unavailable{"Preparation allocation failed"};
   }
   return result;
}
void prepared_module::preparation::deallocate(void* pointer) noexcept {
   detail::engine_load_budget::free(pointer);
}
void prepared_module::preparation::reserve(std::uint64_t bytes) const {
   auto owner = impl::load_budget(*this);
   if (!owner->reserve(bytes))
      impl::check_budget(owner);
}
void prepared_module::preparation::release(std::uint64_t bytes) const noexcept {
   impl::load_budget(*this)->release(bytes);
}
void prepared_module::preparation::attach_resident_charge(std::shared_ptr<void> charge) const {
   try {
      impl::load_budget(*this)->pin_charge(std::move(charge));
   } catch (const detail::engine_load_budget::error& error) {
      budget_failure(error.reason);
   }
}
bool prepared_module::preparation::retry_cleanup() const noexcept {
   return impl::load_budget(*this)->retry_cleanup();
}
prepared_module::preparation::cleanup_result
prepared_module::preparation::retry_pending_cleanup(std::size_t max_owners) noexcept {
   const auto result = detail::engine_runtime::retry_pending_cleanup(max_owners);
   return {result.attempted, result.remaining};
}

prepared_module::prepared_module(std::span<const std::uint8_t> code, const options& policy,
                                 std::vector<host_binding> bindings, purpose use)
    : _impl(impl::make<impl>({}, code, policy, std::span{bindings}, use)) {
   project_imports();
}
prepared_module::prepared_module(std::span<const std::uint8_t> code, const options& policy,
                                 std::span<host_binding> bindings, purpose use, const preparation& allocation)
    : _impl(impl::make<impl>(allocation, code, policy, bindings, use, allocation)) {
   try {
      project_imports();
      allocation.check();
   } catch (...) {
      decltype(imports){}.swap(imports);
      throw;
   }
}

void prepared_module::project_imports() {
   const auto& source = *reinterpret_cast<const WASMModule*>(_impl->source->get());
   // Reviewed libc++ 22.1.8 vector(count) -> __vallocate(count) ->
   // allocator::allocate_at_least returns exactly count, without hidden storage.
   // Other implementations need their own source-level allocation qualification.
#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 220108
   if (_impl->allocation)
      throw exceptions::execution_unavailable{
          "Bounded import projection requires the reviewed libc++ allocation contract"};
#endif
   std::uint64_t charge = static_cast<std::uint64_t>(source.import_function_count) * sizeof(imports[0]);
   for (std::uint32_t i = 0; i < source.import_function_count; ++i) {
      const auto& entry = source.import_functions[i].u.function;
      const auto names = std::strlen(entry.module_name) + std::strlen(entry.field_name);
      if (names > UINT64_MAX - charge)
         throw exceptions::execution_unavailable{"Import projection size overflow"};
      charge += names;
   }
   if (_impl->allocation)
      _impl->allocation->reserve(charge);
   try {
      decltype(imports) projected(source.import_function_count);
      for (std::uint32_t i = 0; i < source.import_function_count; ++i) {
         const auto& entry = source.import_functions[i].u.function;
         auto& result = projected[i];
         result.module_str = std::vector<std::uint8_t>(std::strlen(entry.module_name));
         result.field_str = std::vector<std::uint8_t>(std::strlen(entry.field_name));
         if (!result.module_str.empty())
            std::memcpy(result.module_str.data(), entry.module_name, result.module_str.size());
         if (!result.field_str.empty())
            std::memcpy(result.field_str.data(), entry.field_name, result.field_str.size());
         result.kind = external_kind::Function;
         const auto type = std::find(source.types, source.types + source.type_count, entry.func_type);
         result.type = {static_cast<std::uint32_t>(type - source.types)};
      }
      imports.swap(projected);
      _impl->projection_bytes = charge;
   } catch (const std::bad_alloc&) {
      if (_impl->allocation) {
         _impl->allocation->release(charge);
         auto owner = impl::load_budget(*_impl->allocation);
         owner->fail(detail::engine_load_budget::failure::allocation);
         impl::check_budget(owner);
      }
      throw;
   } catch (...) {
      if (_impl->allocation)
         _impl->allocation->release(charge);
      throw;
   }
}
prepared_module::prepared_module(std::span<const std::uint8_t> source, std::span<const std::uint8_t> native,
                                 const options& policy, std::vector<host_binding> bindings, native_origin origin) {
   if (origin != native_origin::trusted_compiler)
      throw exceptions::profile{"Native execution requires an explicit trusted compiler origin"};
   _impl = impl::make<impl>({}, source, native, policy, std::span{bindings});
   project_imports();
}
prepared_module::prepared_module(std::span<const std::uint8_t> source, std::span<const std::uint8_t> native,
                                 const options& policy, std::span<host_binding> bindings, native_origin origin,
                                 const preparation& allocation) {
   if (origin != native_origin::trusted_compiler)
      throw exceptions::profile{"Native execution requires an explicit trusted compiler origin"};
   _impl = impl::make<impl>(allocation, source, native, policy, bindings, allocation);
   try {
      project_imports();
      allocation.check();
   } catch (...) {
      decltype(imports){}.swap(imports);
      throw;
   }
}
prepared_module::~prepared_module() {
   decltype(imports){}.swap(imports);
}

void prepared_module::validate() const {
   _impl->validate();
}

void prepared_module::impl::validate() const {
   try {
      limits->validate_instantiation();
   } catch (const detail::engine_limits::invalid_instantiation& error) {
      if (error.memory)
         throw exceptions::memory{"WASM active data segment is out of bounds"};
      throw exceptions::interpreter{"WASM active table segment is out of bounds"};
   }
}

void prepared_module::check_limits(const options& policy) const {
   if (policy.max_call_depth > capacity_profile::max_call_depth ||
       (policy.max_pages != UINT32_MAX && policy.max_pages > capacity_profile::max_pages))
      throw exceptions::profile{"Invocation exceeds capacity profile v1"};
   try {
      _impl->engine->check_memory_limit(std::min(policy.max_pages, capacity_profile::max_pages));
   } catch (const std::length_error&) {
      throw exceptions::memory{"WASM initial memory exceeds the caller page limit"};
   }
}

std::uint32_t prepared_module::get_exported_function(std::string_view name) const {
   return _impl->get_exported_function(name);
}

std::uint32_t prepared_module::impl::get_exported_function(std::string_view name) const {
   const auto& module = *reinterpret_cast<const WASMModule*>(source->get());
   for (std::uint32_t i = 0; i < module.export_count; ++i)
      if (module.exports[i].kind == EXPORT_KIND_FUNC &&
          std::string_view{module.exports[i].name, module.exports[i].name_len} == name)
         return module.exports[i].index;
   return UINT32_MAX;
}

prepared_module::function_type prepared_module::get_function_type(std::uint32_t index) const {
   return _impl->get_function_type(index);
}
prepared_module::function_type prepared_module::impl::get_function_type(std::uint32_t index) const {
   const auto type = function_signature(index);
   return {types::func,
           {type.params.begin(), type.params.end()},
           static_cast<std::uint8_t>(type.results.size()),
           type.results.empty() ? static_cast<value_type>(types::ret_void) : type.results.front()};
}
prepared_module::signature_view prepared_module::function_signature(std::uint32_t index) const {
   return _impl->function_signature(index);
}
prepared_module::signature_view prepared_module::impl::function_signature(std::uint32_t index) const {
   try {
      const auto type = source->function_type(index);
      return {type.parameters, type.results};
   } catch (const std::out_of_range&) {
      throw exceptions::interpreter{"Invalid function index"};
   }
}
std::uint32_t prepared_module::function_count() const noexcept {
   const auto& module = *reinterpret_cast<const WASMModule*>(_impl->source->get());
   return module.import_function_count + module.function_count;
}

std::vector<std::string> prepared_module::function_exports() const {
   const auto& module = *reinterpret_cast<const WASMModule*>(_impl->source->get());
   auto result = std::vector<std::string>{};
   for (std::uint32_t i = 0; i < module.export_count; ++i)
      if (module.exports[i].kind == EXPORT_KIND_FUNC)
         result.emplace_back(module.exports[i].name, module.exports[i].name_len);
   return result;
}

std::vector<prepared_module::imported_function> prepared_module::function_imports() const {
   const auto& module = *reinterpret_cast<const WASMModule*>(_impl->source->get());
   std::vector<imported_function> result;
   result.reserve(module.import_function_count);
   for (std::uint32_t i = 0; i < module.import_function_count; ++i) {
      const auto& function = module.import_functions[i].u.function;
      auto type = get_function_type(i);
      host_function signature{std::move(type.param_types), {}};
      if (type.return_count)
         signature.ret.push_back(type.return_type);
      result.push_back({function.module_name, function.field_name, std::move(signature)});
   }
   return result;
}

std::size_t prepared_module::code_size() const noexcept {
   return _impl->bytes;
}
bool prepared_module::has_linear_memory() const noexcept {
   return _impl->has_linear_memory();
}
bool prepared_module::impl::has_linear_memory() const noexcept {
   return reinterpret_cast<const WASMModule*>(source->get())->memory_count != 0;
}

std::vector<std::uint32_t> prepared_module::native_function_stack_bytes() const {
   if (_impl->engine->get()->module_type != Wasm_Module_AoT && !_impl->engine->llvm_jit_ready())
      return {};
   const auto* module = reinterpret_cast<const WASMModule*>(_impl->source->get());
   std::vector<std::uint32_t> result(function_count(), 0);
   for (std::uint32_t i = module->import_function_count; i < result.size(); ++i)
      result[i] = native_function_stack_bytes(i);
   return result;
}
std::uint32_t prepared_module::native_function_stack_bytes(std::uint32_t index) const {
   if (index >= function_count())
      throw exceptions::interpreter{"Invalid function index"};
   const auto& module = *reinterpret_cast<const WASMModule*>(_impl->source->get());
   if (index < module.import_function_count ||
       (_impl->engine->get()->module_type != Wasm_Module_AoT && !_impl->engine->llvm_jit_ready()))
      return 0;
   try {
      const auto sizes = _impl->engine->native_stack_bytes();
      if (sizes.size() != module.function_count || sizes[index - module.import_function_count] == UINT32_MAX)
         throw exceptions::profile{"Native artifact lacks complete native stack metadata"};
      return sizes[index - module.import_function_count];
   } catch (const std::invalid_argument&) {
      throw exceptions::profile{"Native artifact lacks complete native stack metadata"};
   }
}

std::span<const std::byte> prepared_module::native_code() const noexcept {
   if (_impl->engine->get()->module_type != Wasm_Module_AoT)
      return {};
   const auto& module = *reinterpret_cast<const AOTModule*>(_impl->engine->get());
   return {reinterpret_cast<const std::byte*>(module.code), module.code_size};
}

prepared_module::memory_accounting prepared_module::accounted_module_memory() const {
   WASMModuleMemConsumption source{};
   wasm_get_module_mem_consumption(reinterpret_cast<const WASMModule*>(_impl->source->get()), &source);
   memory_accounting result{_impl->source->owned_buffer_bytes(), 0, source.total_size, 0};
   if (_impl->engine != _impl->source) {
      WASMModuleMemConsumption native{};
      aot_get_module_mem_consumption(reinterpret_cast<const AOTModule*>(_impl->engine->get()), &native);
      result.native_buffer_bytes = _impl->engine->owned_buffer_bytes();
      result.upstream_native_bytes = native.total_size;
   }
   return result;
}

} // namespace forge::vm::wasm::interpret
