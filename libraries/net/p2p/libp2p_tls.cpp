module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

module forge.net.p2p.node;

import forge.crypto.asymmetric;
import forge.crypto.pki.x509;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.negotiation;
import forge.net.stcp.connection;
import forge.net.stcp.options;

#include "details/identity_signature.hxx"

#include "details/libp2p_tls.hxx"

namespace forge::net::p2p {
namespace {

constexpr auto extension_oid = "1.3.6.1.4.1.53594.1.1";
constexpr auto signing_prefix = std::string_view{"libp2p-tls-handshake:"};
constexpr auto yamux_protocol = std::string_view{"/yamux/1.0.0"};
constexpr auto legacy_libp2p_alpn = std::string_view{"libp2p"};

struct bio_deleter {
   void operator()(BIO* value) const noexcept {
      BIO_free(value);
   }
};

struct pkey_deleter {
   void operator()(EVP_PKEY* value) const noexcept {
      EVP_PKEY_free(value);
   }
};

struct x509_deleter {
   void operator()(X509* value) const noexcept {
      X509_free(value);
   }
};

struct asn1_object_deleter {
   void operator()(ASN1_OBJECT* value) const noexcept {
      ASN1_OBJECT_free(value);
   }
};

struct asn1_octet_string_deleter {
   void operator()(ASN1_OCTET_STRING* value) const noexcept {
      ASN1_OCTET_STRING_free(value);
   }
};

struct x509_extension_deleter {
   void operator()(X509_EXTENSION* value) const noexcept {
      X509_EXTENSION_free(value);
   }
};

using bio_ptr = std::unique_ptr<BIO, bio_deleter>;
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;
using asn1_object_ptr = std::unique_ptr<ASN1_OBJECT, asn1_object_deleter>;
using asn1_octet_string_ptr = std::unique_ptr<ASN1_OCTET_STRING, asn1_octet_string_deleter>;
using x509_extension_ptr = std::unique_ptr<X509_EXTENSION, x509_extension_deleter>;

[[noreturn]] void throw_identity(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_identity, std::move(message));
}

void require_openssl(bool ok, std::string_view message) {
   if (!ok) {
      throw_identity(std::string{message});
   }
}

[[nodiscard]] std::string bio_to_string(BIO* bio) {
   auto* memory = static_cast<BUF_MEM*>(nullptr);
   BIO_get_mem_ptr(bio, &memory);
   if (memory == nullptr || memory->data == nullptr) {
      throw_identity("failed to read libp2p TLS PEM memory BIO");
   }
   return {memory->data, memory->length};
}

[[nodiscard]] pkey_ptr generate_certificate_key() {
   auto key = pkey_ptr{EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519")};
   require_openssl(key != nullptr, "failed to generate libp2p TLS certificate key");
   return key;
}

[[nodiscard]] std::vector<std::uint8_t> public_key_spki_der(EVP_PKEY* key) {
   const auto length = i2d_PUBKEY(key, nullptr);
   require_openssl(length > 0, "failed to size libp2p TLS certificate public key DER");
   auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
   auto* cursor = out.data();
   require_openssl(i2d_PUBKEY(key, &cursor) == length, "failed to write libp2p TLS certificate public key DER");
   return out;
}

void append_der_length(std::vector<std::uint8_t>& out, std::size_t value) {
   if (value < 128) {
      out.push_back(static_cast<std::uint8_t>(value));
      return;
   }
   auto bytes = std::vector<std::uint8_t>{};
   while (value != 0) {
      bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
      value >>= 8U;
   }
   out.push_back(static_cast<std::uint8_t>(0x80U | bytes.size()));
   for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
      out.push_back(*it);
   }
}

void append_der_octet_string(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> value) {
   out.push_back(0x04);
   append_der_length(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}

[[nodiscard]] std::vector<std::uint8_t> signed_key_der(std::span<const std::uint8_t> public_key,
                                                       std::span<const std::uint8_t> signature) {
   auto content = std::vector<std::uint8_t>{};
   append_der_octet_string(content, public_key);
   append_der_octet_string(content, signature);
   auto out = std::vector<std::uint8_t>{0x30};
   append_der_length(out, content.size());
   out.insert(out.end(), content.begin(), content.end());
   return out;
}

void add_libp2p_extension(X509* certificate, std::span<const std::uint8_t> value) {
   auto object = asn1_object_ptr{OBJ_txt2obj(extension_oid, 1)};
   require_openssl(object != nullptr, "failed to create libp2p TLS extension OID");
   auto octets = asn1_octet_string_ptr{ASN1_OCTET_STRING_new()};
   require_openssl(octets != nullptr, "failed to allocate libp2p TLS extension value");
   require_openssl(ASN1_OCTET_STRING_set(octets.get(), value.data(), static_cast<int>(value.size())) == 1,
                   "failed to set libp2p TLS extension value");
   auto extension = x509_extension_ptr{X509_EXTENSION_create_by_OBJ(nullptr, object.get(), 1, octets.get())};
   require_openssl(extension != nullptr, "failed to create libp2p TLS public key extension");
   require_openssl(X509_add_ext(certificate, extension.get(), -1) == 1,
                   "failed to add libp2p TLS public key extension");
}

[[nodiscard]] x509_ptr parse_certificate(std::span<const std::uint8_t> der) {
   const auto* cursor = der.data();
   auto certificate = x509_ptr{d2i_X509(nullptr, &cursor, static_cast<long>(der.size()))};
   require_openssl(certificate != nullptr, "failed to parse libp2p TLS certificate");
   return certificate;
}

void verify_certificate_basics(X509* certificate) {
   if (X509_cmp_current_time(X509_get0_notBefore(certificate)) > 0 ||
       X509_cmp_current_time(X509_get0_notAfter(certificate)) < 0) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS certificate is not currently valid");
   }
   auto key = pkey_ptr{X509_get_pubkey(certificate)};
   if (!key || X509_verify(certificate, key.get()) != 1) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS certificate self-signature is invalid");
   }
}

[[nodiscard]] std::vector<std::uint8_t> make_signing_message(std::span<const std::uint8_t> spki) {
   auto message = std::vector<std::uint8_t>{};
   message.reserve(signing_prefix.size() + spki.size());
   message.insert(message.end(), signing_prefix.begin(), signing_prefix.end());
   message.insert(message.end(), spki.begin(), spki.end());
   return message;
}

[[nodiscard]] peer_id verify_certificate_identity(const forge::crypto::pki::x509::certificate& certificate,
                                                  const std::optional<peer_id>& expected_peer) {
   const auto value = certificate.extension(extension_oid);
   if (value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "libp2p TLS certificate is missing public key extension");
   }
   auto offset = std::size_t{};
   auto read_length = [&value](std::size_t& cursor) {
      if (cursor >= value.size()) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS extension is truncated");
      }
      const auto first = value[cursor++];
      if ((first & 0x80U) == 0) {
         return static_cast<std::size_t>(first);
      }
      const auto count = static_cast<std::size_t>(first & 0x7fU);
      if (count == 0 || count > sizeof(std::size_t) || count > value.size() - cursor) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "invalid libp2p TLS extension length");
      }
      auto out = std::size_t{};
      for (auto i = std::size_t{}; i < count; ++i) {
         out = (out << 8U) | value[cursor++];
      }
      return out;
   };
   auto read_octet = [&value, &read_length](std::size_t& cursor) {
      if (cursor >= value.size() || value[cursor++] != 0x04) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS extension expected octet string");
      }
      const auto length = read_length(cursor);
      if (length > value.size() - cursor) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS extension octet is truncated");
      }
      const auto begin = value.begin() + static_cast<std::ptrdiff_t>(cursor);
      const auto end = begin + static_cast<std::ptrdiff_t>(length);
      auto out = std::vector<std::uint8_t>{begin, end};
      cursor += length;
      return out;
   };
   if (value.empty() || value[offset++] != 0x30) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS extension expected sequence");
   }
   const auto length = read_length(offset);
   if (length != value.size() - offset) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS extension sequence length mismatch");
   }
   auto encoded_key = read_octet(offset);
   auto signature = read_octet(offset);
   if (offset != value.size()) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS extension has trailing bytes");
   }
   const auto key = decode_public_key(encoded_key);
   const auto peer = make_peer_id(key);
   if (expected_peer && peer != *expected_peer) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS peer id mismatch");
   }
   const auto message = make_signing_message(certificate.public_key_der());
   if (!verify_identity_signature(key, message, signature)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "libp2p TLS public key extension signature is invalid");
   }
   return peer;
}

} // namespace

libp2p_tls_material make_libp2p_tls_material(const libp2p_identity_material& identity) {
   auto certificate_key = generate_certificate_key();
   const auto spki = public_key_spki_der(certificate_key.get());
   const auto message = make_signing_message(spki);
   const auto signature = sign_identity(require_libp2p_identity_private_key(identity), message);
   const auto extension = signed_key_der(identity.public_key, signature);

   auto certificate = x509_ptr{X509_new()};
   require_openssl(certificate != nullptr, "failed to allocate libp2p TLS certificate");
   require_openssl(X509_set_version(certificate.get(), 2) == 1, "failed to set libp2p TLS certificate version");
   ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
   X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
   X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 100L * 365L * 24L * 60L * 60L);
   require_openssl(X509_set_pubkey(certificate.get(), certificate_key.get()) == 1,
                   "failed to set libp2p TLS certificate key");
   auto* name = X509_get_subject_name(certificate.get());
   require_openssl(name != nullptr, "failed to access libp2p TLS certificate subject");
   const auto organization = std::string_view{"libp2p.io"};
   require_openssl(X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                              reinterpret_cast<const unsigned char*>(organization.data()),
                                              static_cast<int>(organization.size()), -1, 0) == 1,
                   "failed to set libp2p TLS certificate subject");
   require_openssl(X509_set_issuer_name(certificate.get(), name) == 1, "failed to set libp2p TLS certificate issuer");
   add_libp2p_extension(certificate.get(), extension);
   require_openssl(X509_sign(certificate.get(), certificate_key.get(), nullptr) > 0,
                   "failed to self-sign libp2p TLS certificate");

   auto certificate_bio = bio_ptr{BIO_new(BIO_s_mem())};
   auto key_bio = bio_ptr{BIO_new(BIO_s_mem())};
   require_openssl(certificate_bio != nullptr && key_bio != nullptr, "failed to allocate libp2p TLS PEM BIO");
   require_openssl(PEM_write_bio_X509(certificate_bio.get(), certificate.get()) == 1,
                   "failed to write libp2p TLS certificate PEM");
   require_openssl(
       PEM_write_bio_PrivateKey(key_bio.get(), certificate_key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1,
       "failed to write libp2p TLS private key PEM");
   return libp2p_tls_material{.certificate_pem = bio_to_string(certificate_bio.get()),
                              .private_key_pem = bio_to_string(key_bio.get())};
}

peer_id verify_libp2p_tls_chain(const forge::net::stcp::certificate_chain& chain,
                                const std::optional<peer_id>& expected_peer) {
   if (chain.certificates.size() != 1) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "libp2p TLS requires exactly one peer certificate");
   }
   auto certificate = parse_certificate(chain.certificates.front().der);
   verify_certificate_basics(certificate.get());
   auto parsed = forge::crypto::pki::x509::certificate::from_der(chain.certificates.front().der);
   return verify_certificate_identity(parsed, expected_peer);
}

libp2p_tls_handshake verify_libp2p_tls_handshake(const forge::net::stcp::certificate_chain& chain,
                                               std::string_view selected_alpn,
                                               const std::optional<peer_id>& expected_peer) {
   const auto peer = verify_libp2p_tls_chain(chain, expected_peer);
   // Legacy/absent ALPN supplies no inline decision; the upgrader must select
   // the muxer on the authenticated stream rather than infer Yamux support.
   if (selected_alpn.empty() || selected_alpn == legacy_libp2p_alpn) {
      return libp2p_tls_handshake{.peer = peer};
   }
   if (selected_alpn != yamux_protocol) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "libp2p TLS did not select a supported ALPN protocol");
   }
   return libp2p_tls_handshake{.peer = peer, .muxer = protocol_id{.value = std::string{yamux_protocol}}};
}

forge::net::stcp::client_options make_libp2p_tls_client_options(const libp2p_identity_material& identity) {
   auto material = make_libp2p_tls_material(identity);
   auto out = forge::net::stcp::client_options{};
   out.security.verify_peer = false;
   out.security.require_peer_certificate = false;
   out.certificate_pem = std::move(material.certificate_pem);
   out.private_key_pem = std::move(material.private_key_pem);
   out.sni = forge::net::stcp::sni_policy::disabled;
   out.alpn_protocols = {std::string{yamux_protocol}, std::string{legacy_libp2p_alpn}};
   return out;
}

forge::net::stcp::server_options make_libp2p_tls_server_options(const libp2p_identity_material& identity) {
   auto material = make_libp2p_tls_material(identity);
   auto out = forge::net::stcp::server_options{};
   out.security.verify_peer = false;
   out.security.require_peer_certificate = true;
   out.certificate_pem = std::move(material.certificate_pem);
   out.private_key_pem = std::move(material.private_key_pem);
   out.alpn_protocols = {std::string{yamux_protocol}, std::string{legacy_libp2p_alpn}};
   return out;
}

} // namespace forge::net::p2p
