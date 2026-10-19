if(NOT FORGE_ENABLE_TLSNOTARY)
   return()
endif()

add_library(forge_plugins_net_tlsnotary_session STATIC plugin.cpp plugin_impl.cpp api_impl.cpp config.cpp)
forge_target_modules_at(forge_plugins_net_tlsnotary_session plugins/net/tlsnotary/session)
target_include_directories(forge_plugins_net_tlsnotary_session PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS}
   "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(forge_plugins_net_tlsnotary_session PUBLIC
   forge_net_tlsnotary forge_app forge_api_core forge_config_core forge_schema forge_exceptions Boost::asio)
