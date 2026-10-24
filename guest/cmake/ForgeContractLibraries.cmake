function(_forge_contract_id_key id output)
   string(SHA256 _key "${id}")
   set(${output} "${_key}" PARENT_SCOPE)
endfunction()

function(_forge_contract_resolve_target input output)
   if(NOT TARGET "${input}")
      message(FATAL_ERROR "unknown Contract SDK dependency target: ${input}")
   endif()
   get_target_property(_aliased "${input}" ALIASED_TARGET)
   if(_aliased)
      set(${output} "${_aliased}" PARENT_SCOPE)
   else()
      set(${output} "${input}" PARENT_SCOPE)
   endif()
endfunction()

function(_forge_contract_normalize_root input description output)
   get_filename_component(
      _absolute "${input}" REALPATH
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
   )
   if(NOT IS_DIRECTORY "${_absolute}")
      message(FATAL_ERROR "${description} is not a directory: ${_absolute}")
   endif()
   set(${output} "${_absolute}" PARENT_SCOPE)
endfunction()

function(_forge_contract_require_descendant root path description)
   get_filename_component(_root "${root}" REALPATH)
   get_filename_component(_absolute "${path}" REALPATH)
   file(RELATIVE_PATH _relative "${_root}" "${_absolute}")
   if(IS_ABSOLUTE "${_relative}" OR _relative MATCHES "^\\.\\.(/|$)")
      message(
         FATAL_ERROR
         "${description} is outside its declared root: ${_absolute}\n"
         "declared root: ${_root}"
      )
   endif()
endfunction()

function(_forge_contract_normalize_file root input description output)
   get_filename_component(_absolute "${input}" ABSOLUTE BASE_DIR "${root}")
   if(IS_DIRECTORY "${_absolute}")
      message(FATAL_ERROR "${description} is a directory: ${_absolute}")
   endif()
   get_property(_generated SOURCE "${_absolute}" PROPERTY GENERATED)
   if(_generated)
      _forge_contract_require_descendant(
         "${CMAKE_BINARY_DIR}" "${_absolute}" "${description} generated output"
      )
   else()
      if(NOT EXISTS "${_absolute}")
         message(
            FATAL_ERROR
            "${description} does not exist and is not a declared generated "
            "output: ${_absolute}"
         )
      endif()
      _forge_contract_require_descendant(
         "${root}" "${_absolute}" "${description}"
      )
   endif()
   set(${output} "${_absolute}" PARENT_SCOPE)
endfunction()

function(_forge_contract_require_source_or_binary root path description)
   get_filename_component(_root "${root}" REALPATH)
   get_filename_component(_binary_root "${CMAKE_BINARY_DIR}" REALPATH)
   get_filename_component(_absolute "${path}" REALPATH)
   file(RELATIVE_PATH _source_relative "${_root}" "${_absolute}")
   if(
      NOT IS_ABSOLUTE "${_source_relative}"
      AND NOT _source_relative MATCHES "^\\.\\.(/|$)"
   )
      return()
   endif()
   file(RELATIVE_PATH _binary_relative "${_binary_root}" "${_absolute}")
   if(
      NOT IS_ABSOLUTE "${_binary_relative}"
      AND NOT _binary_relative MATCHES "^\\.\\.(/|$)"
   )
      return()
   endif()
   message(
      FATAL_ERROR
      "${description} is outside its source and binary roots: ${_absolute}\n"
      "source root: ${_root}\n"
      "binary root: ${_binary_root}"
   )
endfunction()

function(_forge_contract_product_source_root output)
   if(
      DEFINED FORGE_CONTRACT_SOURCE_ROOT
      AND NOT "${FORGE_CONTRACT_SOURCE_ROOT}" STREQUAL ""
   )
      set(_candidate "${FORGE_CONTRACT_SOURCE_ROOT}")
   else()
      set(_candidate "${CMAKE_SOURCE_DIR}")
   endif()
   get_filename_component(
      _candidate "${_candidate}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}"
   )
   if(NOT IS_DIRECTORY "${_candidate}")
      message(
         FATAL_ERROR
         "Forge Contract product source root is not a directory: ${_candidate}"
      )
   endif()
   get_property(
      _root GLOBAL PROPERTY FORGE_CONTRACT_CANONICAL_SOURCE_ROOT
   )
   if(_root AND NOT "${_candidate}" STREQUAL "${_root}")
      message(
         FATAL_ERROR
         "FORGE_CONTRACT_SOURCE_ROOT changed after the guest SDK fixed its "
         "canonical source root: ${_root} -> ${_candidate}"
      )
   endif()
   if(NOT _root)
      set(_root "${_candidate}")
      set_property(
         GLOBAL PROPERTY FORGE_CONTRACT_CANONICAL_SOURCE_ROOT "${_root}"
      )
   endif()
   set(${output} "${_root}" PARENT_SCOPE)
endfunction()

function(_forge_contract_require_product_source path description)
   if(NOT FORGE_CONTRACT_GUEST)
      return()
   endif()
   _forge_contract_product_source_root(_product_root)
   get_filename_component(_absolute "${path}" REALPATH)
   file(RELATIVE_PATH _relative "${_product_root}" "${_absolute}")
   if(NOT IS_ABSOLUTE "${_relative}" AND NOT _relative MATCHES "^\\.\\.(/|$)")
      return()
   endif()
   get_filename_component(_binary_root "${CMAKE_BINARY_DIR}" REALPATH)
   file(RELATIVE_PATH _binary_relative "${_binary_root}" "${_absolute}")
   if(NOT IS_ABSOLUTE "${_binary_relative}" AND NOT _binary_relative MATCHES "^\\.\\.(/|$)")
      return()
   endif()
   message(
      FATAL_ERROR
      "${description} is outside the Forge Contract product roots: ${_absolute}\n"
      "product source root: ${_product_root}\n"
      "product binary root: ${_binary_root}"
   )
endfunction()

function(_forge_contract_semantic_properties output)
   set(
      ${output}
      SOURCES
      INTERFACE_SOURCES
      COMPILE_OPTIONS
      INTERFACE_COMPILE_OPTIONS
      COMPILE_FLAGS
      COMPILE_DEFINITIONS
      INTERFACE_COMPILE_DEFINITIONS
      INCLUDE_DIRECTORIES
      INTERFACE_INCLUDE_DIRECTORIES
      COMPILE_FEATURES
      INTERFACE_COMPILE_FEATURES
      LINK_OPTIONS
      INTERFACE_LINK_OPTIONS
      LINK_FLAGS
      LINKER_TYPE
      LINK_DIRECTORIES
      INTERFACE_LINK_DIRECTORIES
      LINK_DEPENDS
      LINK_DEPENDS_NO_SHARED
      STATIC_LIBRARY_OPTIONS
      # These values are compared verbatim only. Forge never interprets them as
      # a second dependency graph.
      LINK_LIBRARIES
      INTERFACE_LINK_LIBRARIES
      CXX_STANDARD
      CXX_STANDARD_REQUIRED
      CXX_EXTENSIONS
      CXX_MODULE_STD
      CXX_SCAN_FOR_MODULES
      CXX_COMPILER_LAUNCHER
      CXX_LINKER_LAUNCHER
      PRECOMPILE_HEADERS
      INTERFACE_PRECOMPILE_HEADERS
      PRECOMPILE_HEADERS_REUSE_FROM
      RULE_LAUNCH_COMPILE
      RULE_LAUNCH_LINK
      CXX_CLANG_TIDY
      CXX_CPPCHECK
      CXX_INCLUDE_WHAT_YOU_USE
      UNITY_BUILD
      UNITY_BUILD_MODE
      UNITY_BUILD_BATCH_SIZE
      UNITY_BUILD_CODE_BEFORE_INCLUDE
      UNITY_BUILD_CODE_AFTER_INCLUDE
      UNITY_BUILD_UNIQUE_ID
      POSITION_INDEPENDENT_CODE
      INTERPROCEDURAL_OPTIMIZATION
      CXX_VISIBILITY_PRESET
      VISIBILITY_INLINES_HIDDEN
      CXX_MODULE_SETS
      INTERFACE_CXX_MODULE_SETS
      PARENT_SCOPE
   )
endfunction()

function(_forge_contract_target_semantic_properties target output)
   _forge_contract_semantic_properties(_properties)
   get_target_property(_module_sets "${target}" CXX_MODULE_SETS)
   if(_module_sets AND NOT _module_sets MATCHES "-NOTFOUND$")
      foreach(_module_set IN LISTS _module_sets)
         list(
            APPEND _properties
            "CXX_MODULE_SET_${_module_set}"
            "CXX_MODULE_DIRS_${_module_set}"
         )
      endforeach()
   endif()
   set(_configurations DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
   if(CMAKE_CONFIGURATION_TYPES)
      list(APPEND _configurations ${CMAKE_CONFIGURATION_TYPES})
   endif()
   list(REMOVE_DUPLICATES _configurations)
   foreach(_configuration IN LISTS _configurations)
      string(TOUPPER "${_configuration}" _configuration)
      list(
         APPEND _properties
         "COMPILE_DEFINITIONS_${_configuration}"
         "LINK_FLAGS_${_configuration}"
         "INTERPROCEDURAL_OPTIMIZATION_${_configuration}"
      )
   endforeach()
   set(${output} "${_properties}" PARENT_SCOPE)
endfunction()

function(_forge_contract_source_semantic_properties output)
   set(
      _properties
      COMPILE_OPTIONS
      COMPILE_DEFINITIONS
      INCLUDE_DIRECTORIES
      COMPILE_FLAGS
      LANGUAGE
      CXX_SCAN_FOR_MODULES
      GENERATED
      HEADER_FILE_ONLY
      EXTERNAL_OBJECT
      OBJECT_DEPENDS
      OBJECT_OUTPUTS
   )
   set(_configurations DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
   if(CMAKE_CONFIGURATION_TYPES)
      list(APPEND _configurations ${CMAKE_CONFIGURATION_TYPES})
   endif()
   list(REMOVE_DUPLICATES _configurations)
   foreach(_configuration IN LISTS _configurations)
      string(TOUPPER "${_configuration}" _configuration)
      list(APPEND _properties "COMPILE_DEFINITIONS_${_configuration}")
   endforeach()
   set(${output} "${_properties}" PARENT_SCOPE)
endfunction()

function(_forge_contract_source_property target source property output)
   get_property(
      _is_set SOURCE "${source}" TARGET_DIRECTORY "${target}"
      PROPERTY "${property}" SET
   )
   if(_is_set)
      get_property(
         _value SOURCE "${source}" TARGET_DIRECTORY "${target}"
         PROPERTY "${property}"
      )
   else()
      set(_value "<FORGE_UNSET>")
   endif()
   set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(_forge_contract_target_source_files target output)
   get_target_property(_sources "${target}" SOURCES)
   if(NOT _sources OR _sources MATCHES "-NOTFOUND$")
      set(_sources)
   endif()
   get_target_property(_module_sets "${target}" CXX_MODULE_SETS)
   if(NOT _module_sets OR _module_sets MATCHES "-NOTFOUND$")
      set(_module_sets)
   endif()
   foreach(_module_set IN LISTS _module_sets)
      get_target_property(
         _module_sources "${target}" "CXX_MODULE_SET_${_module_set}"
      )
      if(_module_sources AND NOT _module_sources MATCHES "-NOTFOUND$")
         list(APPEND _sources ${_module_sources})
      endif()
   endforeach()
   list(REMOVE_DUPLICATES _sources)
   set(${output} "${_sources}" PARENT_SCOPE)
endfunction()

function(_forge_contract_source_lookup_paths target source output)
   get_target_property(_source_directory "${target}" SOURCE_DIR)
   get_filename_component(
      _absolute "${source}" ABSOLUTE BASE_DIR "${_source_directory}"
   )
   set(_paths "${source}" "${_absolute}")
   file(RELATIVE_PATH _relative "${_source_directory}" "${_absolute}")
   if(NOT IS_ABSOLUTE "${_relative}" AND NOT _relative MATCHES "^\\.\\.(/|$)")
      list(APPEND _paths "${_relative}")
   endif()
   list(REMOVE_DUPLICATES _paths)
   set(${output} "${_paths}" PARENT_SCOPE)
endfunction()

function(_forge_contract_validate_guest_targets)
   _forge_contract_validate_guest_environment()
   get_property(_targets GLOBAL PROPERTY FORGE_CONTRACT_FROZEN_TARGETS)
   foreach(target IN LISTS _targets)
      if(NOT TARGET "${target}")
         message(FATAL_ERROR "frozen Forge Contract guest target disappeared: ${target}")
      endif()
      get_target_property(
         _declaration "${target}" FORGE_CONTRACT_FROZEN_DECLARATION
      )
      _forge_contract_target_semantic_properties("${target}" _properties)
      foreach(_property IN LISTS _properties)
         get_property(_is_set TARGET "${target}" PROPERTY "${_property}" SET)
         if(_is_set)
            get_target_property(_current "${target}" "${_property}")
         else()
            set(_current "<FORGE_UNSET>")
         endif()
         get_target_property(
            _expected "${target}" "FORGE_CONTRACT_FROZEN_${_property}"
         )
         if(NOT "${_current}" STREQUAL "${_expected}")
            message(
               FATAL_ERROR
               "Forge Contract guest target '${target}' was modified after "
               "${_declaration}; post-declaration target mutation is unsupported "
               "because CMake compilation and Abigen must use one semantic profile "
               "(changed property: ${_property})"
            )
         endif()
      endforeach()
      get_target_property(
         _sources "${target}" FORGE_CONTRACT_FROZEN_SOURCE_FILES
      )
      if(NOT _sources OR _sources MATCHES "-NOTFOUND$")
         set(_sources)
      endif()
      _forge_contract_source_semantic_properties(_source_properties)
      foreach(_source IN LISTS _sources)
         _forge_contract_source_lookup_paths(
            "${target}" "${_source}" _source_lookups
         )
         foreach(_source_lookup IN LISTS _source_lookups)
            foreach(_property IN LISTS _source_properties)
               _forge_contract_source_property(
                  "${target}" "${_source_lookup}" "${_property}" _current
               )
               string(
                  SHA256 _source_key "${_source_lookup}\n${_property}"
               )
               get_target_property(
                  _expected "${target}"
                  "FORGE_CONTRACT_FROZEN_SOURCE_${_source_key}"
               )
               if(NOT "${_current}" STREQUAL "${_expected}")
                  message(
                     FATAL_ERROR
                     "Forge Contract guest target '${target}' source '${_source}' "
                     "was modified after ${_declaration}; post-declaration source "
                     "mutation is unsupported because CMake compilation and "
                     "Abigen must use one semantic profile (changed source "
                     "property: ${_property})"
                  )
               endif()
            endforeach()
         endforeach()
      endforeach()
   endforeach()
endfunction()

function(_forge_contract_register_internal_deferred id command)
   string(SHA256 _id_key "${id}")
   set_property(
      GLOBAL PROPERTY "FORGE_CONTRACT_INTERNAL_DEFERRED_${_id_key}"
      "${command}"
   )
   set_property(
      GLOBAL APPEND PROPERTY FORGE_CONTRACT_INTERNAL_DEFERRED_IDS "${id}"
   )
endfunction()

function(_forge_contract_defer_guest_environment_validation)
   cmake_language(
      DEFER ID_VAR _validation_id
      CALL _forge_contract_validate_guest_environment_final
   )
   _forge_contract_register_internal_deferred(
      "${_validation_id}" "_forge_contract_validate_guest_environment_final"
   )
endfunction()

function(_forge_contract_defer_guest_target_validation)
   cmake_language(
      DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
      ID_VAR _validation_id
      CALL _forge_contract_validate_guest_targets_final
   )
   _forge_contract_register_internal_deferred(
      "${_validation_id}" "_forge_contract_validate_guest_targets_final"
   )
endfunction()

function(_forge_contract_downstream_deferred_calls output)
   cmake_language(DEFER GET_CALL_IDS _pending_calls)
   get_property(
      _internal_ids GLOBAL PROPERTY FORGE_CONTRACT_INTERNAL_DEFERRED_IDS
   )
   set(_downstream_calls ${_pending_calls})
   foreach(_internal_id IN LISTS _internal_ids)
      set(_occurrences 0)
      foreach(_pending_id IN LISTS _pending_calls)
         if("${_pending_id}" STREQUAL "${_internal_id}")
            math(EXPR _occurrences "${_occurrences} + 1")
         endif()
      endforeach()
      if(NOT _occurrences EQUAL 1)
         continue()
      endif()
      string(SHA256 _id_key "${_internal_id}")
      get_property(
         _expected GLOBAL
         PROPERTY "FORGE_CONTRACT_INTERNAL_DEFERRED_${_id_key}"
      )
      cmake_language(DEFER GET_CALL "${_internal_id}" _actual)
      if("${_actual}" STREQUAL "${_expected}")
         list(REMOVE_ITEM _downstream_calls "${_internal_id}")
      endif()
   endforeach()
   set(${output} "${_downstream_calls}" PARENT_SCOPE)
endfunction()

function(_forge_contract_validate_guest_environment_final)
   _forge_contract_downstream_deferred_calls(_pending_calls)
   if(_pending_calls)
      cmake_language(
         DEFER ID_VAR _validation_id
         CALL _forge_contract_validate_guest_environment_final
      )
      _forge_contract_register_internal_deferred(
         "${_validation_id}" "_forge_contract_validate_guest_environment_final"
      )
      return()
   endif()
   _forge_contract_validate_guest_environment()
endfunction()

function(_forge_contract_validate_guest_targets_final)
   _forge_contract_downstream_deferred_calls(_pending_calls)
   if(_pending_calls)
      cmake_language(
         DEFER ID_VAR _validation_id
         CALL _forge_contract_validate_guest_targets_final
      )
      _forge_contract_register_internal_deferred(
         "${_validation_id}" "_forge_contract_validate_guest_targets_final"
      )
      return()
   endif()
   _forge_contract_validate_guest_targets()
endfunction()

function(_forge_contract_freeze_guest_target target declaration)
   if(NOT FORGE_CONTRACT_GUEST)
      return()
   endif()
   _forge_contract_target_semantic_properties("${target}" _properties)
   foreach(_property IN LISTS _properties)
      get_property(_is_set TARGET "${target}" PROPERTY "${_property}" SET)
      if(_is_set)
         get_target_property(_current "${target}" "${_property}")
      else()
         set(_current "<FORGE_UNSET>")
      endif()
      set_property(
         TARGET "${target}" PROPERTY
         "FORGE_CONTRACT_FROZEN_${_property}" "${_current}"
      )
   endforeach()
   _forge_contract_target_source_files("${target}" _sources)
   set_property(
      TARGET "${target}" PROPERTY FORGE_CONTRACT_FROZEN_SOURCE_FILES "${_sources}"
   )
   _forge_contract_source_semantic_properties(_source_properties)
   foreach(_source IN LISTS _sources)
      _forge_contract_source_lookup_paths(
         "${target}" "${_source}" _source_lookups
      )
      foreach(_source_lookup IN LISTS _source_lookups)
         foreach(_property IN LISTS _source_properties)
            _forge_contract_source_property(
               "${target}" "${_source_lookup}" "${_property}" _current
            )
            string(SHA256 _source_key "${_source_lookup}\n${_property}")
            set_property(
               TARGET "${target}" PROPERTY
               "FORGE_CONTRACT_FROZEN_SOURCE_${_source_key}" "${_current}"
            )
         endforeach()
      endforeach()
   endforeach()
   set_property(
      TARGET "${target}" PROPERTY
      FORGE_CONTRACT_FROZEN_DECLARATION "${declaration}"
   )
   get_target_property(_source_directory "${target}" SOURCE_DIR)
   string(SHA256 _directory_key "${_source_directory}")
   get_property(
      _directory_scheduled GLOBAL PROPERTY
      "FORGE_CONTRACT_ENVIRONMENT_VALIDATION_${_directory_key}"
   )
   if(NOT _directory_scheduled)
      set_property(
         GLOBAL PROPERTY
         "FORGE_CONTRACT_ENVIRONMENT_VALIDATION_${_directory_key}" TRUE
      )
      cmake_language(
         DEFER DIRECTORY "${_source_directory}"
         ID_VAR _scheduler_id
         CALL _forge_contract_defer_guest_environment_validation
      )
      _forge_contract_register_internal_deferred(
         "${_scheduler_id}"
         "_forge_contract_defer_guest_environment_validation"
      )
   endif()
   set_property(GLOBAL APPEND PROPERTY FORGE_CONTRACT_FROZEN_TARGETS "${target}")
   get_property(
      _scheduled GLOBAL PROPERTY FORGE_CONTRACT_FROZEN_VALIDATION_SCHEDULED
   )
   if(NOT _scheduled)
      set_property(
         GLOBAL PROPERTY FORGE_CONTRACT_FROZEN_VALIDATION_SCHEDULED TRUE
      )
      cmake_language(
         DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
         ID_VAR _scheduler_id
         CALL _forge_contract_defer_guest_target_validation
      )
      _forge_contract_register_internal_deferred(
         "${_scheduler_id}" "_forge_contract_defer_guest_target_validation"
      )
   endif()
endfunction()

function(_forge_contract_validate_guest_environment)
   if(NOT FORGE_CONTRACT_GUEST)
      return()
   endif()
   if(NOT "${CMAKE_CXX_FLAGS}" STREQUAL "")
      message(
         FATAL_ERROR
         "CMAKE_CXX_FLAGS must remain empty in a Forge Contract guest project; "
         "the SDK owns the compile profile shared by CMake and Abigen"
      )
   endif()
   set(_supported_configurations DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
   if(CMAKE_CONFIGURATION_TYPES)
      set(_active_configurations ${CMAKE_CONFIGURATION_TYPES})
   else()
      set(_active_configurations "${CMAKE_BUILD_TYPE}")
   endif()
   foreach(_configuration IN LISTS _active_configurations)
      if("${_configuration}" STREQUAL "")
         continue()
      endif()
      string(TOUPPER "${_configuration}" _configuration_upper)
      if(NOT _configuration_upper IN_LIST _supported_configurations)
         message(
            FATAL_ERROR
            "unsupported Forge Contract guest configuration: ${_configuration}; "
            "supported configurations are Debug, Release, MinSizeRel and "
            "RelWithDebInfo"
         )
      endif()
   endforeach()
   if(
      NOT CMAKE_CXX_STANDARD EQUAL 23
      OR NOT CMAKE_CXX_STANDARD_REQUIRED
      OR CMAKE_CXX_EXTENSIONS
      OR NOT CMAKE_CXX_SCAN_FOR_MODULES
   )
      message(
         FATAL_ERROR
         "Forge Contract guest projects require strict C++23 with required "
         "standard, extensions disabled, and CMake module scanning enabled"
      )
   endif()
   if(CMAKE_INCLUDE_CURRENT_DIR OR CMAKE_INCLUDE_CURRENT_DIR_IN_INTERFACE)
      message(
         FATAL_ERROR
         "CMAKE_INCLUDE_CURRENT_DIR and CMAKE_INCLUDE_CURRENT_DIR_IN_INTERFACE "
         "must remain disabled in a Forge Contract guest project; implicit "
         "include paths are not part of the semantic profile shared by CMake "
         "and Abigen"
      )
   endif()
   set(
      _expected_standard_include_directories
      "${ForgeContract_SYSROOT}/include/c++/v1"
   )
   if(
      NOT "${CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES}"
      STREQUAL "${_expected_standard_include_directories}"
   )
      message(
         FATAL_ERROR
         "CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES must remain "
         "'${_expected_standard_include_directories}' in a Forge Contract guest "
         "project; the SDK owns the standard include profile shared by CMake "
         "and Abigen"
      )
   endif()
   foreach(_entry
      "CMAKE_CXX_FLAGS_DEBUG|-g"
      "CMAKE_CXX_FLAGS_RELEASE|-O3 -DNDEBUG"
      "CMAKE_CXX_FLAGS_MINSIZEREL|-Os -DNDEBUG"
      "CMAKE_CXX_FLAGS_RELWITHDEBINFO|-O2 -g -DNDEBUG"
   )
      string(REPLACE "|" ";" _parts "${_entry}")
      list(GET _parts 0 _variable)
      list(GET _parts 1 _expected)
      if(NOT "${${_variable}}" STREQUAL "${_expected}")
         message(
            FATAL_ERROR
            "${_variable} must remain '${_expected}' in a Forge Contract guest "
            "project; the SDK owns the compile profile shared by CMake and Abigen"
         )
      endif()
   endforeach()
   foreach(_property
      RULE_LAUNCH_COMPILE
      RULE_LAUNCH_LINK
      RULE_LAUNCH_CUSTOM
   )
      get_property(_value GLOBAL PROPERTY "${_property}")
      if(_value)
         message(
            FATAL_ERROR
            "global ${_property} is unsupported in a Forge Contract guest "
            "project; the SDK owns the compile and link launchers"
         )
      endif()
   endforeach()
   foreach(_property
      COMPILE_OPTIONS
      COMPILE_DEFINITIONS
      INCLUDE_DIRECTORIES
      LINK_OPTIONS
      LINK_DIRECTORIES
      RULE_LAUNCH_COMPILE
      RULE_LAUNCH_LINK
      RULE_LAUNCH_CUSTOM
   )
      get_directory_property(_value "${_property}")
      if(_value)
         message(
            FATAL_ERROR
            "directory ${_property} are unsupported in a Forge Contract guest "
            "project; declare the complete target through Forge Contract helpers"
         )
      endif()
   endforeach()
endfunction()

function(_forge_contract_configure_guest_target target)
   if(NOT FORGE_CONTRACT_GUEST)
      return()
   endif()
   _forge_contract_validate_guest_environment()
   _forge_contract_product_source_root(_product_source_root)
   target_include_directories(
      "${target}" PRIVATE "${ForgeContract_DATA_DIR}/include"
   )
   target_compile_definitions("${target}" PRIVATE FORGE_CONTRACT_GUEST=1)
   target_compile_options(
      "${target}"
      PRIVATE
         "-fplugin=${ForgeContract_ATTR_PLUGIN}"
         -fno-exceptions
         -fno-rtti
         -fno-threadsafe-statics
         -ffreestanding
         -fvisibility=hidden
         -fno-ident
         -mcpu=mvp
         "-ffile-prefix-map=${_product_source_root}=./source"
         "-fdebug-prefix-map=${_product_source_root}=./source"
         "-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build"
         "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build"
   )
   set_property(TARGET "${target}" PROPERTY CXX_COMPILER_LAUNCHER "")
endfunction()

function(_forge_contract_register_owner id target)
   _forge_contract_id_key("${id}" _key)
   get_property(_existing GLOBAL PROPERTY "FORGE_CONTRACT_OWNER_${_key}")
   if(_existing AND NOT _existing STREQUAL target)
      message(FATAL_ERROR "duplicate Forge Contract owner ID: ${id}")
   endif()
   set_property(GLOBAL PROPERTY "FORGE_CONTRACT_OWNER_${_key}" "${target}")
endfunction()

function(_forge_contract_object_list target identity output)
   string(
      SHA256 _property_key
      "${CMAKE_CURRENT_BINARY_DIR}\n${target}\n${identity}"
   )
   get_property(
      _existing GLOBAL PROPERTY "FORGE_CONTRACT_OBJECT_LIST_${_property_key}"
   )
   if(_existing)
      set(${output} "${_existing}" PARENT_SCOPE)
      return()
   endif()

   set(
      _path
      "${CMAKE_CURRENT_BINARY_DIR}/contract-compilations/${identity}-$<CONFIG>.objects"
   )
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/contract-compilations")
   file(
      GENERATE
      OUTPUT "${_path}"
      CONTENT "$<JOIN:$<TARGET_OBJECTS:${target}>,\n>\n"
   )
   set_property(
      GLOBAL PROPERTY "FORGE_CONTRACT_OBJECT_LIST_${_property_key}" "${_path}"
   )
   set(${output} "${_path}" PARENT_SCOPE)
endfunction()

function(_forge_contract_module_path target output)
   get_target_property(_binary_dir "${target}" BINARY_DIR)
   if(NOT _binary_dir)
      message(FATAL_ERROR "cannot determine module build directory for ${target}")
   endif()
   set(_configuration_directory)
   if(CMAKE_CONFIGURATION_TYPES)
      set(_configuration_directory "$<CONFIG>/")
   endif()
   set(
      ${output}
      "${_binary_dir}/CMakeFiles/${target}.dir/${_configuration_directory}"
      PARENT_SCOPE
   )
endfunction()

function(_forge_contract_enable_metadata_closure target)
   set_property(
      TARGET "${target}" APPEND PROPERTY TRANSITIVE_LINK_PROPERTIES
      FORGE_CONTRACT_COMPILATIONS
      FORGE_CONTRACT_MODULE_BASES
      FORGE_CONTRACT_MODULE_PATHS
      FORGE_CONTRACT_OWNER_TARGETS
   )
endfunction()

function(_forge_contract_publish_metadata target)
   cmake_parse_arguments(
      ARG
      ""
      "COMPILATION;MODULE_PATH"
      "MODULE_BASES"
      ${ARGN}
   )
   _forge_contract_enable_metadata_closure("${target}")
   if(ARG_COMPILATION)
      set_property(
         TARGET "${target}" APPEND PROPERTY
         INTERFACE_FORGE_CONTRACT_COMPILATIONS "${ARG_COMPILATION}"
      )
   endif()
   if(ARG_MODULE_BASES)
      set_property(
         TARGET "${target}" APPEND PROPERTY
         INTERFACE_FORGE_CONTRACT_MODULE_BASES "${ARG_MODULE_BASES}"
      )
   endif()
   if(ARG_MODULE_PATH)
      set_property(
         TARGET "${target}" APPEND PROPERTY
         INTERFACE_FORGE_CONTRACT_MODULE_PATHS "${ARG_MODULE_PATH}"
      )
   endif()
   set_property(
      TARGET "${target}" APPEND PROPERTY
      INTERFACE_FORGE_CONTRACT_OWNER_TARGETS "${target}"
   )
endfunction()

function(_forge_contract_owner_target id output)
   _forge_contract_id_key("${id}" _key)
   get_property(_target GLOBAL PROPERTY "FORGE_CONTRACT_OWNER_${_key}")
   if(NOT _target)
      message(FATAL_ERROR "Forge Contract owner is not visible: ${id}")
   endif()
   set(${output} "${_target}" PARENT_SCOPE)
endfunction()

if(NOT COMMAND forge_contract_register_guest_component)
   function(forge_contract_register_guest_component)
      cmake_parse_arguments(
         ARG
         "FOUNDATION"
         "ID;TARGET;ARCHIVE"
         "MODULES;MODULE_NAMES;PUBLIC_LIBRARIES"
         ${ARGN}
      )
      if(
         ARG_UNPARSED_ARGUMENTS
         OR NOT ARG_ID
         OR NOT ARG_TARGET
         OR NOT ARG_MODULES
         OR NOT ARG_MODULE_NAMES
      )
         message(FATAL_ERROR "invalid Forge Contract guest component declaration")
      endif()
      list(LENGTH ARG_MODULES _module_count)
      list(LENGTH ARG_MODULE_NAMES _module_name_count)
      if(NOT _module_count EQUAL _module_name_count)
         message(
            FATAL_ERROR
            "Forge Contract guest component ${ARG_ID} has mismatched module metadata"
         )
      endif()

      _forge_contract_id_key("${ARG_ID}" _key)
      get_property(
         _existing GLOBAL PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_ID"
      )
      if(_existing)
         message(FATAL_ERROR "duplicate Forge Contract component ID: ${ARG_ID}")
      endif()
      set_property(
         GLOBAL PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_ID" "${ARG_ID}"
      )
      set_property(
         GLOBAL PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_TARGET_NAME"
         "${ARG_TARGET}"
      )
      set_property(
         GLOBAL PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_MODULE_NAMES"
         "${ARG_MODULE_NAMES}"
      )
      set_property(
         GLOBAL PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_DEPENDENCY_IDS"
         "${ARG_PUBLIC_LIBRARIES}"
      )
      if(ARG_FOUNDATION)
         set_property(
            GLOBAL APPEND PROPERTY FORGE_CONTRACT_FOUNDATION_COMPONENT_IDS
            "${ARG_ID}"
         )
      endif()

      if(NOT FORGE_CONTRACT_GUEST)
         return()
      endif()
      if(TARGET "Forge::${ARG_TARGET}")
         message(
            FATAL_ERROR
            "Forge Contract guest target already exists: Forge::${ARG_TARGET}"
         )
      endif()

      string(SUBSTRING "${_key}" 0 12 _short_key)
      set(_target "_forge_contract_guest_${ARG_TARGET}_${_short_key}")
      set(_module_paths)
      foreach(_module IN LISTS ARG_MODULES)
         set(_path "${ForgeContract_DATA_DIR}/modules/${_module}")
         if(NOT EXISTS "${_path}")
            message(
               FATAL_ERROR
               "Forge Contract guest component module is missing: ${_path}"
            )
         endif()
         list(APPEND _module_paths "${_path}")
      endforeach()

      add_library("${_target}" STATIC)
      add_library("Forge::${ARG_TARGET}" ALIAS "${_target}")
      target_sources(
         "${_target}"
         PUBLIC
            FILE_SET forge_contract_guest_modules TYPE CXX_MODULES
            BASE_DIRS "${ForgeContract_DATA_DIR}/modules"
            FILES ${_module_paths}
      )
      set_source_files_properties(
         ${_module_paths}
         PROPERTIES OBJECT_DEPENDS "${ForgeContract_ATTR_PLUGIN}"
      )
      if(ARG_ARCHIVE)
         set(_archive "${ForgeContract_SYSROOT}/lib/${ARG_ARCHIVE}")
         if(NOT EXISTS "${_archive}")
            message(
               FATAL_ERROR
               "Forge Contract guest component archive is missing: ${_archive}"
            )
         endif()
         add_library("${_target}_archive" STATIC IMPORTED GLOBAL)
         set_target_properties(
            "${_target}_archive" PROPERTIES IMPORTED_LOCATION "${_archive}"
         )
         target_link_libraries("${_target}" PUBLIC "${_target}_archive")
      endif()
      target_compile_features("${_target}" PUBLIC cxx_std_23)
      set_target_properties(
         "${_target}"
         PROPERTIES
            CXX_MODULE_STD OFF
            CXX_SCAN_FOR_MODULES ON
            FORGE_CONTRACT_OWNER_ID "${ARG_ID}"
            FORGE_CONTRACT_COMPONENT TRUE
            FORGE_CONTRACT_MODULE_NAMES "${ARG_MODULE_NAMES}"
            FORGE_CONTRACT_MODULE_BASES "${ForgeContract_DATA_DIR}/modules"
      )
      _forge_contract_configure_guest_target("${_target}")
      _forge_contract_module_path("${_target}" _module_path)
      _forge_contract_publish_metadata(
         "${_target}"
         MODULE_BASES "${ForgeContract_DATA_DIR}/modules"
         MODULE_PATH "${_module_path}"
      )
      _forge_contract_register_owner("${ARG_ID}" "${_target}")
      set_property(
         GLOBAL PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_TARGET" "${_target}"
      )
      set_property(
         GLOBAL APPEND PROPERTY FORGE_CONTRACT_GUEST_COMPONENT_IDS "${ARG_ID}"
      )
   endfunction()
endif()

function(_forge_contract_finalize_guest_components)
   if(NOT FORGE_CONTRACT_GUEST)
      return()
   endif()
   get_property(_ids GLOBAL PROPERTY FORGE_CONTRACT_GUEST_COMPONENT_IDS)
   foreach(_id IN LISTS _ids)
      _forge_contract_owner_target("${_id}" _target)
      _forge_contract_id_key("${_id}" _key)
      get_property(
         _dependencies GLOBAL
         PROPERTY "FORGE_CONTRACT_COMPONENT_${_key}_DEPENDENCY_IDS"
      )
      foreach(_dependency IN LISTS _dependencies)
         _forge_contract_owner_target("${_dependency}" _dependency_target)
         target_link_libraries("${_target}" PUBLIC "${_dependency_target}")
      endforeach()
   endforeach()

   _forge_contract_owner_target("forge.contract.runtime" _runtime_target)
   if(NOT TARGET _forge_contract_guest_eosio)
      add_library(_forge_contract_guest_eosio INTERFACE)
      target_include_directories(
         _forge_contract_guest_eosio
         INTERFACE "${ForgeContract_DATA_DIR}/include"
      )
      target_link_libraries(
         _forge_contract_guest_eosio INTERFACE "${_runtime_target}"
      )
   endif()
   set_property(
      GLOBAL PROPERTY FORGE_CONTRACT_GUEST_EOSIO_TARGET
      _forge_contract_guest_eosio
   )
endfunction()

function(_forge_contract_dependency input scope target_output owner_output)
   _forge_contract_resolve_target("${input}" _target)
   if(NOT FORGE_CONTRACT_GUEST)
      # Native composition uses ordinary CMake dependencies. Only guest compilation
      # requires SDK ownership; aliases still share one resolved duplicate identity.
      set(${target_output} "${_target}" PARENT_SCOPE)
      set(${owner_output} "${_target}" PARENT_SCOPE)
      return()
   endif()
   get_target_property(_owner "${_target}" FORGE_CONTRACT_OWNER_ID)
   if(NOT _owner)
      get_target_property(
         _owner "${_target}" FORGE_CONTRACT_GUEST_COMPONENT_ID
      )
   endif()
   if(NOT _owner)
      message(
         FATAL_ERROR
         "${scope} contract dependency is not guest-compatible: ${input}"
      )
   endif()
   set(${target_output} "${_target}" PARENT_SCOPE)
   set(${owner_output} "${_owner}" PARENT_SCOPE)
endfunction()

function(forge_add_contract_library target)
   cmake_parse_arguments(
      ARG
      ""
      "ID"
      "MODULE_BASE_DIRS;MODULE_SOURCES;SOURCES;PUBLIC_HEADERS;PUBLIC_LIBRARIES;PRIVATE_LIBRARIES"
      ${ARGN}
   )
   if(ARG_UNPARSED_ARGUMENTS)
      message(
         FATAL_ERROR
         "forge_add_contract_library(${target}) received unknown arguments: "
         "${ARG_UNPARSED_ARGUMENTS}"
      )
   endif()
   if(TARGET "${target}")
      message(
         FATAL_ERROR
         "forge_add_contract_library target already exists: ${target}"
      )
   endif()
   if(NOT ARG_ID OR NOT ARG_ID MATCHES "^[A-Za-z0-9][A-Za-z0-9_.-]*$")
      message(
         FATAL_ERROR
         "forge_add_contract_library(${target}) requires a canonical ID"
      )
   endif()
   if(NOT ARG_MODULE_BASE_DIRS OR NOT ARG_MODULE_SOURCES)
      message(
         FATAL_ERROR
         "forge_add_contract_library(${target}) requires "
         "MODULE_BASE_DIRS and MODULE_SOURCES"
      )
   endif()
   set(_source_root "${CMAKE_CURRENT_SOURCE_DIR}")
   _forge_contract_require_product_source(
      "${_source_root}" "contract library source root"
   )

   set(_module_bases)
   foreach(_base IN LISTS ARG_MODULE_BASE_DIRS)
      get_filename_component(
         _absolute "${_base}" REALPATH BASE_DIR "${_source_root}"
      )
      if(NOT IS_DIRECTORY "${_absolute}")
         message(
            FATAL_ERROR
            "contract module base is not a directory: ${_absolute}"
         )
      endif()
      _forge_contract_require_source_or_binary(
         "${_source_root}" "${_absolute}" "contract module base"
      )
      list(APPEND _module_bases "${_absolute}")
   endforeach()
   list(REMOVE_DUPLICATES _module_bases)

   foreach(_role MODULE_SOURCES SOURCES PUBLIC_HEADERS)
      set(_files)
      foreach(_file IN LISTS ARG_${_role})
         _forge_contract_normalize_file(
            "${_source_root}" "${_file}" "contract ${_role} file" _absolute
         )
         if(_absolute IN_LIST _all_files)
            message(
               FATAL_ERROR
               "contract file is declared more than once: ${_absolute}"
            )
         endif()
         list(APPEND _all_files "${_absolute}")
         list(APPEND _files "${_absolute}")
      endforeach()
      set(_${_role} "${_files}")
   endforeach()

   set(_public_targets)
   set(_public_ids)
   foreach(_dependency IN LISTS ARG_PUBLIC_LIBRARIES)
      _forge_contract_dependency(
         "${_dependency}" "PUBLIC" _dependency_target _dependency_id
      )
      if(_dependency_id IN_LIST _public_ids OR _dependency_id IN_LIST _private_ids)
         message(FATAL_ERROR "duplicate contract dependency owner: ${_dependency_id}")
      endif()
      list(APPEND _public_targets "${_dependency_target}")
      list(APPEND _public_ids "${_dependency_id}")
   endforeach()
   set(_private_targets)
   set(_private_ids)
   foreach(_dependency IN LISTS ARG_PRIVATE_LIBRARIES)
      _forge_contract_dependency(
         "${_dependency}" "PRIVATE" _dependency_target _dependency_id
      )
      if(_dependency_id IN_LIST _public_ids OR _dependency_id IN_LIST _private_ids)
         message(FATAL_ERROR "duplicate contract dependency owner: ${_dependency_id}")
      endif()
      list(APPEND _private_targets "${_dependency_target}")
      list(APPEND _private_ids "${_dependency_id}")
   endforeach()

   add_library("${target}" STATIC)
   target_sources(
      "${target}"
      PUBLIC
         FILE_SET forge_contract_modules TYPE CXX_MODULES
         BASE_DIRS ${_module_bases}
         FILES ${_MODULE_SOURCES}
   )
   if(_SOURCES)
      target_sources("${target}" PRIVATE ${_SOURCES})
   endif()
   if(_PUBLIC_HEADERS)
      target_sources(
         "${target}"
         PUBLIC
            FILE_SET forge_contract_headers TYPE HEADERS
            BASE_DIRS ${_module_bases}
            FILES ${_PUBLIC_HEADERS}
      )
   endif()
   target_include_directories(
      "${target}" PRIVATE "$<BUILD_INTERFACE:${_source_root}>"
   )
   if(_public_targets)
      target_link_libraries("${target}" PUBLIC ${_public_targets})
   endif()
   if(_private_targets)
      target_link_libraries("${target}" PRIVATE ${_private_targets})
   endif()
   target_compile_features("${target}" PUBLIC cxx_std_23)
   set_target_properties(
      "${target}"
      PROPERTIES
         CXX_MODULE_STD OFF
         CXX_SCAN_FOR_MODULES ON
         FORGE_CONTRACT_OWNER_ID "${ARG_ID}"
         FORGE_CONTRACT_LIBRARY TRUE
         FORGE_CONTRACT_MODULE_BASES "${_module_bases}"
         FORGE_CONTRACT_MODULE_NAMES ""
   )
   _forge_contract_configure_guest_target("${target}")
   if(FORGE_CONTRACT_GUEST)
      _forge_contract_id_key("${ARG_ID}" _owner_key)
      _forge_contract_object_list(
         "${target}" "library-${_owner_key}" _object_list
      )
      _forge_contract_module_path("${target}" _module_path)
      _forge_contract_publish_metadata(
         "${target}"
         COMPILATION "${ARG_ID}|${_object_list}"
         MODULE_BASES ${_module_bases}
         MODULE_PATH "${_module_path}"
      )
   endif()
   _forge_contract_register_owner("${ARG_ID}" "${target}")
   _forge_contract_freeze_guest_target(
      "${target}" "forge_add_contract_library(${target})"
   )
endfunction()
