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
forge_target_modules_at(forge_api_core api/core)

target_include_directories(forge_api_core PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_api_core PUBLIC
      forge_exceptions
      forge_raw
      forge_reflect
      Boost::headers
      Boost::asio
)
