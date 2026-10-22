#pragma once

#include <string_view>

import forge.vm.wasm.optimizing.artifact;

// Private test runner; the existing executable owns decoding/provenance.
struct operational_oracles {
   static void run(std::string_view mode, forge::vm::wasm::optimizing::artifact code);
};
