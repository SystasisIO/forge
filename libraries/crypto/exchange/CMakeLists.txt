add_library(forge_crypto_exchange STATIC exchange.cpp)
forge_target_modules_at(forge_crypto_exchange crypto/exchange)
target_include_directories(forge_crypto_exchange PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_crypto_exchange PUBLIC forge_crypto_core forge_crypto_digest forge_raw forge_exceptions Boost::headers)
forge_target_contract_guest_component(forge_crypto_exchange ID forge.crypto.exchange)

if(FORGE_CONTRACT_SDK_HOST_PROFILE AND BUILD_TESTING)
   include("${PROJECT_SOURCE_DIR}/tests/crypto_exchange.cmake")
endif()
