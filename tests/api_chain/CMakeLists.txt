add_executable(test_forge_api_chain_counter
   counter_tests.cpp
   counter_client.cpp
   counter_http.cpp
   counter_signer.cpp
   counter_store.cpp
   counter_staged.cpp
   counter_node.cpp
   counter_delivery.cpp
   counter_read.cpp
   ../package_chain_api_component/read_fixture.cpp
   counter_peer.cpp
   ../package_chain_api_component/p2p_runtime.cpp
   ../package_chain_api_component/test_support.cpp)
target_sources(test_forge_api_chain_counter PRIVATE FILE_SET CXX_MODULES
   BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
   FILES
      counter.cppm
      counter_client.cppm
      counter_http.cppm
      counter_read.cppm
      ../package_chain_api_component/read_fixture.cppm
      counter_peer.cppm
      ../package_chain_api_component/p2p_runtime.cppm
      ../package_chain_api_component/test_support.cppm)
target_link_libraries(test_forge_api_chain_counter PRIVATE
   forge_plugins_chain_outbox forge_api_chain forge_chain_api forge_chain_transaction
   forge_plugins_db_store forge_db_mdbx forge_plugins_net_p2p_node forge_plugins_net_p2p_resolver
   forge_plugins_crypto_secrets forge_api_http forge_net_http forge_api_p2p forge_config_core forge_app forge_asio)
add_test(NAME test_forge_api_chain_counter COMMAND test_forge_api_chain_counter p2p)
add_test(NAME test_forge_api_chain_counter_http COMMAND test_forge_api_chain_counter http)
set_tests_properties(test_forge_api_chain_counter test_forge_api_chain_counter_http PROPERTIES TIMEOUT 90)
