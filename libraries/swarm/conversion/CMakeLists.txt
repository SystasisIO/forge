add_library(forge_swarm_conversion STATIC conversion.cpp)
forge_target_modules_at(forge_swarm_conversion swarm/conversion)

target_include_directories(forge_swarm_conversion PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_conversion PUBLIC
   forge_core
   forge_swarm_db
   forge_swarm_protocol
)
