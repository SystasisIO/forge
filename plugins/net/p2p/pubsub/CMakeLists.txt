add_library(
   forge_plugins_net_p2p_pubsub STATIC
      descriptor.cpp
      plugin.cpp
      config.cpp
      project_message.cpp
      plugin_impl.cpp
      plugin_impl_lifecycle.cpp
      api_impl_topics.cpp
      api_impl.cpp
)
forge_target_modules_at(forge_plugins_net_p2p_pubsub plugins/net/p2p/pubsub)

target_include_directories(
   forge_plugins_net_p2p_pubsub
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
      "$<BUILD_INTERFACE:${FORGE_PLUGINS_NET_P2P_PUBSUB_INCLUDE_DIR}>"
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
   forge_plugins_net_p2p_pubsub PUBLIC
      forge_plugins_net_p2p_node
      forge_app
      forge_api_core
      forge_net_p2p
      forge_config_core
      forge_schema
      forge_raw
      forge_exceptions
      forge_asio
      Boost::asio
)
