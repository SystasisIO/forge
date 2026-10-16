cmake_minimum_required(VERSION 3.31)

project(forge-package-codec-xml-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS codec_xml)

get_target_property(_forge_xml_bmi_includes Forge::forge_codec_xml IMPORTED_CXX_MODULES_INCLUDE_DIRECTORIES)
foreach(_include IN LISTS _forge_xml_bmi_includes)
   if(_include MATCHES "(^|/)vendor/pugixml(/|$)")
      message(FATAL_ERROR "Installed codec_xml exports its source-private pugixml include: ${_include}")
   endif()
endforeach()

add_executable(forge_package_codec_xml_component main.cpp)
target_link_libraries(forge_package_codec_xml_component PRIVATE Forge::forge_codec_xml)
