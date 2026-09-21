add_library(
   forge_plugins_log_otlp STATIC
   plugin.cpp
   config.cpp
   api_impl.cpp
   plugin_impl.cpp
)
forge_target_modules_at(forge_plugins_log_otlp plugins/log/otlp)

target_include_directories(
   forge_plugins_log_otlp
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
      "$<BUILD_INTERFACE:${FORGE_PLUGINS_LOG_OTLP_INCLUDE_DIR}>"
)
target_include_directories(forge_plugins_log_otlp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
   forge_plugins_log_otlp PUBLIC
      forge_app
      forge_api_core
      forge_config_core
      forge_schema
      forge_log
      forge_otlp
      forge_net_http
      forge_asio
      forge_exceptions
      Boost::asio
   PRIVATE
      forge_plugins_crypto_secrets
)
