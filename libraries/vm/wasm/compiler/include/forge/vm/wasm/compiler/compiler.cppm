module;

#include <string_view>

export module forge.vm.wasm.compiler.compiler;

export import forge.vm.wasm.optimizing.artifact;

export namespace forge::vm::wasm::compiler {

// Isolated-process code generation. This owner creates no scheduler or cache.
class compiler {
 public:
   static std::string_view llvm_version() noexcept;
   optimizing::artifact compile(const optimizing::compile_request&) const;
};

} // namespace forge::vm::wasm::compiler
