/* Header-shape probes only; actual execution uses the unchanged runtime profile. */
#if FORGE_FP_LAYOUT_PROFILE == 1
#undef WASM_ENABLE_INTERP
#undef WASM_ENABLE_AOT
#undef WASM_ENABLE_JIT
#define WASM_ENABLE_INTERP 0
#define WASM_ENABLE_AOT 1
#define WASM_ENABLE_JIT 0
#elif FORGE_FP_LAYOUT_PROFILE == 2
#undef WASM_ENABLE_INTERP
#undef WASM_ENABLE_AOT
#undef WASM_ENABLE_JIT
#define WASM_ENABLE_INTERP 1
#define WASM_ENABLE_AOT 0
#define WASM_ENABLE_JIT 0
#endif
#include "fp_status_layout.h"
