add_library(
   forge_plugins_net_p2p_resolver STATIC
      descriptor.cpp
      plugin.cpp
      config.cpp
      plugin_impl.cpp
      api_impl.cpp
      publication_catalog.cpp
      resolver_protocol.cpp
      managed_api_impl.cpp
      managed_remote_invoker.cpp
      managed_remote_state.cpp
      details/cache_record.hxx
      details/publication_catalog.hxx
      details/resolver_protocol.hxx
      details/managed_remote_state.hxx
)
forge_target_modules_at(forge_plugins_net_p2p_resolver plugins/net/p2p/resolver)

target_include_directories(
   forge_plugins_net_p2p_resolver
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
      "$<BUILD_INTERFACE:${FORGE_PLUGINS_NET_P2P_RESOLVER_INCLUDE_DIR}>"
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
   forge_plugins_net_p2p_resolver PUBLIC
      forge_plugins_net_p2p_node
      forge_asio
      forge_app
      forge_api_core
      forge_api_transport
      forge_api_p2p
      forge_net_p2p
      forge_config_core
      forge_schema
      forge_exceptions
      Boost::asio
)
