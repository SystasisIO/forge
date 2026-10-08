include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/ForgeContractLibraries.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ForgeContractBuild.cmake")

function(forge_add_contract_project target)
   if(FORGE_CONTRACT_GUEST)
      message(
         FATAL_ERROR
         "forge_add_contract_project(${target}) is a host-side launcher"
      )
   endif()
   cmake_parse_arguments(
      ARG
      "ABI_METADATA"
      "SOURCE_DIR;BINARY_DIR;CONTRACT;SOURCE_ROOT"
      ""
      ${ARGN}
   )
   if(ARG_UNPARSED_ARGUMENTS)
      message(
         FATAL_ERROR
         "forge_add_contract_project(${target}) received unknown arguments: "
         "${ARG_UNPARSED_ARGUMENTS}"
      )
   endif()
   foreach(_required SOURCE_DIR BINARY_DIR CONTRACT)
      if(NOT ARG_${_required})
         message(
            FATAL_ERROR
            "forge_add_contract_project(${target}) requires ${_required}"
         )
      endif()
   endforeach()
   if(TARGET "${target}")
      message(FATAL_ERROR "forge_add_contract_project target already exists: ${target}")
   endif()

   set(_contract_package_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
   set(_contract_prefix "${ForgeContract_PREFIX}")
   set(_contract_toolchain "${ForgeContract_TOOLCHAIN}")
   if(ForgeContract_DIR)
      get_filename_component(
         _contract_package_dir "${ForgeContract_DIR}" REALPATH
         BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
      )
      if(NOT _contract_prefix)
         if(NOT EXISTS "${_contract_package_dir}/ForgeContractPaths.cmake")
            message(
               FATAL_ERROR
               "ForgeContract package does not expose relocatable SDK paths: "
               "${_contract_package_dir}/ForgeContractPaths.cmake"
            )
         endif()
         include("${_contract_package_dir}/ForgeContractPaths.cmake")
         set(_contract_prefix "${ForgeContract_PREFIX}")
      endif()
      if(NOT _contract_toolchain)
         set(
            _contract_toolchain
            "${_contract_package_dir}/ForgeContractToolchain.cmake"
         )
      endif()
   endif()
   if(NOT EXISTS "${_contract_package_dir}/ForgeContractConfig.cmake")
      message(
         FATAL_ERROR
         "forge_add_contract_project requires an installed ForgeContract_DIR"
      )
   endif()
   if(NOT EXISTS "${_contract_toolchain}")
      message(
         FATAL_ERROR
         "Forge Contract toolchain does not exist: ${_contract_toolchain}"
      )
   endif()

   get_filename_component(
      _source_dir "${ARG_SOURCE_DIR}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
   )
   get_filename_component(
      _binary_dir "${ARG_BINARY_DIR}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
   )
   if(NOT EXISTS "${_source_dir}/CMakeLists.txt")
      message(
         FATAL_ERROR
         "Forge Contract guest project has no CMakeLists.txt: ${_source_dir}"
      )
   endif()
   if(_source_dir STREQUAL _binary_dir)
      message(FATAL_ERROR "Forge Contract guest source and binary directories must differ")
   endif()
   if(ARG_SOURCE_ROOT)
      get_filename_component(
         _source_root "${ARG_SOURCE_ROOT}" REALPATH
         BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
      )
   else()
      set(_source_root "${_source_dir}")
   endif()
   if(NOT IS_DIRECTORY "${_source_root}")
      message(
         FATAL_ERROR
         "Forge Contract product source root is not a directory: ${_source_root}"
      )
   endif()

   set(_artifact_root "${_binary_dir}/artifacts")
   if(CMAKE_CONFIGURATION_TYPES)
      set(_artifact_dir "${_artifact_root}/$<CONFIG>")
   else()
      set(_artifact_dir "${_artifact_root}")
   endif()
   set(_abi_metadata)
   if(ARG_ABI_METADATA)
      set(_abi_metadata "${_artifact_dir}/${ARG_CONTRACT}.abi.metadata.json")
   endif()
   set(_prefix_path ${CMAKE_PREFIX_PATH})
   list(APPEND _prefix_path "${_contract_prefix}")
   list(REMOVE_DUPLICATES _prefix_path)
   string(REPLACE ";" "|" _prefix_path "${_prefix_path}")

   ExternalProject_Add(
      "${target}"
      SOURCE_DIR "${_source_dir}"
      BINARY_DIR "${_binary_dir}"
      DOWNLOAD_COMMAND ""
      UPDATE_COMMAND ""
      PATCH_COMMAND ""
      INSTALL_COMMAND ""
      BUILD_ALWAYS TRUE
      CMAKE_GENERATOR "${CMAKE_GENERATOR}"
      LIST_SEPARATOR "|"
      CMAKE_ARGS
         "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
         "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${_contract_toolchain}"
         "-DForgeContract_DIR:PATH=${_contract_package_dir}"
         "-DCMAKE_PREFIX_PATH:PATH=${_prefix_path}"
         "-DFORGE_CONTRACT_ARTIFACT_DIR:PATH=${_artifact_root}"
         "-DFORGE_CONTRACT_SOURCE_ROOT:PATH=${_source_root}"
      BUILD_COMMAND
         "${CMAKE_COMMAND}" --build <BINARY_DIR>
         --config "$<CONFIG>"
         --target "${ARG_CONTRACT}_artifacts" --parallel 4
      BUILD_BYPRODUCTS
         "${_artifact_dir}/${ARG_CONTRACT}.wasm"
         "${_artifact_dir}/${ARG_CONTRACT}.abi"
         "${_artifact_dir}/${ARG_CONTRACT}.contract.json"
         ${_abi_metadata}
   )
   set_target_properties(
      "${target}"
      PROPERTIES
         FORGE_CONTRACT_WASM_FILE "${_artifact_dir}/${ARG_CONTRACT}.wasm"
         FORGE_CONTRACT_ABI_FILE "${_artifact_dir}/${ARG_CONTRACT}.abi"
         FORGE_CONTRACT_ABI_METADATA_FILE "${_abi_metadata}"
         FORGE_CONTRACT_MANIFEST_FILE "${_artifact_dir}/${ARG_CONTRACT}.contract.json"
         FORGE_CONTRACT_SDK_PREFIX "${_contract_prefix}"
   )
   foreach(_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${_configuration}" _configuration_upper)
      set_target_properties(
         "${target}"
         PROPERTIES
            "FORGE_CONTRACT_WASM_FILE_${_configuration_upper}"
               "${_artifact_root}/${_configuration}/${ARG_CONTRACT}.wasm"
            "FORGE_CONTRACT_ABI_FILE_${_configuration_upper}"
               "${_artifact_root}/${_configuration}/${ARG_CONTRACT}.abi"
            "FORGE_CONTRACT_MANIFEST_FILE_${_configuration_upper}"
               "${_artifact_root}/${_configuration}/${ARG_CONTRACT}.contract.json"
      )
   endforeach()
   if(_abi_metadata)
      foreach(_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
         string(TOUPPER "${_configuration}" _configuration_upper)
         set_target_properties("${target}" PROPERTIES
            "FORGE_CONTRACT_ABI_METADATA_FILE_${_configuration_upper}"
            "${_artifact_root}/${_configuration}/${ARG_CONTRACT}.abi.metadata.json")
      endforeach()
   endif()
endfunction()
