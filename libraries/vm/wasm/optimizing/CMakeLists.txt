add_library(
   forge_vm_wasm_optimizing STATIC
   artifact.cpp
   compiler_protocol.cpp
   compile_request.cpp
   executor.cpp
   executor_impl.cpp
   executor_epoch.cpp
   native_image.cpp
   native_stack.cpp
)
forge_target_modules_at(forge_vm_wasm_optimizing vm/wasm/optimizing)
target_include_directories(forge_vm_wasm_optimizing PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_vm_wasm_optimizing
   PUBLIC
      forge_exceptions
      forge_vm_wasm_interpret
      forge_crypto_digest
      forge_raw
      Boost::headers
      forge_asio
)

# The narrow artifact-only auxiliary graph deliberately excludes this module.
# Every normal optimizing build owns the real injected-runtime session source.
if(NOT "compiler_session" IN_LIST FORGE_vm_wasm_optimizing_DISABLED_MODULES)
   find_package(Boost 1.90 REQUIRED CONFIG COMPONENTS process)
   target_sources(forge_vm_wasm_optimizing PRIVATE
      compiler_session.cpp
      compiler_session_impl.cpp
      compiler_session_impl_initialize.cpp
      compiler_session_impl_cache.cpp
      compiler_session_impl_child.cpp
      compiler_session_impl_process.cpp
      compiler_session_impl_worker.cpp
      compiler_session_ticket_state.cpp
   )
   target_link_libraries(forge_vm_wasm_optimizing PRIVATE Boost::process forge_core forge_crypto_core forge_codec_hex)
   if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      include(CheckCXXSourceCompiles)
      check_cxx_source_compiles("\n#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n#include <spawn.h>\nint main() { auto f = &posix_spawn_file_actions_addclosefrom_np; (void)f; }"
         FORGE_HAS_POSIX_SPAWN_CLOSEFROM)
      if(NOT FORGE_HAS_POSIX_SPAWN_CLOSEFROM)
         message(FATAL_ERROR "WASM compiler session requires public posix_spawn closefrom support")
      endif()
      target_compile_definitions(forge_vm_wasm_optimizing PRIVATE _GNU_SOURCE FORGE_HAS_POSIX_SPAWN_CLOSEFROM=1)
   endif()
endif()
