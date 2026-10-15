cmake_minimum_required(VERSION 3.31)
project(forge-package-auth-liboauth2-optional-dependency LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT EXISTS "${Forge_DIR}/ForgeConfig.cmake")
   message(FATAL_ERROR "This fixture requires the real build-tree or installed Forge package")
endif()

set(CMAKE_DISABLE_FIND_PACKAGE_Jansson TRUE)
find_package(Forge CONFIG QUIET COMPONENTS auth_liboauth2)

if(NOT DEFINED Forge_FOUND OR Forge_FOUND)
   message(FATAL_ERROR "Optional Forge lookup must return Forge_FOUND=FALSE for missing Jansson")
endif()
if(NOT Forge_NOT_FOUND_MESSAGE MATCHES "dependency (cjose|OAuth2Dependencies)")
   message(FATAL_ERROR "Optional Forge lookup did not report the auth backend dependency failure")
endif()
if(NOT cjose_NOT_FOUND_MESSAGE MATCHES "dependency Jansson")
   message(FATAL_ERROR "Optional Forge lookup failed for a reason other than missing Jansson")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/optional-dependency.ok" "PASS\n")
