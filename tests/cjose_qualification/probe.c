#include "profile.h"

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(CJOSE_JWS_EXTERNAL_SIGNING_API) || CJOSE_JWS_EXTERNAL_SIGNING_API != 1
#error Installed CJOSE must provide external-signing public API version 1
#endif

static cjose_jws_prepared_t *states[16];
static size_t allocation_index, fail_at;
static int tracking, failure_hit;

static int fail_allocation(void)
{
   if (!tracking)
      return 0;
   ++allocation_index;
   if (fail_at && allocation_index == fail_at) {
      failure_hit = 1;
      return 1;
   }
   return 0;
}

static void *allocate(size_t size) { return fail_allocation() ? NULL : malloc(size); }
static void *reallocate(void *ptr, size_t size) { return fail_allocation() ? NULL : realloc(ptr, size); }

static void arm(json_t *request)
{
   allocation_index = 0;
   failure_hit = 0;
   fail_at = (size_t)json_integer_value(json_object_get(request, "fail_at"));
   tracking = 1;
}

static void unhex(const char *text, uint8_t **bytes, size_t *size)
{
   *bytes = NULL;
   *size = 0;
   if (!text || strlen(text) % 2)
      return;
   *size = strlen(text) / 2;
   *bytes = malloc(*size + 1);
   if (!*bytes)
      return;
   for (size_t i = 0; i < *size; ++i) {
      unsigned int byte;
      if (sscanf(text + i * 2, "%2x", &byte) != 1) {
         free(*bytes);
         *bytes = NULL;
         return;
      }
      (*bytes)[i] = (uint8_t)byte;
   }
   (*bytes)[*size] = 0;
}

static void data(json_t *response, const char *name, const uint8_t *bytes, size_t size)
{
   static const char hex[] = "0123456789abcdef";
   char *text = malloc(size * 2 + 1);
   if (!text)
      exit(3);
   for (size_t i = 0; i < size; ++i) {
      text[i * 2] = hex[bytes[i] >> 4];
      text[i * 2 + 1] = hex[bytes[i] & 15];
   }
   text[size * 2] = 0;
   json_object_set_new(response, name, json_stringn(text, size * 2));
   free(text);
}

static cjose_jwk_t *public_key(json_t *object)
{
   if (!json_is_object(object))
      return NULL;
   const char *name;
   json_t *value;
   json_object_foreach(object, name, value) {
      if (strcmp(name, "kty") && strcmp(name, "crv") && strcmp(name, "x") &&
          strcmp(name, "y") && strcmp(name, "kid"))
         return NULL;
      if (!json_is_string(value))
         return NULL;
   }
   cjose_err err = {0};
   /* No private JWK member ever reaches the CJOSE importer. */
   return cjose_jwk_import_json(object, &err);
}

static void export_result(json_t *response, cjose_jws_t *jws, cjose_err *err)
{
   if (!jws)
      return;
   const char *compact = NULL;
   if (!cjose_jws_export(jws, &compact, err))
      exit(4);
   data(response, "token", (const uint8_t *)compact, strlen(compact));
}

struct parallel_call {
   cjose_jws_prepared_t *state;
   uint8_t *signature;
   size_t size;
   cjose_jws_t *result;
};

static void *complete_thread(void *arg)
{
   struct parallel_call *call = arg;
   cjose_err err = {0};
   call->result = cjose_jws_complete(call->state, call->signature, call->size, &err);
   return NULL;
}

static json_t *dispatch(json_t *request)
{
   const char *op = json_string_value(json_object_get(request, "op"));
   int slot = (int)json_integer_value(json_object_get(request, "slot"));
   json_t *response = json_object();
   cjose_err err = {0};
   int ok = 0;
   if (!op || slot < 0 || slot >= 16)
      exit(2);
   if (!strcmp(op, "prepare")) {
      cjose_jwk_t *key = public_key(json_object_get(request, "key"));
      json_t *header = json_deep_copy(json_object_get(request, "header"));
      uint8_t *payload = NULL;
      size_t size = 0;
      unhex(json_string_value(json_object_get(request, "payload")), &payload, &size);
      if (states[slot] || !key || !header || !payload)
         exit(2);
      arm(request);
      states[slot] = cjose_jws_prepare(key, header, payload, size, &err);
      tracking = 0;
      ok = states[slot] != NULL;
      /* Mutate and destroy ALL caller-owned inputs after prepare. */
      memset(payload, 0xA5, size);
      json_object_set_new(header, "alg", json_string("HS256"));
      cjose_jwk_set_kid(key, "mutated", 7, NULL);
      json_decref(header);
      cjose_jwk_release(key);
      free(payload);
   } else if (!strcmp(op, "view")) {
      const uint8_t *input = NULL, *digest = NULL;
      size_t input_size = 0, digest_size = 0;
      ok = cjose_jws_prepared_get_signing_input(states[slot], &input, &input_size, &err) &&
           cjose_jws_prepared_get_digest(states[slot], &digest, &digest_size, &err);
      if (ok) {
         data(response, "input", input, input_size);
         data(response, "digest", digest, digest_size);
      }
   } else if (!strcmp(op, "complete")) {
      uint8_t *signature = NULL;
      size_t size = 0;
      unhex(json_string_value(json_object_get(request, "signature")), &signature, &size);
      if (!signature)
         exit(2);
      arm(request);
      cjose_jws_t *jws = cjose_jws_complete(states[slot], signature, size, &err);
      tracking = 0;
      ok = jws != NULL;
      memset(signature, 0, size);
      free(signature);
      /* JWS and prepared state must have independent lifetimes. */
      if (json_is_true(json_object_get(request, "release_first"))) {
         cjose_jws_prepared_release(states[slot]);
         states[slot] = NULL;
      }
      export_result(response, jws, &err);
      cjose_jws_release(jws);
   } else if (!strcmp(op, "verify") || !strcmp(op, "verify_backend")) {
      cjose_jwk_t *key = public_key(json_object_get(request, "key"));
      uint8_t *token = NULL;
      size_t size = 0;
      unhex(json_string_value(json_object_get(request, "token")), &token, &size);
      if (!key || !token)
         exit(2);
      arm(request);
      if (!strcmp(op, "verify")) {
         ok = qualification_verify((const char *)token, size, key);
      } else {
         cjose_jws_t *jws = cjose_jws_import((const char *)token, size, &err);
         ok = jws && cjose_jws_verify(jws, key, &err);
         cjose_jws_release(jws);
      }
      tracking = 0;
      cjose_jwk_release(key);
      free(token);
   } else if (!strcmp(op, "parallel")) {
      pthread_t threads[8];
      struct parallel_call calls[8] = {0};
      json_t *signatures = json_object_get(request, "signatures");
      if (json_array_size(signatures) != 8)
         exit(2);
      for (int i = 0; i < 8; ++i) {
         calls[i].state = states[i];
         unhex(json_string_value(json_array_get(signatures, i)), &calls[i].signature, &calls[i].size);
         if (!calls[i].signature || pthread_create(&threads[i], NULL, complete_thread, &calls[i]))
            exit(2);
      }
      json_t *tokens = json_array();
      ok = 1;
      for (int i = 0; i < 8; ++i) {
         if (pthread_join(threads[i], NULL))
            exit(2);
         json_t *item = json_object();
         ok = ok && calls[i].result != NULL;
         export_result(item, calls[i].result, &err);
         json_array_append_new(tokens, item);
         cjose_jws_release(calls[i].result);
         free(calls[i].signature);
      }
      json_object_set_new(response, "tokens", tokens);
   } else if (!strcmp(op, "release")) {
      cjose_jws_prepared_release(states[slot]);
      states[slot] = NULL;
      ok = 1;
   } else if (!strcmp(op, "null_arguments")) {
      const uint8_t *sentinel = (const uint8_t *)"sentinel";
      const uint8_t *view = sentinel;
      size_t size = 17;
      ok = !cjose_jws_prepared_get_signing_input(NULL, &view, &size, &err) && size == 17 &&
           view == sentinel &&
           !cjose_jws_prepared_get_digest(NULL, &view, &size, &err) && size == 17 &&
           !cjose_jws_complete(NULL, NULL, 0, &err);
      cjose_jws_prepared_release(NULL);
   } else {
      exit(2);
   }
   json_object_set_new(response, "ok", json_boolean(ok));
   json_object_set_new(response, "error", json_integer(err.code));
   json_object_set_new(response, "allocations", json_integer((json_int_t)allocation_index));
   json_object_set_new(response, "failure_hit", json_boolean(failure_hit));
   return response;
}

int main(void)
{
   /* Install before any Jansson/OpenSSL object exists, never swap allocators. */
   cjose_set_alloc_funcs(allocate, reallocate, free);
   char *line = malloc(400000);
   if (!line)
      return 3;
   while (fgets(line, 400000, stdin)) {
      if (!strchr(line, '\n'))
         return 2;
      json_t *request = json_loads(line, JSON_REJECT_DUPLICATES, NULL);
      if (!request)
         return 2;
      json_t *response = dispatch(request);
      json_dumpf(response, stdout, JSON_COMPACT);
      putchar('\n');
      fflush(stdout);
      json_decref(response);
      json_decref(request);
   }
   for (int i = 0; i < 16; ++i)
      cjose_jws_prepared_release(states[i]);
   free(line);
   return ferror(stdin) ? 2 : 0;
}
