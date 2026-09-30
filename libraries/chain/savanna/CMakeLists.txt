add_library(
   forge_chain_savanna STATIC
   types.cpp
   policy.cpp
   finality_core.cpp
   qc.cpp
   vote.cpp
   vote_accumulator.cpp
   vote_accumulator_impl.cpp
   finalizer_safety.cpp
   validation.cpp
   rank.cpp
   policy_state.cpp
   extensions.cpp
   genesis.cpp
   header_state.cpp
   checkpoint.cpp
   admission.cpp
   finality_witness.cpp
)
forge_target_modules_at(forge_chain_savanna chain/savanna)

target_include_directories(forge_chain_savanna PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_chain_savanna PUBLIC
      Boost::headers
      Threads::Threads
      forge_chain_core
      forge_chain_quorum
      forge_chain_savanna_values
      forge_chain_protocol
      forge_crypto_asymmetric
      forge_crypto_bls
      forge_crypto_digest
      forge_exceptions
      forge_raw
      forge_variant
)
