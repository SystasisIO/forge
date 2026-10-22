include(ForgeCEL)

add_library(forge_expression STATIC program.cpp)
forge_target_modules(forge_expression expression)
target_include_directories(forge_expression PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_expression PUBLIC forge_exceptions forge_variant
   PRIVATE ForgeCEL::backend)
