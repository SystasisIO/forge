add_library(
   forge_plugins_net_p2p_diagnostics STATIC
      plugin.cpp
      api_impl.cpp
      config.cpp
      plugin_impl.cpp
)
forge_target_modules_at(forge_plugins_net_p2p_diagnostics plugins/net/p2p/diagnostics)

target_include_directories(
   forge_plugins_net_p2p_diagnostics
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
      "$<BUILD_INTERFACE:${FORGE_PLUGINS_NET_P2P_DIAGNOSTICS_INCLUDE_DIR}>"
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
   forge_plugins_net_p2p_diagnostics PUBLIC
      forge_plugins_net_p2p_node
      forge_app
      forge_api_core
      forge_api_p2p
      forge_net_p2p
      forge_config_core
      forge_schema
      forge_exceptions
      Boost::asio
)
