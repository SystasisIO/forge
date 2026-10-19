set(_modules
   include/forge/auth/attestation/document.cppm
   include/forge/auth/attestation/exceptions.cppm
   include/forge/auth/attestation/signed_document.cppm
   include/forge/auth/attestation/verification_policy.cppm
)
set(_sources document.cpp signed_document.cpp verification_policy.cpp)
if(NOT FORGE_CONTRACT_GUEST)
   list(APPEND _modules include/forge/auth/attestation/document_schema.cppm)
   list(APPEND _sources document_schema.cpp)
endif()

set(_raw forge_raw)
if(TARGET Forge::forge_raw)
   set(_raw Forge::forge_raw)
endif()
forge_add_contract_library(forge_auth_attestation
   ID forge.auth.attestation
   MODULE_BASE_DIRS include
   MODULE_SOURCES ${_modules}
   SOURCES ${_sources}
   PUBLIC_LIBRARIES ${_raw}
)

if(NOT FORGE_CONTRACT_GUEST)
   target_include_directories(forge_auth_attestation PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
   target_link_libraries(forge_auth_attestation PUBLIC forge_exceptions forge_schema forge_variant Boost::headers)
endif()
