add_library(
   forge_otlp STATIC
   crash.cpp
   log_exporter.cpp
   log_exporter_impl.cpp
   log_sink.cpp
)
forge_target_modules(forge_otlp otlp)

target_include_directories(forge_otlp PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_otlp PUBLIC
      forge_exceptions
      forge_log
      forge_asio
      forge_net_http
)
