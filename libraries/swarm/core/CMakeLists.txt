add_library(forge_swarm_core STATIC descriptor.cpp merkle.cpp identity.cpp)
forge_target_modules_at(forge_swarm_core swarm/core)

target_include_directories(forge_swarm_core PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_core PUBLIC
   forge_core
   forge_crypto_digest
   forge_raw
   forge_exceptions
   Boost::headers
)
