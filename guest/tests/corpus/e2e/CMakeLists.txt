cmake_minimum_required(VERSION 3.31)

project(ForgeContractCompatibilityCorpusE2E LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT FORGE_CONTRACT_CORPUS_ARTIFACT_DIR)
   message(FATAL_ERROR "FORGE_CONTRACT_CORPUS_ARTIFACT_DIR is required")
endif()
if(NOT EXISTS "${FORGE_CONTRACT_CORPUS_ARTIFACT_DIR}")
   message(
      FATAL_ERROR
      "FORGE_CONTRACT_CORPUS_ARTIFACT_DIR does not exist: "
      "${FORGE_CONTRACT_CORPUS_ARTIFACT_DIR}"
   )
endif()

find_package(
   Forge CONFIG REQUIRED
   COMPONENTS chain_protocol tooling_testing crypto_digest exceptions raw vm_wasm_interpret
)
find_package(Boost REQUIRED)

add_executable(forge_contract_corpus_tests contract_tests.cpp)
target_link_libraries(
   forge_contract_corpus_tests
   PRIVATE
      Forge::forge_chain_protocol
      Forge::forge_tooling_testing
      Forge::forge_crypto_digest
      Forge::forge_exceptions
      Forge::forge_raw
      Forge::forge_vm_wasm_interpret
      Boost::headers
)
target_compile_definitions(
   forge_contract_corpus_tests
   PRIVATE
      FORGE_CONTRACT_CORPUS_BUILD_DIR="${FORGE_CONTRACT_CORPUS_ARTIFACT_DIR}"
      FORGE_CONTRACT_CORPUS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

enable_testing()
add_test(NAME forge_contract_corpus_e2e COMMAND forge_contract_corpus_tests)
