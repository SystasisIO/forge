add_library(forge_plugins_chain_outbox STATIC
   api_impl.cpp
   plugin.cpp
   plugin_impl.cpp
   plugin_impl_network.cpp
   plugin_impl_signer.cpp
   plugin_impl_store.cpp
   types.cpp)
forge_target_modules_at(forge_plugins_chain_outbox plugins/chain/outbox)

target_include_directories(forge_plugins_chain_outbox
   PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      ${FORGE_PUBLIC_INCLUDE_DIRS}
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(forge_plugins_chain_outbox PUBLIC
   Boost::asio
   forge_api_chain
   forge_app
   forge_chain_api
   forge_config_core
   forge_plugins_db_store
   forge_schema
   PRIVATE
      forge_api_auth
      forge_api_http
      forge_asio
      forge_exceptions
      forge_log
      forge_net_http
      forge_plugins_net_p2p_resolver)


if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
   target_compile_options(forge_plugins_chain_outbox PRIVATE -fmodules-reduced-bmi)
endif()
