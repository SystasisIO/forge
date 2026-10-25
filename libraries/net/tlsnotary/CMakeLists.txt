find_package(Boost 1.90 REQUIRED CONFIG COMPONENTS process)
find_program(FORGE_TLSNOTARY_CARGO cargo REQUIRED)
set(FORGE_TLSNOTARY_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-target" CACHE PATH "Private TLSNotary Cargo output directory")
set(_forge_tlsnotary_worker "${FORGE_TLSNOTARY_CARGO_TARGET_DIR}/release/forge-tlsnotary-worker")
file(GLOB _forge_tlsnotary_rust_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/native/src/*.rs")
add_custom_command(
   OUTPUT "${_forge_tlsnotary_worker}"
   COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${FORGE_TLSNOTARY_CARGO_TARGET_DIR}"
      ${FORGE_TLSNOTARY_CARGO} build --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/native/Cargo.toml" --locked --release -j 4
   DEPENDS native/Cargo.toml native/Cargo.lock ${_forge_tlsnotary_rust_sources}
   VERBATIM
)
add_custom_target(forge_net_tlsnotary_native DEPENDS "${_forge_tlsnotary_worker}")
add_library(forge_net_tlsnotary STATIC engine.cpp engine_impl.cpp native_exchange.cpp wire.cpp)
forge_target_modules_at(forge_net_tlsnotary net/tlsnotary)
target_include_directories(forge_net_tlsnotary PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS}
   "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(forge_net_tlsnotary PUBLIC forge_asio forge_net_transport forge_exceptions forge_crypto_digest forge_crypto_core Boost::asio PRIVATE Boost::process forge_log)
add_dependencies(forge_net_tlsnotary forge_net_tlsnotary_native)
set(FORGE_TLSNOTARY_WORKER "${_forge_tlsnotary_worker}" CACHE INTERNAL "TLSNotary build worker")
install(PROGRAMS "${_forge_tlsnotary_worker}" DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/forge" COMPONENT dev EXCLUDE_FROM_ALL)
