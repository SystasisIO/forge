add_library(forge_swarm_api STATIC peer.cpp)
forge_target_modules_at(forge_swarm_api swarm/api)

target_include_directories(forge_swarm_api PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_api PUBLIC
   forge_swarm_protocol
   forge_api_core
   forge_api_auth
   Boost::asio
)
