#if defined(__linux__)
#define _GNU_SOURCE 1
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include <cjose/cjose.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A macro here would silently replace the data-symbol ABI we must exercise. */
#if defined(CJOSE_HDR_ALG) || defined(CJOSE_HDR_ALG_ES256)
#error Compile this consumer only against historical 0.6.2.8 public headers
#endif

struct legacy_constant {
   const char **address;
   const char *expected;
};

static const struct legacy_constant constants[] = {
   {&CJOSE_HDR_ALG, "alg"}, {&CJOSE_HDR_ENC, "enc"},
   {&CJOSE_HDR_CTY, "cty"}, {&CJOSE_HDR_KID, "kid"},
   {&CJOSE_HDR_EPK, "epk"}, {&CJOSE_HDR_APU, "apu"}, {&CJOSE_HDR_APV, "apv"},
   {&CJOSE_HDR_ALG_NONE, "none"}, {&CJOSE_HDR_ALG_ECDH_ES, "ECDH-ES"},
   {&CJOSE_HDR_ALG_RSA_OAEP, "RSA-OAEP"}, {&CJOSE_HDR_ALG_RSA1_5, "RSA1_5"},
   {&CJOSE_HDR_ALG_A128KW, "A128KW"}, {&CJOSE_HDR_ALG_A192KW, "A192KW"},
   {&CJOSE_HDR_ALG_A256KW, "A256KW"},
   {&CJOSE_HDR_ALG_PS256, "PS256"}, {&CJOSE_HDR_ALG_PS384, "PS384"}, {&CJOSE_HDR_ALG_PS512, "PS512"},
   {&CJOSE_HDR_ALG_RS256, "RS256"}, {&CJOSE_HDR_ALG_RS384, "RS384"}, {&CJOSE_HDR_ALG_RS512, "RS512"},
   {&CJOSE_HDR_ALG_HS256, "HS256"}, {&CJOSE_HDR_ALG_HS384, "HS384"}, {&CJOSE_HDR_ALG_HS512, "HS512"},
   {&CJOSE_HDR_ALG_ES256, "ES256"}, {&CJOSE_HDR_ALG_ES384, "ES384"}, {&CJOSE_HDR_ALG_ES512, "ES512"},
   {&CJOSE_HDR_ALG_DIR, "dir"},
   {&CJOSE_HDR_ENC_A128GCM, "A128GCM"}, {&CJOSE_HDR_ENC_A192GCM, "A192GCM"}, {&CJOSE_HDR_ENC_A256GCM, "A256GCM"},
   {&CJOSE_HDR_ENC_A128CBC_HS256, "A128CBC-HS256"},
   {&CJOSE_HDR_ENC_A192CBC_HS384, "A192CBC-HS384"},
   {&CJOSE_HDR_ENC_A256CBC_HS512, "A256CBC-HS512"}
};

static int loaded_from(const void *symbol, const char *expected)
{
   Dl_info info;
   char resolved[PATH_MAX];
   return dladdr(symbol, &info) != 0 && info.dli_fname &&
          realpath(info.dli_fname, resolved) && strcmp(resolved, expected) == 0;
}

int main(int argc, char **argv)
{
   /* Public RFC 7515 A.3 vector only; no private key enters this consumer. */
   static const char key_json[] = "{\"kty\":\"EC\",\"crv\":\"P-256\","
      "\"x\":\"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU\","
      "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\"}";
   static const char compact[] = "eyJhbGciOiJFUzI1NiJ9."
      "eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
      "cGxlLmNvbS9pc19yb290Ijp0cnVlfQ."
      "DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8ISlSApmWQxfKTUJqPP3-Kg6NU1Q";
   cjose_err err = {0};
   cjose_jwk_t *key = NULL;
   cjose_jws_t *jws = NULL;
   cjose_header_t *header = NULL;
   char expected_library[PATH_MAX];
   const char *stage = "arguments";
   int result = 1;
   if (argc != 3 || !realpath(argv[1], expected_library) ||
       (strcmp(argv[2], "0.6.2.8") && strcmp(argv[2], "0.8.1")))
      goto done;
   stage = "historical_headers";
   if (strcmp(CJOSE_VERSION, "0.6.2.8"))
      goto done;
   stage = "loaded_library";
   if (!loaded_from((const void *)cjose_jws_import, expected_library) ||
       !loaded_from((const void *)cjose_version, expected_library))
      goto done;
   stage = "runtime_version";
   if (strcmp(cjose_version(), argv[2]))
      goto done;
   stage = "legacy_constants";
   for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); ++i) {
      if (!*constants[i].address || strcmp(*constants[i].address, constants[i].expected) ||
          !loaded_from(constants[i].address, expected_library))
         goto done;
   }
   stage = "legacy_header_api";
   header = cjose_header_new(&err);
   if (!header || !cjose_header_set(header, CJOSE_HDR_ALG, CJOSE_HDR_ALG_ES256, &err))
      goto done;
   const char *algorithm = cjose_header_get(header, CJOSE_HDR_ALG, &err);
   if (!algorithm || strcmp(algorithm, CJOSE_HDR_ALG_ES256))
      goto done;
   stage = "rfc_import_verify_export";
   key = cjose_jwk_import(key_json, sizeof(key_json) - 1, &err);
   jws = cjose_jws_import(compact, sizeof(compact) - 1, &err);
   const char *exported = NULL;
   if (!key || !jws || !cjose_jws_verify(jws, key, &err) ||
       !cjose_jws_export(jws, &exported, &err) || !exported || strcmp(exported, compact))
      goto done;
   result = 0;
done:
   cjose_header_release(header);
   cjose_jws_release(jws);
   if (key)
      cjose_jwk_release(key);
   if (result == 0) {
      printf("{\"status\":\"PASS\",\"header_version\":\"0.6.2.8\",\"runtime_version\":\"%s\","
             "\"loaded_expected_library\":true,\"legacy_constants_checked\":%zu,"
             "\"rfc_import_verify_export\":true}\n", argv[2], sizeof(constants) / sizeof(constants[0]));
   } else {
      /* Fixed stage names only, never path, token, key or backend error text. */
      printf("{\"status\":\"FAILED\",\"stage\":\"%s\"}\n", stage);
   }
   return result;
}
