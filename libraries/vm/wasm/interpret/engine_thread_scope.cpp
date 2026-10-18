#include "details/engine_thread_scope.hxx"

#include <stdexcept>
#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

engine_thread_scope::engine_thread_scope() : _owned(!wasm_runtime_thread_env_inited()) {
   if (_owned && !wasm_runtime_init_thread_env()) {
      throw std::runtime_error("WASM thread initialization failed");
   }
}

engine_thread_scope::~engine_thread_scope() {
   if (_owned) {
      wasm_runtime_destroy_thread_env();
   }
}

} // namespace forge::vm::wasm::interpret::detail
