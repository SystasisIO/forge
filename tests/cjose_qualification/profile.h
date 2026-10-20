#ifndef CJOSE_QUALIFICATION_PROFILE_H
#define CJOSE_QUALIFICATION_PROFILE_H

#include <cjose/cjose.h>
#include <stddef.h>
#include <stdint.h>

enum {
   qualification_header_limit = 1024,
   qualification_payload_limit = 65536,
   qualification_token_limit = 100000,
   qualification_kid_limit = 128,
   qualification_signature_size = 64
};

/* Qualification-only policy, NOT a production parser or Forge codec. */
int qualification_verify(const char *token, size_t size, const cjose_jwk_t *key);

#endif
