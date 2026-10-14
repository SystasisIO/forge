add_library(
   forge_vm_wasm_interpret STATIC
   allocator.cpp
   debug_info.cpp
   signals.cpp
   types.cpp
   watchdog.cpp
   watchdog_stop_callback.cpp
)

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
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
   target_compile_definitions(forge_vm_wasm_interpret PUBLIC FORGE_VM_WASM_INTERPRET_HAS_JIT=1)
else()
   target_compile_definitions(forge_vm_wasm_interpret PUBLIC FORGE_VM_WASM_INTERPRET_HAS_JIT=0)
endif()
