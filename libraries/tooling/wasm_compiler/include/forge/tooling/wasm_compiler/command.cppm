export module forge.tooling.wasm_compiler.command;

export namespace forge::tooling::wasm_compiler::command {

// Fresh isolated helper only. No shell, environment discovery or WASM argv.
int run(int argc, const char* const* argv);

} // namespace forge::tooling::wasm_compiler::command
