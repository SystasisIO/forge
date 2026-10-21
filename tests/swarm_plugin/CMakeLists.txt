add_executable(test_forge_swarm_plugin
   main.cpp
   config_tests.cpp
   capability_tests.cpp
   lifecycle_tests.cpp
   metadata_tests.cpp
   metadata_fixture.hxx
   ../quic_p2p/libp2p_identity_fixture.cpp
)
target_link_libraries(test_forge_swarm_plugin PRIVATE
   forge_plugins_swarm_node forge_plugins_net_p2p_node forge_plugins_crypto_secrets forge_plugins_db_store
   forge_api_p2p forge_api_transport forge_raw forge_crypto_asymmetric forge_crypto_pki
   OpenSSL::SSL Boost::included_unit_test_framework
)
add_test(NAME test_forge_swarm_plugin COMMAND test_forge_swarm_plugin)
set_tests_properties(test_forge_swarm_plugin PROPERTIES TIMEOUT 120)
target_compile_definitions(test_forge_swarm_plugin PRIVATE
   FORGE_HAS_MDBX=$<BOOL:${FORGE_HAS_MDBX}>
   FORGE_HAS_ROCKSDB=$<BOOL:${FORGE_HAS_ROCKSDB}>
)
if(FORGE_HAS_MDBX)
   target_link_libraries(test_forge_swarm_plugin PRIVATE forge_db_mdbx)
   add_test(NAME test_forge_swarm_plugin_metadata_mdbx
      COMMAND test_forge_swarm_plugin --run_test=swarm_plugin_metadata_mdbx --log_level=test_suite)
   set_tests_properties(test_forge_swarm_plugin_metadata_mdbx PROPERTIES TIMEOUT 120)
endif()
if(FORGE_HAS_ROCKSDB)
   target_link_libraries(test_forge_swarm_plugin PRIVATE forge_db_rocksdb)
   add_test(NAME test_forge_swarm_plugin_metadata_rocksdb
      COMMAND test_forge_swarm_plugin --run_test=swarm_plugin_metadata_rocksdb --log_level=test_suite)
   set_tests_properties(test_forge_swarm_plugin_metadata_rocksdb PROPERTIES TIMEOUT 120)
endif()
