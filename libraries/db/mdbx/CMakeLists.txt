add_library(
   forge_db_mdbx STATIC
   driver.cpp
   diagnostics.cpp
   native_diagnostics.cpp
   result_diagnostics.cpp
   driver_impl.cpp
   environment.cpp
   transaction_session.cpp
   snapshot_session.cpp
   error.cpp
   scan.cpp
)
set(FORGE_db_mdbx_DISABLED_MODULES error result_diagnostics)
forge_target_modules_at(forge_db_mdbx db/mdbx)
target_sources(
   forge_db_mdbx PRIVATE
      FILE_SET private_modules TYPE CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
         "${CMAKE_CURRENT_SOURCE_DIR}/include/forge/db/mdbx/error.cppm"
         "${CMAKE_CURRENT_SOURCE_DIR}/include/forge/db/mdbx/result_diagnostics.cppm"
)

target_include_directories(forge_db_mdbx PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_include_directories(
   forge_db_mdbx PRIVATE
      "${PROJECT_SOURCE_DIR}/vendor/libmdbx"
)

target_link_libraries(
   forge_db_mdbx PUBLIC
      forge_asio
      forge_db_core
      forge_exceptions
   PRIVATE
      forge_log
      mdbx-internal
)
