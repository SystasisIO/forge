add_library(forge_api_chain STATIC binding.cpp outbox.cpp)
forge_target_modules_at(forge_api_chain api/chain)

target_include_directories(forge_api_chain
   PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      ${FORGE_PUBLIC_INCLUDE_DIRS}
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(forge_api_chain PUBLIC
   Boost::asio
   forge_api_core
   forge_chain_api
   forge_chain_protocol
   forge_chain_transaction
   forge_db_object
   forge_net_p2p
   PRIVATE forge_exceptions forge_log forge_raw)
