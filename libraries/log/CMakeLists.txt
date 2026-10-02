add_library(
   forge_log STATIC
   record.cpp
   logger.cpp
   logger_config.cpp
)
forge_target_modules(forge_log log)

target_include_directories(forge_log PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_log
   PUBLIC
      forge_core
      forge_exceptions
      forge_reflect
      forge_variant
      Boost::headers
   PRIVATE
      forge_chrono
      Boost::dll
)
if(FORGE_HAS_STD_STACKTRACE)
   target_compile_definitions(forge_log PRIVATE FORGE_HAS_STD_STACKTRACE=1)
elseif(TARGET Boost::stacktrace_basic)
   target_compile_definitions(
      forge_log PRIVATE
         FORGE_HAS_BOOST_STACKTRACE=1
         BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
   )
   target_link_libraries(forge_log PRIVATE Boost::stacktrace_basic)
endif()
target_compile_definitions(
   forge_log
   PUBLIC OPENSSL_API_COMPAT=0x30000000L
          OPENSSL_NO_DEPRECATED
)
