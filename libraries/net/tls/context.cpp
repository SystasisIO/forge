module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

module forge.net.tls.context;

import forge.net.tls.exceptions;
import forge.crypto.pki.x509;

namespace forge::net::tls {

struct context_snapshot::impl {
   explicit impl(boost::asio::ssl::context::method method, peer_verification verification_value)
       : native(method), verification_value(verification_value) {}

   boost::asio::ssl::context native;
   std::vector<std::string> alpn_protocols;
   std::vector<unsigned char> client_alpn_wire;
   peer_verification verification_value;
};

namespace {

namespace asio = boost::asio;

[[noreturn]] void throw_identity_invalid(std::string message, std::string_view reason) {
   FORGE_THROW_EXCEPTION(exceptions::identity_invalid, std::move(message), forge::exceptions::ctx("reason", reason));
}

[[noreturn]] void throw_trust_anchors_invalid(std::string message, std::string_view reason) {
   FORGE_THROW_EXCEPTION(exceptions::trust_anchors_invalid, std::move(message),
                         forge::exceptions::ctx("reason", reason));
}

struct x509_deleter {
   void operator()(X509* value) const noexcept {
      X509_free(value);
   }
};

struct bio_deleter {
   void operator()(BIO* value) const noexcept {
      BIO_free(value);
   }
};

using x509_ptr = std::unique_ptr<X509, x509_deleter>;
using bio_ptr = std::unique_ptr<BIO, bio_deleter>;

void erase_tls_text(std::string& value) noexcept {
   if (!value.empty()) {
      OPENSSL_cleanse(value.data(), value.size());
      value.clear();
   }
}

void erase_context_options(context_options& options) noexcept {
   erase_tls_text(options.certificate_chain_pem);
   erase_tls_text(options.private_key_pem);
   for (auto& authority : options.trust_anchors_pem) {
      erase_tls_text(authority);
   }
   options.trust_anchors_pem.clear();
}

class context_options_eraser {
 public:
   explicit context_options_eraser(context_options& options) : options_(options) {}

   ~context_options_eraser() {
      erase_context_options(options_);
   }

   context_options_eraser(const context_options_eraser&) = delete;
   context_options_eraser& operator=(const context_options_eraser&) = delete;

 private:
   context_options& options_;
};

void validate_role(endpoint_role value) {
   switch (value) {
   case endpoint_role::client:
   case endpoint_role::server:
      return;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unknown TLS endpoint role");
}

void validate_protocol_policy(protocol_policy value) {
   switch (value) {
   case protocol_policy::tls13_only:
   case protocol_policy::system_default:
      return;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unknown TLS protocol policy");
}

void validate_peer_verification(peer_verification value) {
   switch (value) {
   case peer_verification::none:
   case peer_verification::verify_peer:
   case peer_verification::require_peer_certificate:
   case peer_verification::require_peer_certificate_for_application_verification:
      return;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unknown TLS peer-verification mode");
}

[[nodiscard]] bool verifies_peer_chain(peer_verification verification) noexcept {
   return verification == peer_verification::verify_peer || verification == peer_verification::require_peer_certificate;
}

[[nodiscard]] bool requires_peer_certificate(peer_verification verification) noexcept {
   return verification == peer_verification::require_peer_certificate ||
          verification == peer_verification::require_peer_certificate_for_application_verification;
}

[[nodiscard]] std::vector<unsigned char> encode_alpn(const std::vector<std::string>& protocols) {
   if (protocols.size() > max_alpn_protocols) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS ALPN protocol count exceeds its configured bound");
   }

   auto out = std::vector<unsigned char>{};
   for (const auto& protocol : protocols) {
      if (protocol.empty() || protocol.size() > 255U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS ALPN protocol length must be 1..255 bytes");
      }
      out.push_back(static_cast<unsigned char>(protocol.size()));
      out.insert(out.end(), protocol.begin(), protocol.end());
   }
   return out;
}

int select_alpn(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
                void* arg) {
   const auto* supported = static_cast<const std::vector<std::string>*>(arg);
   auto offset = unsigned{0};
   while (offset < inlen) {
      const auto length = static_cast<unsigned>(in[offset]);
      ++offset;
      if (length == 0 || offset + length > inlen) {
         return SSL_TLSEXT_ERR_NOACK;
      }
      const auto value = std::string_view{reinterpret_cast<const char*>(in + offset), length};
      if (std::find(supported->begin(), supported->end(), value) != supported->end()) {
         *out = in + offset;
         *outlen = static_cast<unsigned char>(length);
         return SSL_TLSEXT_ERR_OK;
      }
      offset += length;
   }
   return SSL_TLSEXT_ERR_NOACK;
}

void validate_options(const context_options& options) {
   validate_role(options.role);
   validate_protocol_policy(options.protocols);
   validate_peer_verification(options.verification);

   if (options.certificate_chain_pem.empty() != options.private_key_pem.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "TLS certificate chain and private key must be configured together");
   }
   if (options.role == endpoint_role::server && options.certificate_chain_pem.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS server requires a certificate chain and private key");
   }
   if (options.certificate_chain_pem.size() > max_certificate_chain_pem_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS certificate chain exceeds its configured bound");
   }
   if (options.private_key_pem.size() > max_private_key_pem_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS private key exceeds its configured bound");
   }
   if (options.trust_anchors_pem.size() > max_trust_anchors) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS trust-anchor count exceeds its configured bound");
   }
   auto total_trust_anchor_bytes = std::size_t{};
   for (const auto& authority : options.trust_anchors_pem) {
      if (authority.size() > max_trust_anchor_pem_bytes || authority.size() > max_trust_anchor_pem_total_bytes ||
          total_trust_anchor_bytes > max_trust_anchor_pem_total_bytes - authority.size()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS trust-anchor material exceeds its configured bound");
      }
      total_trust_anchor_bytes += authority.size();
   }
   if (requires_peer_certificate(options.verification)) {
      if (options.role != endpoint_role::server) {
         FORGE_THROW_EXCEPTION(exceptions::verification_configuration_invalid,
                               "TLS client cannot require a peer certificate");
      }
   }
   if (options.verification == peer_verification::require_peer_certificate) {
      if (options.trust_anchors_pem.empty() && !options.use_default_verify_paths) {
         FORGE_THROW_EXCEPTION(exceptions::verification_configuration_invalid,
                               "mTLS requires trust anchors or default verify paths");
      }
   }
   if (options.verification == peer_verification::verify_peer && options.trust_anchors_pem.empty() &&
       !options.use_default_verify_paths) {
      FORGE_THROW_EXCEPTION(exceptions::verification_configuration_invalid,
                            "TLS peer verification requires trust anchors or default verify paths");
   }
}

void configure_protocols(asio::ssl::context& context, protocol_policy policy) {
   if (policy == protocol_policy::system_default) {
      return;
   }
   if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_3_VERSION) != 1 ||
       SSL_CTX_set_max_proto_version(context.native_handle(), TLS1_3_VERSION) != 1) {
      FORGE_THROW_EXCEPTION(exceptions::context_creation_failed, "failed to configure TLS 1.3 only context");
   }
}

void load_identity(asio::ssl::context& context, const context_options& options) {
   if (options.certificate_chain_pem.empty()) {
      return;
   }
   try {
      context.use_certificate_chain(
          asio::buffer(options.certificate_chain_pem.data(), options.certificate_chain_pem.size()));
      context.use_private_key(asio::buffer(options.private_key_pem.data(), options.private_key_pem.size()),
                              asio::ssl::context::pem);
   } catch (const boost::system::system_error& error) {
      throw_identity_invalid("failed to load TLS certificate chain or private key", error.code().message());
   }
   if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
      throw_identity_invalid("TLS certificate chain does not match private key", "OpenSSL rejected the key pair");
   }
}

[[nodiscard]] bool contains_only_whitespace(std::string_view value) {
   return std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isspace(character) != 0; });
}

[[nodiscard]] std::vector<x509_ptr> parse_trust_anchor_bundle(std::string_view authority) {
   auto source = bio_ptr{BIO_new_mem_buf(authority.data(), static_cast<int>(authority.size()))};
   if (!source) {
      FORGE_THROW_EXCEPTION(exceptions::context_creation_failed, "failed to allocate TLS trust-anchor parser");
   }

   auto certificates = std::vector<x509_ptr>{};
   for (;;) {
      const auto offset = BIO_tell(source.get());
      ERR_clear_error();
      auto certificate = x509_ptr{PEM_read_bio_X509(source.get(), nullptr, nullptr, nullptr)};
      if (certificate) {
         certificates.push_back(std::move(certificate));
         continue;
      }
      if (offset < 0 || !contains_only_whitespace(authority.substr(static_cast<std::size_t>(offset)))) {
         FORGE_THROW_EXCEPTION(exceptions::trust_anchors_invalid, "TLS trust-anchor bundle contains malformed PEM");
      }
      ERR_clear_error();
      break;
   }
   if (certificates.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::trust_anchors_invalid, "TLS trust-anchor bundle contains no certificates");
   }
   return certificates;
}

void load_trust_anchors(asio::ssl::context& context, const context_options& options) {
   auto* store = SSL_CTX_get_cert_store(context.native_handle());
   if (store == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::context_creation_failed, "TLS context has no certificate trust store");
   }

   const auto advertise_client_authorities =
       options.role == endpoint_role::server && options.verification == peer_verification::require_peer_certificate;
   for (const auto& authority : options.trust_anchors_pem) {
      if (authority.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::trust_anchors_invalid, "TLS trust anchor must not be empty");
      }
      for (const auto& certificate : parse_trust_anchor_bundle(authority)) {
         if (X509_STORE_add_cert(store, certificate.get()) != 1) {
            throw_trust_anchors_invalid("failed to load TLS trust anchor", "OpenSSL rejected the certificate");
         }
         if (advertise_client_authorities && SSL_CTX_add_client_CA(context.native_handle(), certificate.get()) != 1) {
            FORGE_THROW_EXCEPTION(exceptions::context_creation_failed,
                                  "failed to advertise TLS client certificate authority");
         }
      }
   }

   if (!verifies_peer_chain(options.verification) || !options.trust_anchors_pem.empty()) {
      return;
   }

   auto error = boost::system::error_code{};
   context.set_default_verify_paths(error);
   if (error) {
      throw_trust_anchors_invalid("failed to load default TLS verify paths", error.message());
   }
}

void configure_verification(asio::ssl::context& context, peer_verification verification) {
   auto mode = asio::ssl::verify_none;
   switch (verification) {
   case peer_verification::none:
      break;
   case peer_verification::verify_peer:
      mode = asio::ssl::verify_peer;
      break;
   case peer_verification::require_peer_certificate:
      mode = asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert;
      break;
   case peer_verification::require_peer_certificate_for_application_verification:
      SSL_CTX_set_verify(context.native_handle(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                         [](int, X509_STORE_CTX*) noexcept { return 1; });
      return;
   }
   context.set_verify_mode(mode);
}

void configure_server_alpn(asio::ssl::context& context, std::vector<std::string>& configured,
                           const context_options& options) {
   if (options.role != endpoint_role::server || options.alpn_protocols.empty()) {
      return;
   }
   configured = options.alpn_protocols;
   SSL_CTX_set_alpn_select_cb(context.native_handle(), select_alpn, &configured);
}

[[nodiscard]] std::string normalize_fingerprint(std::string_view value) {
   auto out = std::string{};
   out.reserve(value.size());
   for (const auto character : value) {
      if (character == ':' || character == ' ' || character == '\t' || character == '\n' || character == '\r') {
         continue;
      }
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
   }
   return out;
}

[[nodiscard]] peer_certificate peer_certificate_from_x509(X509* certificate) {
   const auto length = i2d_X509(certificate, nullptr);
   if (length <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::peer_certificate_invalid, "failed to size peer certificate DER");
   }

   auto der = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
   auto* cursor = der.data();
   if (i2d_X509(certificate, &cursor) != length) {
      FORGE_THROW_EXCEPTION(exceptions::peer_certificate_invalid, "failed to write peer certificate DER");
   }

   try {
      auto parsed = crypto::pki::x509::certificate::from_der(der);
      return peer_certificate{.der = std::move(der), .sha256_fingerprint = parsed.fingerprint_sha256_text()};
   } catch (const forge::exceptions::base& error) {
      FORGE_THROW_EXCEPTION(exceptions::peer_certificate_invalid, "failed to parse peer certificate",
                            forge::exceptions::ctx("reason", error.message()));
   }
}

[[nodiscard]] std::optional<std::string> select_sni(const client_stream_options& options) {
   switch (options.sni) {
   case sni_policy::endpoint_host:
      if (!options.server_name.empty()) {
         return options.server_name;
      }
      if (!options.endpoint_host.empty()) {
         return options.endpoint_host;
      }
      return std::nullopt;
   case sni_policy::explicit_name:
      if (options.server_name.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "explicit TLS SNI requires a server name");
      }
      return options.server_name;
   case sni_policy::disabled:
      return std::nullopt;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unknown TLS SNI policy");
}

void require_trusted_peer(SSL* native_handle) {
   const auto result = SSL_get_verify_result(native_handle);
   if (result != X509_V_OK) {
      FORGE_THROW_EXCEPTION(exceptions::certificate_chain_untrusted, "TLS peer certificate chain verification failed",
                            forge::exceptions::ctx("reason", X509_verify_cert_error_string(result)));
   }
}

void verify_hostname(const peer_certificate& certificate, std::string_view host) {
   if (host.empty()) {
      return;
   }

   const auto* cursor = certificate.der.data();
   auto parsed = x509_ptr{d2i_X509(nullptr, &cursor, static_cast<long>(certificate.der.size()))};
   if (!parsed) {
      FORGE_THROW_EXCEPTION(exceptions::peer_certificate_invalid,
                            "failed to parse peer certificate for hostname verification");
   }

   const auto host_value = std::string{host};
   auto address_error = boost::system::error_code{};
   (void)boost::asio::ip::make_address(host_value, address_error);
   const auto accepted = address_error
                             ? X509_check_host(parsed.get(), host_value.c_str(), host_value.size(), 0, nullptr)
                             : X509_check_ip_asc(parsed.get(), host_value.c_str(), 0);
   if (accepted != 1) {
      FORGE_THROW_EXCEPTION(exceptions::hostname_mismatch, "TLS peer certificate hostname mismatch",
                            forge::exceptions::ctx("host", host_value));
   }
}

} // namespace

context_snapshot::context_snapshot(std::shared_ptr<impl> impl_value) : impl_(std::move(impl_value)) {}

context_snapshot::~context_snapshot() = default;

boost::asio::ssl::context& context_snapshot::context_for_stream() const noexcept {
   return impl_->native;
}

std::shared_ptr<asio_tls_stream> make_asio_stream(context_snapshot_ptr snapshot, boost::asio::ip::tcp::socket socket) {
   if (!snapshot) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS stream requires a context snapshot");
   }

   auto* stream = new asio_tls_stream{std::move(socket), snapshot->context_for_stream()};
   return {stream, [snapshot = std::move(snapshot)](auto* value) { delete value; }};
}

std::shared_ptr<beast_tls_stream> make_beast_stream(context_snapshot_ptr snapshot, boost::beast::tcp_stream stream) {
   if (!snapshot) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS stream requires a context snapshot");
   }

   auto* result = new beast_tls_stream{std::move(stream), snapshot->context_for_stream()};
   return {result, [snapshot = std::move(snapshot)](auto* value) { delete value; }};
}

std::span<const unsigned char> context_snapshot::client_alpn_wire() const noexcept {
   return impl_->client_alpn_wire;
}

peer_verification context_snapshot::verification() const noexcept {
   return impl_->verification_value;
}

void configure_client_stream(SSL* native_handle, const context_snapshot& snapshot, client_stream_options options) {
   if (native_handle == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS client stream has no native handle");
   }

   const auto host = select_sni(options);
   if (host && SSL_set_tlsext_host_name(native_handle, host->c_str()) != 1) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "failed to configure TLS SNI");
   }

   const auto alpn = snapshot.client_alpn_wire();
   if (!alpn.empty() && SSL_set_alpn_protos(native_handle, alpn.data(), static_cast<unsigned>(alpn.size())) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "failed to configure TLS ALPN");
   }
}

std::optional<peer_certificate> extract_peer_certificate(SSL* native_handle) {
   if (native_handle == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS peer extraction requires a native handle");
   }

   auto certificate = x509_ptr{SSL_get1_peer_certificate(native_handle)};
   if (!certificate) {
      return std::nullopt;
   }
   return peer_certificate_from_x509(certificate.get());
}

certificate_chain extract_peer_certificate_chain(SSL* native_handle) {
   auto out = certificate_chain{};
   if (auto leaf = extract_peer_certificate(native_handle)) {
      out.certificates.push_back(std::move(*leaf));
   }

   auto* native_chain = SSL_get_peer_cert_chain(native_handle);
   if (native_chain == nullptr) {
      return out;
   }

   const auto count = sk_X509_num(native_chain);
   for (auto index = 0; index < count; ++index) {
      auto* certificate = sk_X509_value(native_chain, index);
      if (certificate == nullptr) {
         continue;
      }

      auto next = peer_certificate_from_x509(certificate);
      const auto duplicate_leaf =
          !out.certificates.empty() && out.certificates.front().der.size() == next.der.size() &&
          std::equal(out.certificates.front().der.begin(), out.certificates.front().der.end(), next.der.begin());
      if (!duplicate_leaf) {
         out.certificates.push_back(std::move(next));
      }
   }

   return out;
}

void classify_handshake_failure(SSL* native_handle, const context_snapshot& snapshot) {
   if (native_handle == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS handshake classification requires a native handle");
   }
   if (snapshot.verification() == peer_verification::none) {
      return;
   }

   if (verifies_peer_chain(snapshot.verification())) {
      require_trusted_peer(native_handle);
   }
   if (requires_peer_certificate(snapshot.verification()) && !extract_peer_certificate(native_handle)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_certificate_missing, "TLS peer did not present a required certificate");
   }
}

void validate_peer(SSL* native_handle, const context_snapshot& snapshot, const peer_validation& validation) {
   if (native_handle == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS peer validation requires a native handle");
   }

   const auto requires_certificate = snapshot.verification() != peer_verification::none ||
                                     !validation.expected_host.empty() || validation.expected_sha256_fingerprint ||
                                     static_cast<bool>(validation.verifier);
   if (!requires_certificate) {
      return;
   }

   if (verifies_peer_chain(snapshot.verification())) {
      require_trusted_peer(native_handle);
   }

   const auto chain = extract_peer_certificate_chain(native_handle);
   if (chain.certificates.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::peer_certificate_missing, "TLS peer did not present a certificate");
   }

   const auto& certificate = chain.certificates.front();
   verify_hostname(certificate, validation.expected_host);
   if (validation.expected_sha256_fingerprint) {
      const auto actual = normalize_fingerprint(certificate.sha256_fingerprint);
      const auto expected = normalize_fingerprint(*validation.expected_sha256_fingerprint);
      if (actual != expected) {
         FORGE_THROW_EXCEPTION(exceptions::fingerprint_mismatch, "TLS peer certificate fingerprint mismatch",
                               forge::exceptions::ctx("actual", actual));
      }
   }
   if (validation.verifier && !validation.verifier(chain)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_rejected, "TLS peer verifier rejected certificate");
   }
}

std::string selected_alpn(SSL* native_handle) {
   if (native_handle == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS ALPN inspection requires a native handle");
   }

   const auto* data = static_cast<const unsigned char*>(nullptr);
   auto length = unsigned{};
   SSL_get0_alpn_selected(native_handle, &data, &length);
   if (data == nullptr || length == 0) {
      return {};
   }
   return std::string{reinterpret_cast<const char*>(data), length};
}

context_snapshot_ptr make_context(context_options options) {
   auto erase_options = context_options_eraser{options};
   validate_options(options);
   auto alpn_wire = encode_alpn(options.alpn_protocols);

   try {
      const auto method =
          options.role == endpoint_role::client ? asio::ssl::context::tls_client : asio::ssl::context::tls_server;
      auto impl_value = std::make_shared<context_snapshot::impl>(method, options.verification);
      auto& context = impl_value->native;
      context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                          asio::ssl::context::no_sslv3);
      configure_protocols(context, options.protocols);
      load_identity(context, options);
      load_trust_anchors(context, options);
      configure_verification(context, options.verification);
      configure_server_alpn(context, impl_value->alpn_protocols, options);
      if (options.role == endpoint_role::client) {
         impl_value->client_alpn_wire = std::move(alpn_wire);
      }

      return context_snapshot_ptr{new context_snapshot{std::move(impl_value)}};
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const boost::system::system_error& error) {
      FORGE_THROW_EXCEPTION(exceptions::context_creation_failed, "failed to create TLS context",
                            forge::exceptions::ctx("reason", error.code().message()));
   }
}

context_provider::context_provider(context_options initial) {
   auto erase_initial = context_options_eraser{initial};
   current_ = make_context(std::move(initial));
}

context_snapshot_ptr context_provider::snapshot() const noexcept {
   return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

void context_provider::replace(context_options replacement) {
   auto erase_replacement = context_options_eraser{replacement};
   auto next = make_context(std::move(replacement));
   const auto retired = exchange(std::move(next));
}

context_snapshot_ptr context_provider::exchange(context_snapshot_ptr prepared) {
   if (!prepared) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS context replacement must not be null");
   }
   return std::atomic_exchange_explicit(&current_, std::move(prepared), std::memory_order_acq_rel);
}

} // namespace forge::net::tls
