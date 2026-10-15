#include "profile.h"

#include <stdlib.h>
#include <string.h>

#if !defined(CJOSE_JWS_EXTERNAL_SIGNING_API) || CJOSE_JWS_EXTERNAL_SIGNING_API != 1
#error External-signing API version 1 is required
#endif

int LLVMFuzzerTestOneInput(const uint8_t *bytes, size_t size)
{
   static const char public_json[] = "{\"kty\":\"EC\",\"crv\":\"P-256\","
      "\"x\":\"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU\","
      "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\"}";
   if (size > qualification_token_limit + 1)
      return 0;
   cjose_err err = {0};
   cjose_jwk_t *key = cjose_jwk_import(public_json, sizeof(public_json) - 1, &err);
   if (!key)
      abort();
   qualification_verify((const char *)bytes, size, key);
   /* Exercise raw backend import too: policy rejection must not mask crashes. */
   if (size <= qualification_token_limit) {
      cjose_jws_t *jws = cjose_jws_import((const char *)bytes, size, &err);
      if (jws)
         cjose_jws_verify(jws, key, &err);
      cjose_jws_release(jws);
   }
   cjose_header_t *header = cjose_header_new(&err);
   if (header && cjose_header_set(header, "alg", "ES256", &err)) {
      size_t payload_size = size < qualification_payload_limit ? size : qualification_payload_limit;
      cjose_jws_prepared_t *state = cjose_jws_prepare(key, header, bytes, payload_size, &err);
      if (state) {
         /* Corrupt/short/long/DER-shaped signature candidates, never fake success. */
         size_t signature_size = size < 128 ? size : 128;
         cjose_jws_t *jws = cjose_jws_complete(state, bytes, signature_size, &err);
         cjose_jws_release(jws);
         cjose_jws_prepared_release(state);
      }
   }
   cjose_header_release(header);
   cjose_jwk_release(key);
   return 0;
}
