#include "details/engine_bindings.hxx"
#include "details/engine_memory.hxx"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <tuple>
#include <utility>
#include <aot_runtime.h>

namespace forge::vm::wasm::interpret::detail {

engine_bindings::function::function(std::pmr::memory_resource* resource)
    : module(resource), name(resource), parameters(resource) {}

engine_bindings::engine_bindings(std::vector<function> functions)
    : engine_bindings(std::pmr::vector<function>{std::make_move_iterator(functions.begin()),
                                                 std::make_move_iterator(functions.end())},
                      nullptr, nullptr) {}

engine_bindings::engine_bindings(std::pmr::vector<function> functions, const void* context,
                                 void (*invoke)(const void*, std::size_t, call&))
    : _functions(std::move(functions)), _context(context), _invoke(invoke) {
   for (std::size_t i = 0; i < _functions.size(); ++i) {
      _functions[i].index = i;
      _functions[i].owner = this;
   }
   std::sort(_functions.begin(), _functions.end(),
             [](const auto& a, const auto& b) { return std::tie(a.module, a.name) < std::tie(b.module, b.name); });
   const auto numeric = [](wasm_valkind_t type) {
      return type == WASM_I32 || type == WASM_I64 || type == WASM_F32 || type == WASM_F64;
   };
   for (std::size_t i = 0; i < _functions.size(); ++i) {
      const auto& item = _functions[i];
      if ((!item.invoke && !_invoke) || item.module.empty() || item.name.empty() ||
          item.module.find('\0') != std::string::npos || item.name.find('\0') != std::string::npos ||
          !std::all_of(item.parameters.begin(), item.parameters.end(), numeric) ||
          (item.result && !numeric(*item.result)))
         throw link_error("Invalid WASM host registration");
      if (i && std::tie(item.module, item.name) == std::tie(_functions[i - 1].module, _functions[i - 1].name))
         throw link_error("Duplicate WASM host registration");
   }
}

void engine_bindings::bind(wasm_module_t module) const {
   const auto count = wasm_runtime_get_import_count(module);
   if (count < 0)
      throw link_error("Cannot inspect WASM imports");
   for (std::int32_t i = 0; i < count; ++i) {
      wasm_import_t item{};
      wasm_runtime_get_import_type(module, i, &item);
      if (item.kind != WASM_IMPORT_EXPORT_KIND_FUNC)
         throw link_error("Only function imports are admitted");
      const auto key = std::make_tuple(std::string_view(item.module_name), std::string_view(item.name));
      const auto found = std::lower_bound(_functions.begin(), _functions.end(), key,
                                          [](const auto& a, const auto& b) { return std::tie(a.module, a.name) < b; });
      if (found == _functions.end() || std::tie(found->module, found->name) != key)
         throw link_error("WASM import is not in the supplied host profile");
      const auto type = item.u.func_type;
      if (wasm_func_type_get_param_count(type) != found->parameters.size() ||
          wasm_func_type_get_result_count(type) != (found->result ? 1U : 0U))
         throw link_error("WASM import signature mismatch");
      for (std::uint32_t p = 0; p < found->parameters.size(); ++p)
         if (wasm_func_type_get_param_valkind(type, p) != found->parameters[p])
            throw link_error("WASM import parameter mismatch");
      if (found->result && wasm_func_type_get_result_valkind(type, 0) != *found->result)
         throw link_error("WASM import result mismatch");

      // The module was loaded with no_resolve. Bind its own function slots;
      // never publish or temporarily replace process-global native names.
      const auto install = [&](auto& slot) {
         slot.func_ptr_linked = reinterpret_cast<void*>(&dispatch);
         slot.signature = nullptr; // Guest pointers remain numeric offsets.
         slot.attachment = const_cast<function*>(&*found);
         slot.call_conv_raw = true;
         slot.call_conv_wasm_c_api = false;
      };
      if (module->module_type == Wasm_Module_Bytecode)
         install(reinterpret_cast<WASMModule*>(module)->import_functions[i].u.function);
      else if (module->module_type == Wasm_Module_AoT)
         install(reinterpret_cast<AOTModule*>(module)->import_funcs[i]);
      else
         throw link_error("Unsupported WASM module kind");
   }
}

engine_bindings::call::call(wasm_exec_env_t env, state& context, const function& function, std::uint64_t* arguments)
    : _env(env), _state(context), _function(function), _arguments(arguments) {}

std::span<char> engine_bindings::call::memory(std::uint32_t offset, std::uint32_t size) const {
   const auto instance = wasm_runtime_get_module_inst(_env);
   const auto memory = wasm_runtime_get_memory(instance, 0);
   if (!memory)
      throw memory_error{};
   const std::uint64_t capacity =
       static_cast<std::uint64_t>(wasm_memory_get_cur_page_count(memory)) * wasm_memory_get_bytes_per_page(memory);
   if (offset > capacity || size > capacity - offset)
      throw memory_error{};
   // A zero-sized view at the end is valid, even though WAMR's pointer mapper
   // deliberately refuses the one-past-end address.
   if (size == 0)
      return {};
   auto* data = static_cast<char*>(wasm_runtime_addr_app_to_native(instance, offset));
   if (!data)
      throw memory_error{};
   return {data, size};
}

std::span<char> engine_bindings::call::memory() const {
   const auto instance = wasm_runtime_get_module_inst(_env);
   const auto memory = wasm_runtime_get_memory(instance, 0);
   if (!memory)
      return {};
   const auto size =
       static_cast<std::uint64_t>(wasm_memory_get_cur_page_count(memory)) * wasm_memory_get_bytes_per_page(memory);
   auto* base = static_cast<char*>(wasm_memory_get_base_address(memory));
   if (!base && !size)
      base = static_cast<char*>(engine_memory::current_base());
   return {base, static_cast<std::size_t>(size)};
}

std::string_view engine_bindings::call::string(std::uint32_t offset, std::uint32_t max_bytes) const {
   const auto instance = wasm_runtime_get_module_inst(_env);
   const auto memory = wasm_runtime_get_memory(instance, 0);
   if (!memory)
      throw memory_error{};
   const std::uint64_t capacity =
       static_cast<std::uint64_t>(wasm_memory_get_cur_page_count(memory)) * wasm_memory_get_bytes_per_page(memory);
   if (offset >= capacity || max_bytes == 0)
      throw memory_error{};
   const auto view =
       this->memory(offset, static_cast<std::uint32_t>(std::min<std::uint64_t>(max_bytes, capacity - offset)));
   const auto end = static_cast<const char*>(std::memchr(view.data(), 0, view.size()));
   if (!end)
      throw memory_error{};
   return {view.data(), static_cast<std::size_t>(end - view.data())};
}

void* engine_bindings::call::host() const noexcept {
   return _state.host;
}
std::stop_token engine_bindings::call::stop() const noexcept {
   return _state.stop;
}
void engine_bindings::call::check_stop() const {
   if (_state.stop.stop_requested())
      throw canceled{};
}
std::size_t engine_bindings::call::parameter_count() const noexcept {
   return _function.parameters.size();
}
wasm_valkind_t engine_bindings::call::parameter_kind(std::size_t index) const {
   return _function.parameters.at(index);
}
std::optional<wasm_valkind_t> engine_bindings::call::result_kind() const noexcept {
   return _function.result;
}

void engine_bindings::dispatch(wasm_exec_env_t env, std::uint64_t* arguments) noexcept {
   const auto instance = wasm_runtime_get_module_inst(env);
   if (engine_memory::current_failed()) {
      wasm_runtime_set_exception(instance, "Local linear-memory allocation failed");
      return;
   }
   auto* context = static_cast<state*>(wasm_runtime_get_custom_data(instance));
   auto* function = static_cast<const engine_bindings::function*>(wasm_runtime_get_function_attachment(env));
   if (!context || !function) {
      wasm_runtime_set_exception(instance, "Missing Forge host invocation context");
      return;
   }
   if (env->forge_call_depth >= env->forge_max_call_depth) {
      env->forge_depth_exceeded = 1;
      wasm_runtime_set_exception(instance, "Forge logical call depth exceeded");
      return;
   }
   const auto incoming_depth = env->forge_call_depth++;
   if (context->stop.stop_requested()) {
      wasm_runtime_set_exception(instance, "Forge invocation canceled");
      env->forge_call_depth = incoming_depth;
      return;
   }
   try {
      if (context->failure)
         std::rethrow_exception(context->failure);
      call value(env, *context, *function, arguments);
      if (function->owner->_invoke)
         function->owner->_invoke(function->owner->_context, function->index, value);
      else
         function->invoke(value);
      if (function->result && !value._returned)
         throw std::logic_error("Host did not return its declared result");
   } catch (...) {
      // Keep the original typed exception for the outer C++ boundary. Nothing
      // unwinds through WAMR's C interpreter or generated AOT frames.
      if (!context->failure)
         context->failure = std::current_exception();
      wasm_runtime_set_exception(instance, "Forge host invocation failed");
   }
   env->forge_call_depth = incoming_depth;
}

} // namespace forge::vm::wasm::interpret::detail
