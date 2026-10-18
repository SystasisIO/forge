#include "details/engine_instance.hxx"
#include "details/engine_thread_scope.hxx"
#include <wasm_exec_env.h>
#include <wasm_runtime.h>
#include <aot_runtime.h>

#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace forge::vm::wasm::interpret::detail {
namespace {

constexpr auto no_fp_failure = WASM_FP_FAILURE_NONE;

class native_execution_scope {
 public:
   explicit native_execution_scope(wasm_module_inst_t instance) {
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
      _native = instance->module_type == Wasm_Module_AoT || wasm_runtime_get_running_mode(instance) == Mode_LLVM_JIT;
      if (_native) {
         if (!os_thread_jit_write_protect_supported())
            throw engine_instance::unavailable{};
         _previous = os_thread_jit_write_protect_state();
         os_thread_jit_write_protect_np(true);
      }
#else
      (void)instance;
#endif
   }
   ~native_execution_scope() {
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
      if (_native)
         os_thread_jit_write_protect_np(_previous);
#endif
   }
   native_execution_scope(const native_execution_scope&) = delete;
   native_execution_scope& operator=(const native_execution_scope&) = delete;

 private:
#if defined(BH_PLATFORM_DARWIN) && defined(BUILD_TARGET_AARCH64)
   bool _native = false;
   bool _previous = true;
#endif
};

std::size_t cell_count(const std::vector<wasm_valkind_t>& types) {
   std::size_t result = 0;
   for (const auto type : types) {
      switch (type) {
      case WASM_I32:
      case WASM_F32:
         result += 1;
         break;
      case WASM_I64:
      case WASM_F64:
         result += 2;
         break;
      default:
         throw std::invalid_argument("Unsupported WASM value kind");
      }
   }
   return result;
}

} // namespace

engine_instance::engine_instance(std::shared_ptr<engine_module> source, std::uint32_t stack_bytes, std::stop_token stop,
                                 void* custom_data, std::uint32_t max_pages, std::uint32_t max_call_depth,
                                 std::exception_ptr* host_failure, std::byte* native_stack_boundary,
                                 engine_memory* memory, RunningMode mode, const std::exception_ptr* normal_exit)
    : _source(std::move(source)), _host_state{custom_data, stop, {}}, _normal_exit(normal_exit),
      _system_memory(engine_memory::system()), _memory(memory ? memory : &_system_memory) {
   if (!_source || stack_bytes == 0 || max_pages > UINT32_MAX / 65536) {
      throw std::invalid_argument("Invalid WASM instance configuration");
   }
   if (mode != Mode_Interp && mode != Mode_LLVM_JIT)
      throw std::invalid_argument("Unsupported WASM instance execution mode");
   if (mode == Mode_LLVM_JIT && (_source->get()->module_type != Wasm_Module_Bytecode || !_source->llvm_jit_ready() ||
                                 !wasm_runtime_is_running_mode_supported(Mode_LLVM_JIT)))
      throw unavailable{};
   const engine_thread_scope thread;
   const engine_memory::scope memory_scope(*_memory);
   _memory->bind_instance(nullptr);
   _memory->clear_failure();
   if (stop.stop_requested())
      throw canceled{};
   _source->check_memory_limit(max_pages);
   InstantiationArgs2* args_pointer{};
   if (!wasm_runtime_instantiation_args_create(&args_pointer)) {
      throw std::runtime_error("WASM instantiation arguments allocation failed");
   }
   const std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)> args(
       args_pointer, wasm_runtime_instantiation_args_destroy);
   wasm_runtime_instantiation_args_set_default_stack_size(args.get(), stack_bytes);
   if (const auto limit = _source->memory_page_limit(max_pages))
      wasm_runtime_instantiation_args_set_max_memory_pages(args.get(), *limit);
   wasm_runtime_instantiation_args_set_defer_start(args.get(), true);
   wasm_runtime_instantiation_args_set_custom_data(args.get(), _source->has_bindings() ? &_host_state : custom_data);
   std::array<char, 512> diagnostic{};
   _instance = wasm_runtime_instantiate_ex2(_source->get(), args.get(), diagnostic.data(), diagnostic.size());
   if (!_instance) {
      if (stop.stop_requested())
         throw canceled{};
      if (_memory->failed())
         throw unavailable{};
      throw std::runtime_error(std::string("WASM instantiation failed: ") + diagnostic.data());
   }
   try {
      _memory->bind_instance(_instance);
      if (_source->get()->module_type == Wasm_Module_Bytecode &&
          (!wasm_runtime_set_running_mode(_instance, mode) || wasm_runtime_get_running_mode(_instance) != mode))
         throw unavailable{};
      // A zero override means "unset" upstream. Apply the logical byte budget
      // to this instance before deferred guest initialization, including zero.
      for (std::uint32_t index = 0; auto* memory = wasm_runtime_get_memory(_instance, index); ++index) {
         const auto pages = static_cast<std::uint64_t>(max_pages) * 65536 / memory->num_bytes_per_page;
         memory->max_page_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(memory->max_page_count, pages));
      }
      _env = wasm_runtime_create_exec_env(_instance, stack_bytes);
      if (!_env) {
         throw std::runtime_error("WASM execution environment allocation failed");
      }
      _env->forge_max_call_depth = max_call_depth;
      if (native_stack_boundary)
         wasm_runtime_set_native_stack_boundary(_env, reinterpret_cast<std::uint8_t*>(native_stack_boundary));
      // Register only after the instance and exec environment exist, before any
      // guest initialization. Callback destruction precedes instance teardown.
      const std::stop_callback callback(stop, [this] { cancel(); });
      if (stop.stop_requested())
         throw canceled{};
      const native_execution_scope execution(_instance);
      _env->forge_fp_failure = no_fp_failure;
      const bool initialized = wasm_runtime_initialize_with_exec_env(_instance, _env);
      const bool normal_exit_requested = is_normal_exit();
      if (normal_exit_requested)
         _host_state.failure = {};
      if (_host_state.failure)
         std::rethrow_exception(_host_state.failure);
      if (_env->forge_fp_failure != no_fp_failure)
         throw fp_failure{};
      if (stop.stop_requested())
         throw canceled{};
      if (_env->forge_depth_exceeded)
         throw depth_exceeded{};
      if (_memory->failed())
         throw unavailable{};
      if (!initialized && !normal_exit_requested) {
         throw initialization_trap{engine_exception::inspect(_instance)};
      }
      if (normal_exit_requested)
         wasm_runtime_clear_exception(_instance);
   } catch (...) {
      if (host_failure)
         *host_failure = _host_state.failure;
      _memory->bind_instance(nullptr);
      if (_env)
         wasm_runtime_destroy_exec_env(_env);
      wasm_runtime_deinstantiate(_instance);
      throw;
   }
}

engine_instance::~engine_instance() {
   const engine_memory::scope memory_scope(*_memory);
   _memory->bind_instance(nullptr);
   wasm_runtime_destroy_exec_env(_env);
   wasm_runtime_deinstantiate(_instance);
}

wasm_module_inst_t engine_instance::get() const noexcept {
   return _instance;
}
wasm_exec_env_t engine_instance::env() const noexcept {
   return _env;
}

bool engine_instance::invoke(const char* name, std::uint32_t argument_cells, std::span<std::uint32_t> cells,
                             std::stop_token stop) {
   const engine_thread_scope thread;
   if (!name)
      throw std::invalid_argument("Invalid WASM export name");
   return invoke_function(wasm_runtime_lookup_function(_instance, name), argument_cells, cells, stop);
}

bool engine_instance::invoke(std::uint32_t index, std::uint32_t argument_cells, std::span<std::uint32_t> cells,
                             std::stop_token stop, std::optional<void*> host) {
   const std::unique_lock admission(_execution, std::try_to_lock);
   if (!admission.owns_lock())
      throw std::logic_error("WASM instance already executing");
   const engine_thread_scope thread;
   wasm_function_inst_t function{};
   if (_source->get()->module_type == Wasm_Module_Bytecode) {
      const auto instance = reinterpret_cast<WASMModuleInstance*>(_instance);
      if (index < instance->e->function_count)
         function = &instance->e->functions[index];
   } else {
      const auto module = reinterpret_cast<const AOTModule*>(_source->get());
      if (index >= static_cast<std::uint64_t>(module->import_func_count) + module->func_count)
         throw std::out_of_range("WASM function index is outside its bounds");
      function = reinterpret_cast<wasm_function_inst_t>(
          aot_get_function_instance(reinterpret_cast<AOTModuleInstance*>(_instance), index));
      if (!function)
         throw unavailable{};
   }
   return invoke_function(function, argument_cells, cells, stop, host);
}

bool engine_instance::invoke_indirect(std::uint32_t index, std::uint32_t argument_cells, std::span<std::uint32_t> cells,
                                      std::stop_token stop) {
   // Resolve table entries through the engine; never reinterpret an AOT entry as
   // a generated-code function with a different native ABI.
   const auto module = _source->get();
   if (module->module_type != Wasm_Module_Bytecode)
      throw std::invalid_argument("Direct table invocation requires bytecode metadata");
   const auto instance = reinterpret_cast<WASMModuleInstance*>(_instance);
   if (!instance->table_count || index >= instance->tables[0]->cur_size)
      throw std::out_of_range("WASM table element is outside its bounds");
   const auto function_index = instance->tables[0]->elems[index];
   if (function_index == UINT32_MAX)
      throw std::out_of_range("WASM table element is uninitialized");
   return invoke(function_index, argument_cells, cells, stop);
}

bool engine_instance::invoke_function(wasm_function_inst_t function, std::uint32_t argument_cells,
                                      std::span<std::uint32_t> cells, std::stop_token stop, std::optional<void*> host) {
   const std::unique_lock admission(_execution, std::try_to_lock);
   if (!admission.owns_lock()) {
      throw std::logic_error("WASM instance already executing");
   }
   if (cells.empty() || argument_cells > cells.size()) {
      throw std::invalid_argument("Invalid WASM call buffer");
   }
   const engine_thread_scope thread;
   if (!function) {
      throw std::invalid_argument("WASM export is missing");
   }
   const auto params = wasm_func_get_param_count(function, _instance);
   const auto results = wasm_func_get_result_count(function, _instance);
   if (params > argument_cells || results > cells.size()) {
      throw std::invalid_argument("WASM call buffer is too small");
   }
   std::vector<wasm_valkind_t> param_types(params), result_types(results);
   wasm_func_get_param_types(function, _instance, param_types.data());
   wasm_func_get_result_types(function, _instance, result_types.data());
   if (cell_count(param_types) != argument_cells || cell_count(result_types) > cells.size()) {
      throw std::invalid_argument("WASM call does not match the export signature");
   }
   const bool outermost = _nesting == 0;
   const engine_memory::scope memory_scope(*_memory);
   if (outermost) {
      _memory->clear_failure();
      wasm_runtime_clear_exception(_instance);
      _env->forge_depth_exceeded = 0;
      _env->forge_fp_failure = no_fp_failure;
      _host_state.failure = {};
   } else if (_host_state.failure)
      std::rethrow_exception(_host_state.failure);
   const auto previous_host = _host_state.host;
   const auto previous_stop = _host_state.stop;
   ++_nesting;
   const auto restore = [this, previous_host, previous_stop](void*) {
      --_nesting;
      _host_state.host = previous_host;
      _host_state.stop = previous_stop;
   };
   const std::unique_ptr<void, decltype(restore)> frame(this, restore);
   if (host)
      _host_state.host = *host;
   _host_state.stop = stop;
   const std::stop_callback callback(stop, [this] { cancel(); });
   if (stop.stop_requested())
      throw canceled{};
   const native_execution_scope execution(_instance);
   const bool result = wasm_runtime_call_wasm(_env, function, argument_cells, cells.data());
   auto normal_exit_failure = std::exception_ptr{};
   if (is_normal_exit())
      normal_exit_failure = std::exchange(_host_state.failure, {});
   if (_host_state.failure)
      std::rethrow_exception(_host_state.failure);
   if (_env->forge_fp_failure != no_fp_failure)
      throw fp_failure{};
   if (stop.stop_requested())
      throw canceled{};
   if (_env->forge_depth_exceeded)
      throw depth_exceeded{};
   if (_memory->failed())
      throw unavailable{};
   if (normal_exit_failure) {
      _host_state.failure = normal_exit_failure;
      std::rethrow_exception(normal_exit_failure);
   }
   return result;
}

void engine_instance::cancel() noexcept {
   wasm_runtime_terminate(_instance);
}

std::exception_ptr engine_instance::host_failure() const noexcept {
   return _host_state.failure;
}

bool engine_instance::is_normal_exit() const noexcept {
   return _normal_exit && *_normal_exit && _host_state.failure == *_normal_exit;
}

bool engine_instance::consume_normal_exit(const std::exception_ptr& expected) noexcept {
   if (!expected || _host_state.failure != expected || _env->forge_fp_failure != no_fp_failure)
      return false;
   _host_state.failure = {};
   wasm_runtime_clear_exception(_instance);
   return true;
}

} // namespace forge::vm::wasm::interpret::detail
