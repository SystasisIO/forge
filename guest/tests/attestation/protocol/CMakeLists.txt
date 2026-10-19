forge_add_contract_library(attestation_fixture_protocol
   ID fixture.attestation
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/fixture/grant.cppm
   PUBLIC_LIBRARIES Forge::forge_auth_attestation
)
