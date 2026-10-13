add_library(
   forge_rocksdb STATIC
   native.cpp
   native_diagnostics.cpp
   snapshot_impl.cpp
   store.cpp
   store_impl.cpp
   store_snapshot.cpp
   store_transaction.cpp
   transaction_impl.cpp
   types.cpp
)
forge_target_modules(forge_rocksdb rocksdb)

target_include_directories(forge_rocksdb PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_rocksdb PUBLIC
      forge_exceptions
      forge_schema
   PRIVATE
      forge_log
      RocksDB::rocksdb
)

target_compile_definitions(forge_rocksdb PUBLIC FORGE_HAVE_ROCKSDB=1)
