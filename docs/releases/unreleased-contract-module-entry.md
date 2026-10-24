# Contract module entry

`forge_add_contract()` accepts the additive `DISPATCH_MODULE` option for an
annotated contract class in an exported C++ module. The SDK generates its
import source in the build directory and retains the existing compiler,
Abigen, dispatcher, ABI and runtime-manifest pipeline. Existing `SOURCES`
consumers keep their behavior; no wire or persisted formats change.

Migration: remove an import-only product entry source, keep the annotated
class and its implementation in the existing contract library, and replace
`SOURCES entry.cpp` with `DISPATCH_MODULE <primary.module>`.
The option cannot be combined with `SOURCES` or `DISPATCH_SOURCE`.
