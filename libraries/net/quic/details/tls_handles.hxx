#pragma once

#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace forge::net::quic::detail {
struct tls_handles {
   struct context_deleter {
      void operator()(SSL_CTX* ctx) const noexcept;
   };
   struct session_deleter {
      void operator()(SSL* ssl) const noexcept;
   };
   struct certificate_deleter {
      void operator()(X509* value) const noexcept;
   };
   struct key_deleter {
      void operator()(EVP_PKEY* value) const noexcept;
   };
};

} // namespace forge::net::quic::detail
