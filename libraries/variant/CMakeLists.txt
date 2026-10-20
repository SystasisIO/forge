add_library(
   forge_variant STATIC
   chrono.cpp
   format.cpp
   schema.cpp
   value.cpp
   value_object.cpp
)
forge_target_modules(forge_variant variant)

target_include_directories(forge_variant PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_variant
   PUBLIC
      forge_codec_base64
      forge_codec_hex
      forge_core
      forge_exceptions
      forge_reflect
      forge_schema
      Boost::headers
      Boost::multi_index
      Boost::multiprecision
   PRIVATE
      forge_chrono
)
target_compile_definitions(
   forge_variant
   PUBLIC OPENSSL_API_COMPAT=0x30000000L
          OPENSSL_NO_DEPRECATED
)
