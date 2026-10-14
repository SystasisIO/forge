forge_add_contract_library(
   api_wasm_fixture_protocol
   ID fixture.api_wasm
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/fixture/api_wasm.cppm
   PUBLIC_LIBRARIES Forge::forge_api_wasm
)
