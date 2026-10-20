add_library(forge_swarm_protocol STATIC metadata.cpp metadata_reader.cpp metadata_assembler.cpp peer_message.cpp)
forge_target_modules_at(forge_swarm_protocol swarm/protocol)

target_include_directories(forge_swarm_protocol PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_protocol PUBLIC
   forge_swarm_core
   forge_raw
   forge_reflect
   forge_exceptions
   Boost::headers
)
