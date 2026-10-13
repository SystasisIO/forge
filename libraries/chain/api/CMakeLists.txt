add_library(
   forge_chain_api STATIC
   abi.cpp
   authenticated_audit_verifier.cpp
   finality.cpp
   limits.cpp
   raw_client.cpp
   savanna_finality_trust_store.cpp
   details/savanna_finality_trust_store.hxx
   savanna_finality_verifier.cpp
   submission_client.cpp
   table_key.cpp
   contract_table_projection_verifier.cpp
   verified_client.cpp
   verified_client_factory.cpp
)
forge_target_modules_at(forge_chain_api chain/api)

target_include_directories(forge_chain_api PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_chain_api PUBLIC
      Boost::asio
      Boost::headers
      forge_api_auth
      forge_api_core
      forge_api_http
      forge_chain_protocol
      forge_chain_savanna
      forge_chain_transaction
      forge_crypto_asymmetric
      forge_crypto_bls
      forge_crypto_digest
      forge_db_authenticated
      forge_exceptions
      forge_net_http
      forge_variant
)

target_link_libraries(
   forge_chain_api PRIVATE
      forge_asio
      forge_codec_hex
      forge_raw
      forge_schema
)
