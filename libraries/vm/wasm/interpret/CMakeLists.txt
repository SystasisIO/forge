add_library(
   forge_vm_wasm_interpret STATIC
   allocator.cpp
   backend.cpp
   debug_info.cpp
   floating_point_environment.cpp
   signals.cpp
   types.cpp
   watchdog.cpp
   watchdog_stop_callback.cpp
)

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
   find_program(FORGE_VM_WASM_INTERPRET_MIG mig REQUIRED)
   set(mach_protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/mach_protocol")
   set(mach_sdk_dir "${CMAKE_OSX_SYSROOT}/usr/include/mach")
   if(NOT EXISTS "${mach_sdk_dir}/mach_exc.defs")
      message(FATAL_ERROR "Darwin recovery requires public mach_exc.defs in CMAKE_OSX_SYSROOT")
   endif()
   add_custom_command(
      OUTPUT
         "${mach_protocol_dir}/forge_mach_exc_user.h"
         "${mach_protocol_dir}/forge_mach_exc_server.h"
         "${mach_protocol_dir}/forge_mach_exc_user.c"
         "${mach_protocol_dir}/forge_mach_exc_server.c"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${mach_protocol_dir}"
      COMMAND ${FORGE_VM_WASM_INTERPRET_MIG} -arch arm64 -isysroot "${CMAKE_OSX_SYSROOT}"
         -cc "${CMAKE_C_COMPILER}"
         -Dmach_exc=forge_vm_mach_exc
         -Dmach_exception_raise=forge_vm_mach_exception_raise
         -Dmach_exception_raise_state=forge_vm_mach_exception_raise_state
         -Dmach_exception_raise_state_identity=forge_vm_mach_exception_raise_state_identity
         -DMACH_EXC_SERVER_AUDITTOKEN=1
         -header "${mach_protocol_dir}/forge_mach_exc_user.h"
         -sheader "${mach_protocol_dir}/forge_mach_exc_server.h"
         -user "${mach_protocol_dir}/forge_mach_exc_user.c"
         -server "${mach_protocol_dir}/forge_mach_exc_server.c"
         "${mach_sdk_dir}/mach_exc.defs"
      DEPENDS "${mach_sdk_dir}/mach_exc.defs"
      VERBATIM
   )
   target_sources(forge_vm_wasm_interpret PRIVATE
      mach_recovery.cpp
      "${mach_protocol_dir}/forge_mach_exc_server.c"
   )
   set_source_files_properties(mach_recovery.cpp
      PROPERTIES INCLUDE_DIRECTORIES "${mach_protocol_dir}")
   set_source_files_properties("${mach_protocol_dir}/forge_mach_exc_server.c"
      PROPERTIES COMPILE_DEFINITIONS "__MigTypeCheck=1")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
   list(APPEND FORGE_vm_wasm_interpret_DISABLED_MODULES x86_64)
endif()

forge_target_modules_at(forge_vm_wasm_interpret vm/wasm/interpret)

target_include_directories(
   forge_vm_wasm_interpret
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
)

target_link_libraries(
   forge_vm_wasm_interpret
   PUBLIC
      forge_exceptions
      Threads::Threads
      softfloat
      Boost::headers
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
   target_compile_definitions(forge_vm_wasm_interpret PUBLIC FORGE_VM_WASM_INTERPRET_HAS_JIT=1)
else()
   target_compile_definitions(forge_vm_wasm_interpret PUBLIC FORGE_VM_WASM_INTERPRET_HAS_JIT=0)
endif()
