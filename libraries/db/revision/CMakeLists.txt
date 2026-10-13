add_library(
   forge_db_revision STATIC
   store.cpp
   store_impl.cpp
   transaction.cpp
   transaction_impl.cpp
)
forge_target_modules_at(forge_db_revision db/revision)

target_include_directories(forge_db_revision PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_include_directories(forge_db_revision PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/details>")

target_link_libraries(
   forge_db_revision PUBLIC
      Boost::asio
      forge_db_core
      forge_db_object
      forge_exceptions
      forge_db_ids
      forge_raw
   PRIVATE
      forge_log
)
