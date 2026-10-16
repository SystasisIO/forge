include_guard(GLOBAL)

function(forge_configure_softfloat_abi target)
   if(NOT TARGET ${target})
      message(FATAL_ERROR "Unknown SoftFloat target: ${target}")
   endif()
   # The pinned donor leaves THREAD_LOCAL empty for C definitions. All
   # definitions and C/C++ declarations must use the same variable linkage.
   # __thread is the supported Clang/GCC baseline already used by its C++ header.
   target_compile_definitions(${target} PUBLIC THREAD_LOCAL=__thread)
endfunction()
