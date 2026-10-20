# Injected only into the macOS sanitizer build, without editing upstream files.
function(_cjose_qualification_upstream_test_warning_policy)
   if(NOT APPLE OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT TARGET check_cjose)
      message(FATAL_ERROR "Expected the macOS Clang upstream test target")
   endif()
   get_target_property(test_options check_cjose COMPILE_OPTIONS)
   if(NOT "-Werror" IN_LIST test_options)
      message(FATAL_ERROR "The upstream test target must retain its Werror policy")
   endif()
   # SDK-deprecated sprintf in unchanged upstream test helpers remains visible
   # as a warning. No production/object/public-consumer target is relaxed.
   target_compile_options(check_cjose PRIVATE -Wno-error=deprecated-declarations)
   message(STATUS "QUALIFICATION_UPSTREAM_TEST_DEPRECATION_WARNING_ONLY")
endfunction()

# The project() hook runs before targets exist; apply after upstream defines them.
cmake_language(DEFER CALL _cjose_qualification_upstream_test_warning_policy)
