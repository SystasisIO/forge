add_library(forge_swarm_storage STATIC
   catalog.cpp
   catalog_impl.cpp
   catalog_checkpoint.cpp
   catalog_store_access.cpp
   file_store.cpp
   file_store_impl.cpp
   file_store_impl_pieces.cpp
   file_store_impl_checkpoint.cpp
   file_store_impl_writer.cpp
   file_layout.cpp
)
forge_target_modules_at(forge_swarm_storage swarm/storage)
target_include_directories(forge_swarm_storage PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_swarm_storage PUBLIC
   forge_swarm_core forge_swarm_db forge_db_object forge_db_core forge_asio forge_exceptions)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_compile_definitions(forge_swarm_storage PRIVATE _GNU_SOURCE)
endif()
