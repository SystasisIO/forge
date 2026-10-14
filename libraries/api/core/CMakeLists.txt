add_library(forge_api_core_values STATIC)
target_sources(forge_api_core_values PUBLIC
   FILE_SET CXX_MODULES
   BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/forge/api/core/types.cppm"
)
target_compile_features(forge_api_core_values PUBLIC cxx_std_23)
target_include_directories(forge_api_core_values PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_api_core_values PUBLIC forge_raw Boost::headers)
forge_target_contract_guest_component(forge_api_core_values ID forge.api.core_values)

if(FORGE_CONTRACT_SDK_HOST_PROFILE)
   return()
endif()

add_library(
   forge_api_core STATIC
   binding.cpp
   call_options.cpp
   call_state.cpp
   contextual_handler_gate_state.cpp
   context.cpp
   dispatcher.cpp
   descriptor.cpp
   error_projection.cpp
   invocation_context.cpp
   registry.cpp
   stream_reader.cpp
   stream_state.cpp
   trusted_invocation.cpp
)
set(FORGE_api_core_DISABLED_MODULES types)
forge_target_modules_at(forge_api_core api/core)
unset(FORGE_api_core_DISABLED_MODULES)

target_include_directories(forge_api_core PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_api_core PUBLIC
      forge_api_core_values
      forge_exceptions
      forge_raw
      forge_reflect
      Boost::headers
      Boost::asio
)
