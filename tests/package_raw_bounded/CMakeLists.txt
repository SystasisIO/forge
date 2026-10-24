cmake_minimum_required(VERSION 3.31)
project(ForgeRawBoundedConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS raw)
find_package(Boost REQUIRED)

add_executable(forge_raw_bounded_consumer bounded.cpp codec_only.cpp)
target_link_libraries(forge_raw_bounded_consumer PRIVATE Forge::forge_raw Boost::headers)

enable_testing()
add_test(NAME forge_raw_bounded_consumer COMMAND forge_raw_bounded_consumer)

set(FORGE_SOURCE_ROOT "" CACHE PATH "Optional Forge source root for the unchanged Raw regression suite")
if(FORGE_SOURCE_ROOT)
   find_package(Forge CONFIG REQUIRED COMPONENTS api_core_values codec_hex crypto_digest net_http_values raw)
   add_executable(
      forge_raw_legacy_regression
      "${FORGE_SOURCE_ROOT}/tests/unit/main.cpp"
      "${FORGE_SOURCE_ROOT}/tests/raw/test_raw.cpp"
   )
   target_include_directories(forge_raw_legacy_regression PRIVATE "${FORGE_SOURCE_ROOT}/tests/unit")
   target_link_libraries(
      forge_raw_legacy_regression
      PRIVATE
         Forge::forge_api_core_values
         Forge::forge_codec_hex
         Forge::forge_crypto_digest
         Forge::forge_net_http_values
         Forge::forge_raw
         Boost::headers
   )
   add_test(NAME forge_raw_legacy_regression COMMAND forge_raw_legacy_regression)
endif()
