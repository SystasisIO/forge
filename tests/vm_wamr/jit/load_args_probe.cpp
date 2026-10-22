#include <cstddef>
#include <type_traits>

#if FORGE_WAMR_C_API_FIRST
#include <wasm_c_api.h>
#include <wasm_export.h>
#else
#include <wasm_export.h>
#include <wasm_c_api.h>
#endif

static_assert(std::is_standard_layout_v<LoadArgs>);
static_assert(std::is_same_v<decltype(LoadArgs::defer_jit), bool>);
static_assert(std::is_same_v<decltype(LoadArgs::load_budget), WASMLoadBudget>);
static_assert(LoadArgs{}.block_type_mode == WASM_BLOCK_TYPES_STANDARD);
static_assert(!LoadArgs{}.defer_jit && !LoadArgs{}.no_resolve);
static_assert(!LoadArgs{}.load_budget.begin_mapping && !LoadArgs{}.load_budget.complete_mapping);
static_assert(!LoadArgs{}.load_budget.unmap);

#if FORGE_WAMR_C_API_FIRST
extern "C" void jit_load_args_capi_first(std::size_t* result)
#else
extern "C" void jit_load_args_export_first(std::size_t* result)
#endif
{
   const std::size_t layout[]{sizeof(LoadArgs),
                              alignof(LoadArgs),
                              offsetof(LoadArgs, name),
                              offsetof(LoadArgs, clone_wasm_binary),
                              offsetof(LoadArgs, wasm_binary_freeable),
                              offsetof(LoadArgs, no_resolve),
                              offsetof(LoadArgs, block_type_mode),
                              offsetof(LoadArgs, load_budget),
                              offsetof(LoadArgs, defer_jit),
                              sizeof(WASMLoadBudget),
                              alignof(WASMLoadBudget),
                              offsetof(WASMLoadBudget, context),
                              offsetof(WASMLoadBudget, begin_mapping),
                              offsetof(WASMLoadBudget, complete_mapping),
                              offsetof(WASMLoadBudget, unmap),
                              offsetof(WASMLoadBudget, check),
                              offsetof(WASMLoadBudget, before_protect),
                              offsetof(WASMLoadBudget, allocation_failed),
                              offsetof(WASMLoadBudget, protection_failed)};
   for (std::size_t index = 0; index != sizeof(layout) / sizeof(*layout); ++index)
      result[index] = layout[index];
}
