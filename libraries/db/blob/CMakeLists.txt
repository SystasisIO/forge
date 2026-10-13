add_library(
   forge_db_blob STATIC
   details/snapshot_access.hxx
   types.cpp
   store.cpp
   snapshot.cpp
   transaction.cpp
   transaction_participant_impl.cpp
)
forge_target_modules_at(forge_db_blob db/blob)

target_include_directories(forge_db_blob PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_include_directories(forge_db_blob PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/details>")
target_include_directories(
   forge_db_blob INTERFACE
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/cmake/Forge/details/db_blob>"
)

install(
   FILES details/snapshot_access.hxx
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge/details/db_blob
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

target_link_libraries(
   forge_db_blob PUBLIC
      Boost::asio
      forge_codec_hex
      forge_crypto_digest
      forge_db_core
      forge_exceptions
      forge_db_ids
      forge_raw
      forge_variant
   PRIVATE
      forge_log
)
