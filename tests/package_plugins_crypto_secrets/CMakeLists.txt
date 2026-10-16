cmake_minimum_required(VERSION 3.31)

project(forge-package-plugins-crypto-secrets LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS plugins_crypto_secrets app asio)

if(NOT TARGET Forge::forge_codec_base64 OR NOT TARGET Forge::forge_codec_hex)
   message(FATAL_ERROR "plugins_crypto_secrets must provide its private codec dependencies")
endif()

add_executable(forge_package_plugins_crypto_secrets main.cpp)
target_link_libraries(forge_package_plugins_crypto_secrets PRIVATE
   Forge::forge_plugins_crypto_secrets
   Forge::forge_app
   Forge::forge_asio
)
