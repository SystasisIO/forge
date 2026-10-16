get_filename_component(FORGE_XML_PUGIXML_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../vendor/pugixml/src" ABSOLUTE)

add_library(
   forge_codec_xml STATIC
   xml.cpp
   "${FORGE_XML_PUGIXML_SOURCE_DIR}/pugixml.cpp"
)
forge_target_modules_at(forge_codec_xml codec/xml)

target_include_directories(
   forge_codec_xml
   PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS}
   PRIVATE "$<BUILD_INTERFACE:${FORGE_XML_PUGIXML_SOURCE_DIR}>"
)
target_link_libraries(
   forge_codec_xml PUBLIC
      forge_core
      forge_reflect
      forge_schema
)
target_compile_definitions(
   forge_codec_xml
   PRIVATE
      PUGIXML_NO_XPATH
      PUGIXML_NO_EXCEPTIONS
)
