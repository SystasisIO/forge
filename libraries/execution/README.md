# Execution Libraries

`forge::execution` is an empty family namespace. Each execution substrate owns
its limits and invocation contract; scheduling and product policy belong to
consumers. There is no aggregate target or module.

- [WASM](wasm/README.md): bounded synchronous Contract SDK actions over the
  existing Forge interpreter.
