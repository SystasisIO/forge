add_executable(test_forge_net_s3 client.cpp)
target_link_libraries(test_forge_net_s3 PRIVATE forge_net_s3 Boost::asio Boost::beast)
add_test(NAME test_forge_net_s3 COMMAND test_forge_net_s3)
set_tests_properties(test_forge_net_s3 PROPERTIES TIMEOUT 90)
