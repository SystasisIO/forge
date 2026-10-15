include(FindPackageHandleStandardArgs)
include(CMakePushCheckState)

function(_forge_cjose_check source result)
   # Recheck after dependency-prefix changes, even within one configure run.
   unset(${result} CACHE)
   unset(${result})
   if(CMAKE_C_COMPILER_LOADED)
      include(CheckCSourceCompiles)
      check_c_source_compiles("${source}" ${result})
   else()
      include(CheckCXXSourceCompiles)
      check_cxx_source_compiles("${source}" ${result})
   endif()
   set(${result} "${${result}}" PARENT_SCOPE)
endfunction()

find_package(cjose 0.8.1 EXACT CONFIG QUIET COMPONENTS static)
set(_forge_cjose_dependencies FALSE)
set(_forge_cjose_extension FALSE)
unset(FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES CACHE)
unset(FORGE_CJOSE_HAS_EXTERNAL_SIGNING CACHE)
unset(FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES)
unset(FORGE_CJOSE_HAS_EXTERNAL_SIGNING)
if(cjose_FOUND AND TARGET cjose::cjose_static)
   cmake_push_check_state(RESET)
   set(CMAKE_REQUIRED_LIBRARIES cjose::cjose_static)
   # Jansson's CMake package version can describe its shared-library ABI,
   # not its release. Validate the headers and linkage actually selected.
   _forge_cjose_check([=[
      #include <jansson.h>
      #include <cjose/jws.h>
      #if JANSSON_VERSION_HEX != 0x020e01
      #error Forge requires Jansson release 2.14.1
      #endif
      int main(void) {
         json_t *value = json_object();
         json_decref(value);
         cjose_header_t *header = cjose_header_new(NULL);
         cjose_header_release(header);
         return 0;
      }
   ]=] FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES)
   set(_forge_cjose_dependencies "${FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES}")
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
   if(_forge_cjose_dependencies)
      _forge_cjose_check("${_forge_cjose_probe}" FORGE_CJOSE_HAS_EXTERNAL_SIGNING)
      set(_forge_cjose_extension "${FORGE_CJOSE_HAS_EXTERNAL_SIGNING}")
   endif()
   cmake_pop_check_state()
endif()

find_package_handle_standard_args(CJOSE
   REQUIRED_VARS cjose_FOUND _forge_cjose_dependencies _forge_cjose_extension
   VERSION_VAR cjose_VERSION
   REASON_FAILURE_MESSAGE "Forge requires Jansson 2.14.1 and the qualified CJOSE external-signing extension, not only a matching upstream version"
)
