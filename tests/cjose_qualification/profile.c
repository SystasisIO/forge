#include "profile.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

/* CJOSE owns decoding; re-encoding enforces canonical unpadded base64url. */
static int decode(const char *text, size_t size, size_t limit, uint8_t **out, size_t *out_size)
{
   cjose_err err = {0};
   char *canonical = NULL;
   size_t canonical_size = 0;
   int ok = 0;
   if (size > (limit * 4 + 2) / 3 || size % 4 == 1)
      return 0;
   for (size_t i = 0; i < size; ++i) {
      const unsigned char c = (unsigned char)text[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_'))
         return 0;
   }
   if (!cjose_base64url_decode(text, size, out, out_size, &err))
      return 0;
   if (*out_size <= limit && cjose_base64url_encode(*out, *out_size, &canonical, &canonical_size, &err))
      ok = canonical_size == size && memcmp(canonical, text, size) == 0;
   cjose_get_dealloc()(canonical);
   return ok;
}

int qualification_verify(const char *token, size_t size, const cjose_jwk_t *key)
{
   size_t dots[2] = {0}, count = 0;
   uint8_t *header = NULL, *payload = NULL, *signature = NULL;
   size_t header_size = 0, payload_size = 0, signature_size = 0;
   json_t *object = NULL;
   cjose_jws_t *jws = NULL;
   cjose_err err = {0};
   int ok = 0;
   if (!token || !key || size > qualification_token_limit || memchr(token, 0, size))
      return 0;
   if (cjose_jwk_get_kty(key, &err) != CJOSE_JWK_KTY_EC ||
       cjose_jwk_EC_get_curve(key, &err) != CJOSE_JWK_EC_P_256)
      return 0;
   for (size_t i = 0; i < size; ++i) {
      if (token[i] == '.') {
         if (count == 2)
            return 0;
         dots[count++] = i;
      }
   }
   if (count != 2 || dots[0] == 0 || dots[1] <= dots[0] + 1 || dots[1] + 1 >= size)
      return 0;
   if (!decode(token, dots[0], qualification_header_limit, &header, &header_size) ||
       !decode(token + dots[0] + 1, dots[1] - dots[0] - 1, qualification_payload_limit, &payload, &payload_size) ||
       !decode(token + dots[1] + 1, size - dots[1] - 1, qualification_signature_size, &signature, &signature_size) ||
       signature_size != qualification_signature_size || payload_size == 0)
      goto done;

   /* Public Jansson handles UTF-8/JSON/duplicates; no handwritten JSON parser. */
   object = json_loadb((const char *)header, header_size, JSON_REJECT_DUPLICATES, NULL);
   if (!json_is_object(object) || json_object_size(object) != 3)
      goto done;
   json_t *alg = json_object_get(object, "alg");
   json_t *typ = json_object_get(object, "typ");
   json_t *kid = json_object_get(object, "kid");
   if (!json_is_string(alg) || json_string_length(alg) != 5 || strcmp(json_string_value(alg), "ES256") ||
       !json_is_string(typ) || json_string_length(typ) != sizeof("application/ci-jws+octets") - 1 ||
       strcmp(json_string_value(typ), "application/ci-jws+octets") ||
       !json_is_string(kid) || json_string_length(kid) == 0 || json_string_length(kid) > qualification_kid_limit)
      goto done;
   for (size_t i = 0; i < json_string_length(kid); ++i) {
      unsigned char c = (unsigned char)json_string_value(kid)[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
         goto done;
   }
   jws = cjose_jws_import(token, size, &err);
   if (!jws)
      goto done;
   /* Detect any backend/parser disagreement on the protected object. */
   if (!json_equal(object, cjose_jws_get_protected(jws)))
      goto done;
   uint8_t *actual = NULL;
   size_t actual_size = 0;
   if (!cjose_jws_get_plaintext(jws, &actual, &actual_size, &err) || actual_size != payload_size ||
       memcmp(actual, payload, payload_size))
      goto done;
   ok = cjose_jws_verify(jws, key, &err);
done:
   cjose_jws_release(jws);
   json_decref(object);
   cjose_get_dealloc()(header);
   cjose_get_dealloc()(payload);
   cjose_get_dealloc()(signature);
   return ok;
}
