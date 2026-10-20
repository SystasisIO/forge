find_package(PkgConfig QUIET)
find_package(CURL QUIET)

if(PkgConfig_FOUND)
   pkg_check_modules(FORGE_OAUTH2_PCRE2 QUIET IMPORTED_TARGET GLOBAL libpcre2-8>=10.42)
endif()

# In-tree builds provide the pinned backend before discovering the remaining
# OAuth dependencies. Installed consumers must prove the extension is present.
if(NOT _forge_cjose_source_ready)
   find_package(CJOSE QUIET)
endif()
set(_forge_oauth2_cjose_found FALSE)
if((_forge_cjose_source_ready OR CJOSE_FOUND) AND
   TARGET cjose::cjose_static AND TARGET Jansson::Jansson)
   set(_forge_oauth2_cjose_found TRUE)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
   OAuth2Dependencies
   REQUIRED_VARS CURL_FOUND _forge_oauth2_cjose_found FORGE_OAUTH2_PCRE2_FOUND
)
