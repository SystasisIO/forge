add_library(forge_codec_json_values STATIC)
target_sources(forge_codec_json_values PUBLIC
   FILE_SET CXX_MODULES
   BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/forge/codec/json/document.cppm"
)
target_compile_features(forge_codec_json_values PUBLIC cxx_std_23)
target_include_directories(forge_codec_json_values PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_codec_json_values PUBLIC forge_raw)
forge_target_contract_guest_component(forge_codec_json_values ID forge.codec.json_values)

add_library(forge_codec_json STATIC exact.cpp json.cpp)
set(FORGE_codec_json_DISABLED_MODULES document)
forge_target_modules_at(forge_codec_json codec/json)
unset(FORGE_codec_json_DISABLED_MODULES)

target_include_directories(forge_codec_json PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_codec_json PUBLIC
      forge_codec_json_values
      forge_core
      forge_config_core
      forge_schema
      forge_variant
   PRIVATE
      glaze::glaze
)
target_compile_definitions(
   forge_codec_json
   PUBLIC OPENSSL_API_COMPAT=0x30000000L
          OPENSSL_NO_DEPRECATED
)
