get_filename_component(_forge_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
include("${_forge_root}/cmake/ForgeWamrSource.cmake")
set(_forge_wamr_compiler_role ON)
include("${_forge_root}/cmake/ForgeWamrProfile.cmake")
forge_prepare_wamr(_source _identity)
set(WAMR_BUILD_WITH_CUSTOM_LLVM 1)
find_package(LLVM 22.1.8 EXACT REQUIRED CONFIG)
add_subdirectory("${_source}/wamr-compiler" vendor EXCLUDE_FROM_ALL)
# Upstream's compiler unconditionally adds its own feature flags.
forge_wamr_apply_profile("${_source}/wamr-compiler" compiler)
get_directory_property(_compiler_definitions DIRECTORY "${_source}/wamr-compiler" COMPILE_DEFINITIONS)
get_target_property(_compiler_runtime_sources vmlib SOURCES)
list(FILTER _compiler_runtime_sources EXCLUDE REGEX "/common/gc/")
set_property(TARGET vmlib PROPERTY SOURCES ${_compiler_runtime_sources})
set_target_properties(vmlib PROPERTIES EXPORT_NAME forge_wamr_compiler_runtime OUTPUT_NAME forge_wamr_compiler_runtime)
set_target_properties(aotclib PROPERTIES EXPORT_NAME forge_wamr_codegen OUTPUT_NAME forge_wamr_codegen)
if(NOT LLVM_ENABLE_RTTI)
   message(FATAL_ERROR "Forge WAMR compiler qualification requires an RTTI-enabled LLVM build")
endif()
# Upstream adds -fno-rtti. On Darwin that emits private std::exception typeinfo
# into aotclib and prevents surrounding C++ handlers from matching derived errors.
target_compile_options(aotclib PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-frtti>")
foreach(_target IN ITEMS vmlib aotclib)
   target_compile_options(${_target} PRIVATE -fno-fast-math -ffp-contract=off -frounding-math)
endforeach()

add_library(forge_vm_wasm_compiler STATIC engine_compiler.cpp ../interpret/engine_limits.cpp)
# Private metadata observations must use the compiler's exact definitions.
get_target_property(_compiler_includes vmlib INCLUDE_DIRECTORIES)
target_compile_definitions(forge_vm_wasm_compiler PRIVATE ${_compiler_definitions})
foreach(_include IN LISTS _compiler_includes)
   target_include_directories(forge_vm_wasm_compiler PRIVATE "$<BUILD_INTERFACE:${_include}>")
endforeach()
set_property(TARGET forge_vm_wasm_compiler PROPERTY FORGE_WAMR_SOURCE_IDENTITY "${_identity}")
target_compile_features(forge_vm_wasm_compiler PRIVATE cxx_std_23)
target_include_directories(forge_vm_wasm_compiler PRIVATE "$<BUILD_INTERFACE:${_source}/core/iwasm/include>")
target_link_libraries(forge_vm_wasm_compiler PRIVATE aotclib vmlib LLVM Threads::Threads ${CMAKE_DL_LIBS} m)
if(COMMAND forge_target_modules_at)
   target_sources(forge_vm_wasm_compiler PRIVATE compiler.cpp)
   forge_target_modules_at(forge_vm_wasm_compiler vm/wasm/compiler)
   target_include_directories(forge_vm_wasm_compiler PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
   target_link_libraries(forge_vm_wasm_compiler PUBLIC forge_vm_wasm_optimizing)
else()
   # The standalone helper qualifies the very same source-owned capacity type.
   target_sources(forge_vm_wasm_compiler PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../interpret/include"
      FILES "${CMAKE_CURRENT_SOURCE_DIR}/../interpret/include/forge/vm/wasm/interpret/capacity_profile.cppm")
endif()
add_custom_target(forge_wamrc_identity
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:wamrc>"
      "-DSOURCE_IDENTITY=${_identity}" -DWRITE_IDENTITY=ON
      -P "${_forge_root}/cmake/ForgeWamrCompilerIdentity.cmake"
   DEPENDS wamrc VERBATIM)
