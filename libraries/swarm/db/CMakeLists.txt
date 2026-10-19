add_library(forge_swarm_db STATIC)
forge_target_modules_at(forge_swarm_db swarm/db)

target_include_directories(forge_swarm_db PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_db PUBLIC
   forge_db_object
   forge_swarm_core
   Boost::headers
)
