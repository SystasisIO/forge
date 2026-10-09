find_package(AWSSDK 1.11.900 EXACT CONFIG REQUIRED COMPONENTS s3)

add_library(forge_net_s3 STATIC client.cpp client_impl.cpp)
forge_target_modules_at(forge_net_s3 net/s3)

target_include_directories(forge_net_s3 PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_include_directories(forge_net_s3 SYSTEM BEFORE PRIVATE ${AWSSDK_INCLUDE_DIRS})
target_link_libraries(forge_net_s3 PUBLIC forge_asio forge_exceptions forge_crypto_core)
target_link_libraries(forge_net_s3 PRIVATE ${AWSSDK_LINK_LIBRARIES})
