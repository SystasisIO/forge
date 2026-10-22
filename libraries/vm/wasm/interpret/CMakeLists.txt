get_filename_component(_forge_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
include("${_forge_root}/cmake/ForgeWamrSource.cmake")
set(_forge_wamr_compiler_role OFF)
include("${_forge_root}/cmake/ForgeWamrProfile.cmake")
enable_language(ASM)
string(TOLOWER "${CMAKE_SYSTEM_NAME}" WAMR_BUILD_PLATFORM)
if(APPLE)
   add_compile_definitions(BH_PLATFORM_DARWIN)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
   set(WAMR_BUILD_TARGET AARCH64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
   set(WAMR_BUILD_TARGET X86_64)
else()
   message(FATAL_ERROR "Unsupported WAMR host architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   set(WAMR_BUILD_WITH_CUSTOM_LLVM 1)
   find_package(LLVM 22.1.8 EXACT REQUIRED CONFIG)
   if(NOT LLVM_ENABLE_RTTI)
      message(FATAL_ERROR "Forge WAMR JIT requires an RTTI-enabled LLVM build")
   endif()
endif()
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   forge_prepare_wamr(WAMR_ROOT_DIR _forge_wamr_identity jit)
else()
   forge_prepare_wamr(WAMR_ROOT_DIR _forge_wamr_identity)
endif()
get_property(_forge_inherited_include_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
set(_forge_inherited_cxx_flags "${CMAKE_CXX_FLAGS}")
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
set(CMAKE_CXX_FLAGS "${_forge_inherited_cxx_flags}")
forge_wamr_apply_profile("${CMAKE_CURRENT_SOURCE_DIR}" runtime)
get_directory_property(_forge_wamr_compile_definitions COMPILE_DEFINITIONS)
get_property(_forge_wamr_include_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${_forge_inherited_include_dirs}")
if(_forge_inherited_include_dirs)
   list(REMOVE_ITEM _forge_wamr_include_dirs ${_forge_inherited_include_dirs})
endif()

add_library(
   forge_vm_wasm_interpret STATIC
   ${WAMR_RUNTIME_LIB_SOURCE}
   engine_runtime.cpp
   engine_load_budget.cpp
   engine_memory.cpp
   engine_thread_scope.cpp
   engine_module.cpp
   engine_module_jit.cpp
   engine_bindings.cpp
   engine_limits.cpp
   engine_instance.cpp
   engine_exception.cpp
   prepared_module.cpp
   prepared_module_impl.cpp
   prepared_module_context.cpp
   prepared_module_jit.cpp
   allocator.cpp
   backend.cpp
   debug_info.cpp
   floating_point_environment.cpp
   signals.cpp
   watchdog.cpp
   watchdog_stop_callback.cpp
)
set_property(TARGET forge_vm_wasm_interpret PROPERTY FORGE_WAMR_SOURCE_IDENTITY "${_forge_wamr_identity}")
target_compile_definitions(forge_vm_wasm_interpret PRIVATE ${_forge_wamr_compile_definitions})
# Private module build requirements are exported by CMake unless build-scoped.
foreach(_forge_wamr_include_dir IN LISTS _forge_wamr_include_dirs)
   target_include_directories(forge_vm_wasm_interpret PRIVATE "$<BUILD_INTERFACE:${_forge_wamr_include_dir}>")
endforeach()
target_compile_options(forge_vm_wasm_interpret PRIVATE
   "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-fast-math;-ffp-contract=off;-frounding-math>")
target_link_libraries(forge_vm_wasm_interpret PRIVATE ${CMAKE_DL_LIBS} m)
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   target_link_libraries(forge_vm_wasm_interpret PRIVATE LLVM)
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
      Boost::headers
      Threads::Threads
      softfloat
)

if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   target_compile_definitions(forge_vm_wasm_interpret PUBLIC FORGE_VM_WASM_INTERPRET_HAS_JIT=1)
else()
   target_compile_definitions(forge_vm_wasm_interpret PUBLIC FORGE_VM_WASM_INTERPRET_HAS_JIT=0)
endif()
