add_library(forge_vm_wasm_execution STATIC
   compiled_module.cpp
   compiled_module_impl.cpp
   intrinsics.cpp
   limits.cpp
)
target_sources(forge_vm_wasm_execution PUBLIC FILE_SET CXX_MODULES
   BASE_DIRS include FILES
      include/forge/vm/wasm/execution/compiled_module.cppm
      include/forge/vm/wasm/execution/exceptions.cppm
      include/forge/vm/wasm/execution/invocation.cppm
      include/forge/vm/wasm/execution/limits.cppm
      include/forge/vm/wasm/execution/profile.cppm
)
configure_file("${PROJECT_SOURCE_DIR}/guest/cmake/types.h.in"
   "${CMAKE_CURRENT_BINARY_DIR}/generated/forge/contract/types.h" COPYONLY)
target_include_directories(forge_vm_wasm_execution PRIVATE
   "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/guest/libraries/contract/include>"
   "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
)
target_link_libraries(forge_vm_wasm_execution PUBLIC
   forge_vm_wasm_interpret forge_chain_protocol forge_raw forge_exceptions
)
target_compile_features(forge_vm_wasm_execution PUBLIC cxx_std_23)
