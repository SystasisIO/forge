#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "crypto_stream_salsa20.h"
#include "crypto_stream_xsalsa20.h"
#include "ref/salsa20_ref.h"

/*
 * The selected upstream sources scrub temporary subkeys through this symbol.
 * The complete libsodium utility layer is intentionally outside this private
 * XSalsa20 subset, so retain its documented volatile-write fallback here.
 */
void
sodium_memzero(void *const pnt, const size_t len)
{
   volatile unsigned char *volatile pnt_ = (volatile unsigned char *volatile) pnt;
   size_t i = 0U;

   while (i < len) {
      pnt_[i++] = 0U;
   }
}

/* The upstream key-generation entry point is not linked into Forge's API. */
void
randombytes_buf(void *const buf, const size_t size)
{
   (void) buf;
   (void) size;
   abort();
}

int
crypto_stream_salsa20_xor_ic(unsigned char *c, const unsigned char *m,
                             unsigned long long mlen, const unsigned char *n,
                             uint64_t ic, const unsigned char *k)
{
   return crypto_stream_salsa20_ref_implementation.stream_xor_ic(c, m, mlen, n, ic, k);
}

int
crypto_stream_salsa20(unsigned char *c, unsigned long long clen,
                      const unsigned char *n, const unsigned char *k)
{
   return crypto_stream_salsa20_ref_implementation.stream(c, clen, n, k);
}

int
forge_xsalsa20_vendor_xor_ic(unsigned char *c, const unsigned char *m,
                             unsigned long long mlen, const unsigned char *n,
                             uint64_t ic, const unsigned char *k)
{
   return crypto_stream_xsalsa20_xor_ic(c, m, mlen, n, ic, k);
}

int
forge_salsa20_vendor_keystream(unsigned char *c, unsigned long long clen,
                               const unsigned char *n, const unsigned char *k)
{
   return crypto_stream_salsa20(c, clen, n, k);
}
