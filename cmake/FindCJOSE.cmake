include(FindPackageHandleStandardArgs)
include(CMakePushCheckState)

find_package(cjose 0.8.1 EXACT CONFIG QUIET COMPONENTS static)
set(_forge_cjose_extension FALSE)
if(cjose_FOUND AND TARGET cjose::cjose_static)
   list(PREPEND CMAKE_MODULE_PATH "${cjose_DIR}")
   find_package(Jansson 2.14.1 EXACT QUIET)
   list(REMOVE_AT CMAKE_MODULE_PATH 0)
   cmake_push_check_state(RESET)
   set(CMAKE_REQUIRED_LIBRARIES cjose::cjose_static)
   set(_forge_cjose_probe [=[
      #include <cjose/jws.h>
      #if !defined(CJOSE_JWS_EXTERNAL_SIGNING_API) || CJOSE_JWS_EXTERNAL_SIGNING_API != 1
      #error The qualified external signing API is required
      #endif
      int main(void) {
         cjose_jws_prepared_t *(*volatile prepare)(const cjose_jwk_t *, cjose_header_t *, const uint8_t *, size_t, cjose_err *) = cjose_jws_prepare;
         bool (*volatile input)(const cjose_jws_prepared_t *, const uint8_t **, size_t *, cjose_err *) = cjose_jws_prepared_get_signing_input;
         bool (*volatile digest)(const cjose_jws_prepared_t *, const uint8_t **, size_t *, cjose_err *) = cjose_jws_prepared_get_digest;
         cjose_jws_t *(*volatile complete)(cjose_jws_prepared_t *, const uint8_t *, size_t, cjose_err *) = cjose_jws_complete;
         void (*volatile release)(cjose_jws_prepared_t *) = cjose_jws_prepared_release;
         return !prepare || !input || !digest || !complete || !release;
      }
   ]=])
   # Do not reuse a successful probe after a caller changes dependency prefixes.
   unset(FORGE_CJOSE_HAS_EXTERNAL_SIGNING CACHE)
   if(CMAKE_C_COMPILER_LOADED)
      include(CheckCSourceCompiles)
      check_c_source_compiles("${_forge_cjose_probe}" FORGE_CJOSE_HAS_EXTERNAL_SIGNING)
   else()
      include(CheckCXXSourceCompiles)
      check_cxx_source_compiles("${_forge_cjose_probe}" FORGE_CJOSE_HAS_EXTERNAL_SIGNING)
   endif()
   cmake_pop_check_state()
   set(_forge_cjose_extension "${FORGE_CJOSE_HAS_EXTERNAL_SIGNING}")
endif()

find_package_handle_standard_args(CJOSE
   REQUIRED_VARS cjose_FOUND Jansson_FOUND _forge_cjose_extension
   VERSION_VAR cjose_VERSION
   REASON_FAILURE_MESSAGE "Forge requires the qualified CJOSE external-signing extension, not only a matching upstream version"
)
