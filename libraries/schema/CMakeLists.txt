add_library(forge_schema STATIC constraint.cpp)
forge_target_modules(forge_schema schema)

target_include_directories(forge_schema PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_schema PUBLIC
      forge_exceptions
      forge_reflect
      Boost::headers
   PRIVATE
      Boost::regex
)

target_compile_definitions(forge_schema PRIVATE
   BOOST_REGEX_MAX_STATE_COUNT=100000
   BOOST_REGEX_MAX_BLOCKS=64
   BOOST_REGEX_BLOCKSIZE=4096
)
