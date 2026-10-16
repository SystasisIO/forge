export module forge.tooling.wasm_compiler.compiler;

export import forge.vm.wasm.optimizing.artifact;

export namespace forge::tooling::wasm_compiler {

// Synchronous CPU work for the isolated compiler worker, never invocation
// code. The runtime/session owner supplies its asynchronous process boundary.
class compiler {
 public:
   vm::wasm::optimizing::artifact compile(const vm::wasm::optimizing::compile_request& request) const;
};

} // namespace forge::tooling::wasm_compiler
