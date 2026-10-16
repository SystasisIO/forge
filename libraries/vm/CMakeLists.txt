add_subdirectory(wasm/interpret)
if(FORGE_ENABLE_VM_WASM_OPTIMIZING)
   add_subdirectory(wasm/optimizing)
endif()
