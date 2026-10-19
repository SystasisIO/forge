include(ForgeCJOSE)
find_package(OAuth2Dependencies REQUIRED)

get_filename_component(_forge_oauth2_source "${CMAKE_CURRENT_LIST_DIR}/../vendor/liboauth2" ABSOLUTE)
set(_forge_oauth2_generated "${PROJECT_BINARY_DIR}/vendor/liboauth2/include")
set(PACKAGE_NAME liboauth2)
set(PACKAGE_VERSION 2.4.0)
configure_file(
   "${_forge_oauth2_source}/include/oauth2/version.h.in"
   "${_forge_oauth2_generated}/oauth2/version.h"
   @ONLY
)
unset(PACKAGE_NAME)
unset(PACKAGE_VERSION)

# Match the upstream host library without its optional web-server/cache adapters.
set(_forge_oauth2_sources
   src/version.c
   src/mem.c
   src/log.c
   src/util.c
   src/cfg/cfg.c
   src/cfg/auth.c
   src/cfg/source.c
   src/cfg/cache_cfg.c
   src/cfg/verify.c
   src/cfg/target.c
   src/cfg/proto_cfg.c
   src/cfg/openidc_cfg.c
   src/cfg/session_cfg.c
   src/ipc.c
   src/cache.c
   src/cache/shm.c
   src/cache/file.c
   src/jose.c
   src/http.c
   src/proto.c
   src/oauth2.c
   src/dpop.c
   src/session.c
   src/openidc/provider.c
   src/openidc/resolver.c
   src/openidc/client.c
   src/openidc/state.c
   src/openidc/openidc.c
)
list(TRANSFORM _forge_oauth2_sources PREPEND "${_forge_oauth2_source}/")
add_library(oauth2-internal STATIC ${_forge_oauth2_sources})
set_target_properties(oauth2-internal PROPERTIES
   EXPORT_NAME oauth2_internal
   POSITION_INDEPENDENT_CODE ON
)
target_include_directories(oauth2-internal SYSTEM PUBLIC
   "$<BUILD_INTERFACE:${_forge_oauth2_source}/include>"
   "$<BUILD_INTERFACE:${_forge_oauth2_generated}>"
)
target_include_directories(oauth2-internal PRIVATE "${_forge_oauth2_source}/src")
target_link_libraries(oauth2-internal PUBLIC
   CURL::libcurl
   OpenSSL::SSL
   OpenSSL::Crypto
   Jansson::Jansson
   "$<BUILD_LOCAL_INTERFACE:cjose::cjose_static>"
   PkgConfig::FORGE_OAUTH2_PCRE2
   Threads::Threads
)
forge_apply_vendored_implementation_policy(oauth2-internal)
