add_library(forge_swarm_runtime STATIC
   admission.cpp
   capabilities.cpp
   quota.cpp
   quota_impl.cpp
   resource_budget.cpp
   call_scope.cpp
   engine.cpp
   engine_impl.cpp
   sharing.cpp
   download.cpp
   operation_state.cpp
   peer_session.cpp
   peer_session_download.cpp
)
forge_target_modules_at(forge_swarm_runtime swarm/runtime)
target_include_directories(forge_swarm_runtime PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_runtime PUBLIC
   forge_swarm_api forge_swarm_core forge_swarm_protocol forge_api_core
   forge_api_auth forge_asio forge_exceptions Boost::asio)
target_link_libraries(forge_swarm_runtime PRIVATE forge_crypto_core)
