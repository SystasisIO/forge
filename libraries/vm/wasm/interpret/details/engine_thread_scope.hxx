#pragma once

namespace forge::vm::wasm::interpret::detail {

class engine_thread_scope {
 public:
   engine_thread_scope();
   ~engine_thread_scope();
   engine_thread_scope(const engine_thread_scope&) = delete;
   engine_thread_scope& operator=(const engine_thread_scope&) = delete;

 private:
   bool _owned;
};

} // namespace forge::vm::wasm::interpret::detail
