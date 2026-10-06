#include "details/tls_handles.hxx"

namespace forge::net::quic::detail {
void tls_handles::context_deleter::operator()(SSL_CTX* ctx) const noexcept {
   SSL_CTX_free(ctx);
}
void tls_handles::session_deleter::operator()(SSL* ssl) const noexcept {
   if (ssl != nullptr) {
      SSL_set_app_data(ssl, nullptr);
   }
   SSL_free(ssl);
}
void tls_handles::certificate_deleter::operator()(X509* value) const noexcept {
   X509_free(value);
}
void tls_handles::key_deleter::operator()(EVP_PKEY* value) const noexcept {
   EVP_PKEY_free(value);
}

} // namespace forge::net::quic::detail
