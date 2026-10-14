add_library(forge_execution_wasm STATIC
   compiled_module.cpp
   compiled_module_impl.cpp
   intrinsics.cpp
   limits.cpp
)
target_sources(forge_execution_wasm PUBLIC FILE_SET CXX_MODULES
   BASE_DIRS include FILES
      include/forge/execution/wasm/compiled_module.cppm
      include/forge/execution/wasm/exceptions.cppm
      include/forge/execution/wasm/invocation.cppm
      include/forge/execution/wasm/limits.cppm
      include/forge/execution/wasm/profile.cppm
)
configure_file("${PROJECT_SOURCE_DIR}/guest/cmake/types.h.in"
   "${CMAKE_CURRENT_BINARY_DIR}/generated/forge/contract/types.h" COPYONLY)
target_include_directories(forge_execution_wasm PRIVATE
   "${PROJECT_SOURCE_DIR}/guest/libraries/contract/include"
   "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(forge_execution_wasm PUBLIC
   forge_vm_wasm_interpret forge_chain_protocol forge_raw forge_exceptions
)
target_compile_features(forge_execution_wasm PUBLIC cxx_std_23)
