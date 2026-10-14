forge_add_contract_library(
   wire_values_protocol
   ID fixture.wire_values
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/fixture/wire_values.cppm
   PUBLIC_LIBRARIES Forge::forge_api_core_values Forge::forge_net_http_values Forge::forge_raw
)
