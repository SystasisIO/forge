add_library(
   forge_tooling_abi STATIC
   command.cpp
   compilation_metadata.cpp
   enum_wire_resolver.cpp
   generator.cpp
)
forge_target_modules_at(forge_tooling_abi tooling/abi)

set_target_properties(forge_tooling_abi PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(forge_tooling_abi PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_include_directories(forge_tooling_abi SYSTEM PRIVATE ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(forge_tooling_abi PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(
   forge_tooling_abi
   PUBLIC
      forge_chain_protocol
      forge_codec_json
   PRIVATE
      forge_db_ids
      clang-cpp
      LLVM
)
