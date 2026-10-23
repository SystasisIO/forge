add_library(forge_expression_values STATIC)
target_sources(forge_expression_values PUBLIC
   FILE_SET CXX_MODULES
   BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
   FILES
      include/forge/expression/types.cppm
      include/forge/expression/artifact.cppm
)
target_include_directories(forge_expression_values PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_expression_values PUBLIC forge_raw forge_crypto_digest Boost::headers)
forge_target_contract_guest_component(forge_expression_values ID forge.expression.values)

if(NOT FORGE_ENABLE_EXPRESSION)
   return()
endif()

include(ForgeCEL)
find_package(Boost 1.90 REQUIRED CONFIG COMPONENTS process)

add_library(forge_expression STATIC artifact_codec.cpp program.cpp runtime.cpp runtime_impl.cpp worker_protocol.cpp)
set(FORGE_expression_DISABLED_MODULES types artifact)
forge_target_modules(forge_expression expression)
unset(FORGE_expression_DISABLED_MODULES)
target_include_directories(forge_expression PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_expression PUBLIC forge_expression_values forge_exceptions forge_variant forge_asio
   PRIVATE forge_raw Boost::process)

# Only the isolated executable owns CEL, its allocator and its C++ runtime.
# Parent applications never link or interpose the worker's allocation symbols.
add_library(forge_expression_worker_runtime STATIC run_worker.cpp memory.cpp)
target_include_directories(forge_expression_worker_runtime PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR} ${Boost_INCLUDE_DIRS})
target_link_libraries(forge_expression_worker_runtime PRIVATE forge_expression ForgeCEL::backend)
# Imported Forge modules retain their canonical compiler configuration. Only
# this textual, helper-only allocator TU needs the private runtime flags; CEL's
# complete separate build uses them independently.
set_source_files_properties(memory.cpp PROPERTIES
   COMPILE_OPTIONS "-femulated-tls;-fno-c++-static-destructors")
add_subdirectory(${PROJECT_SOURCE_DIR}/tools/expression-worker ${PROJECT_BINARY_DIR}/tools/expression-worker)
target_sources(forge_expression_worker PRIVATE ${FORGE_CEL_RUNTIME_OBJECTS})
add_dependencies(forge_expression_worker forge_expression_backend_build)

set_target_properties(forge_expression_worker PROPERTIES EXPORT_NAME worker)
add_executable(ForgeCEL::worker ALIAS forge_expression_worker)
get_filename_component(_forge_expression_llvm_root "${CMAKE_CXX_COMPILER}" REALPATH)
get_filename_component(_forge_expression_llvm_root "${_forge_expression_llvm_root}" DIRECTORY)
get_filename_component(_forge_expression_llvm_root "${_forge_expression_llvm_root}" DIRECTORY)
foreach(_forge_expression_runtime libc++ libc++abi)
   set(_forge_expression_runtime_archive "${_forge_expression_llvm_root}/lib/c++/${_forge_expression_runtime}.a")
   if(NOT EXISTS "${_forge_expression_runtime_archive}")
      message(FATAL_ERROR "Expression worker requires the selected LLVM static ${_forge_expression_runtime} runtime")
   endif()
   # Archive position matters: main.o's audited diagnostics initializer must
   # precede the private runtime's initializers in the final Mach-O image.
   target_link_libraries(forge_expression_worker PRIVATE "-Wl,-load_hidden,${_forge_expression_runtime_archive}")
endforeach()
target_link_libraries(forge_expression_worker PRIVATE
   "-Wl,-load_hidden,${_forge_expression_llvm_root}/lib/unwind/libunwind.a")
target_link_options(forge_expression_worker PRIVATE -nostdlib++
   "LINKER:-dead_strip"
   "LINKER:-dead_strip_dylibs"
   "LINKER:-unexported_symbol,___emutls_get_address"
   "LINKER:-unexported_symbol,___cxa_get_globals"
   "LINKER:-unexported_symbol,___cxa_get_globals_fast"
   "LINKER:-map,${FORGE_CEL_WORKER_LINK_MAP}")
target_compile_options(forge_expression_worker PRIVATE -femulated-tls -fno-c++-static-destructors)
set_property(TARGET forge_expression_worker APPEND PROPERTY LINK_DEPENDS
   "${PROJECT_SOURCE_DIR}/cmake/cel/verify_worker.py"
   "${PROJECT_SOURCE_DIR}/cmake/cel/sources.json")

install(TARGETS forge_expression_worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dev EXCLUDE_FROM_ALL)

if(FORGE_CONTRACT_SDK_HOST_PROFILE AND BUILD_TESTING)
   include("${PROJECT_SOURCE_DIR}/tests/expression.cmake")
endif()
