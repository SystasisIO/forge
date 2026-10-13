find_package(PkgConfig QUIET)
find_package(CURL QUIET)

if(PkgConfig_FOUND)
   pkg_check_modules(FORGE_OAUTH2_JANSSON QUIET IMPORTED_TARGET GLOBAL jansson=2.14.1)
   pkg_check_modules(FORGE_OAUTH2_CJOSE QUIET IMPORTED_TARGET GLOBAL cjose=0.6.2.8)
   pkg_check_modules(FORGE_OAUTH2_PCRE2 QUIET IMPORTED_TARGET GLOBAL libpcre2-8>=10.42)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
   OAuth2Dependencies
   REQUIRED_VARS CURL_FOUND FORGE_OAUTH2_JANSSON_FOUND FORGE_OAUTH2_CJOSE_FOUND FORGE_OAUTH2_PCRE2_FOUND
)
