#include "libp2p_identity.hxx"

#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace forge::test::libp2p_interop {
namespace {

void require_openssl(bool ok, std::string_view message) {
   if (!ok) {
      throw std::runtime_error{std::string{message}};
   }
}

std::string bio_to_string(BIO* bio) {
   BUF_MEM* memory = nullptr;
   BIO_get_mem_ptr(bio, &memory);
   if (memory == nullptr) {
      throw std::runtime_error{"failed to read OpenSSL memory BIO"};
   }
   return std::string{memory->data, memory->length};
}

std::vector<std::uint8_t> public_key_spki_der(EVP_PKEY* key) {
   const auto length = i2d_PUBKEY(key, nullptr);
   require_openssl(length > 0, "failed to DER-encode libp2p certificate public key");
   auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
   auto* cursor = out.data();
   require_openssl(i2d_PUBKEY(key, &cursor) == length, "failed to DER-write libp2p certificate public key");
   return out;
}

std::vector<std::uint8_t> raw_public_key(EVP_PKEY* key) {
   auto size = std::size_t{};
   require_openssl(EVP_PKEY_get_raw_public_key(key, nullptr, &size) == 1 && size != 0,
                   "failed to size libp2p identity public key");
   auto out = std::vector<std::uint8_t>(size);
   require_openssl(EVP_PKEY_get_raw_public_key(key, out.data(), &size) == 1,
                   "failed to read libp2p identity public key");
   out.resize(size);
   return out;
}

std::vector<std::uint8_t> identity_sign(EVP_PKEY* key, std::span<const std::uint8_t> message) {
   auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
   require_openssl(context != nullptr, "failed to allocate libp2p signing context");
   require_openssl(EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1,
                   "failed to initialize libp2p certificate extension signer");
   auto size = std::size_t{};
   require_openssl(EVP_DigestSign(context.get(), nullptr, &size, message.data(), message.size()) == 1,
                   "failed to size libp2p certificate extension signature");
   auto out = std::vector<std::uint8_t>(size);
   require_openssl(EVP_DigestSign(context.get(), out.data(), &size, message.data(), message.size()) == 1,
                   "failed to sign libp2p certificate extension");
   out.resize(size);
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

std::vector<std::uint8_t> signed_key_der(std::span<const std::uint8_t> public_key,
                                         std::span<const std::uint8_t> signature) {
   auto content = std::vector<std::uint8_t>{};
   append_der_octet_string(content, public_key);
   append_der_octet_string(content, signature);
   auto out = std::vector<std::uint8_t>{0x30};
   append_der_length(out, content.size());
   out.insert(out.end(), content.begin(), content.end());
   return out;
}

} // namespace

libp2p_identity generate_libp2p_identity() {
   auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>{
       EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), &EVP_PKEY_free};
   require_openssl(key != nullptr, "failed to generate libp2p Ed25519 identity key");

   const auto spki = public_key_spki_der(key.get());
   auto signed_message = std::vector<std::uint8_t>{};
   constexpr auto prefix = std::string_view{"libp2p-tls-handshake:"};
   signed_message.insert(signed_message.end(), prefix.begin(), prefix.end());
   signed_message.insert(signed_message.end(), spki.begin(), spki.end());
   const auto signature = identity_sign(key.get(), signed_message);
   const auto public_key = forge::net::p2p::encode_public_key(forge::net::p2p::public_key{
       .type = forge::net::p2p::public_key::type::ed25519,
       .data = raw_public_key(key.get()),
   });
   const auto extension_value = signed_key_der(public_key, signature);

   auto certificate = std::unique_ptr<X509, decltype(&X509_free)>{X509_new(), &X509_free};
   require_openssl(certificate != nullptr, "failed to allocate libp2p certificate");
   require_openssl(X509_set_version(certificate.get(), 2) == 1, "failed to set libp2p certificate version");
   require_openssl(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1,
                   "failed to set libp2p certificate serial");
   require_openssl(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr,
                   "failed to set libp2p certificate notBefore");
   require_openssl(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 100L * 365L * 24L * 60L * 60L) != nullptr,
                   "failed to set libp2p certificate notAfter");
   require_openssl(X509_set_pubkey(certificate.get(), key.get()) == 1, "failed to set libp2p certificate key");
   auto* name = X509_get_subject_name(certificate.get());
   require_openssl(name != nullptr, "failed to allocate libp2p certificate subject");
   const auto serial = std::string{"forge-libp2p-interop"};
   require_openssl(X509_NAME_add_entry_by_txt(name, "serialNumber", MBSTRING_ASC,
                                              reinterpret_cast<const unsigned char*>(serial.data()),
                                              static_cast<int>(serial.size()), -1, 0) == 1,
                   "failed to set libp2p certificate subject");
   require_openssl(X509_set_issuer_name(certificate.get(), name) == 1, "failed to set libp2p certificate issuer");

   auto object = std::unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)>{
       OBJ_txt2obj("1.3.6.1.4.1.53594.1.1", 1), &ASN1_OBJECT_free};
   require_openssl(object != nullptr, "failed to create libp2p extension OID");
   auto octets = std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)>{
       ASN1_OCTET_STRING_new(), &ASN1_OCTET_STRING_free};
   require_openssl(octets != nullptr, "failed to allocate libp2p extension value");
   require_openssl(
       ASN1_OCTET_STRING_set(octets.get(), extension_value.data(), static_cast<int>(extension_value.size())) == 1,
       "failed to set libp2p extension value");
   auto extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>{
       X509_EXTENSION_create_by_OBJ(nullptr, object.get(), 1, octets.get()), &X509_EXTENSION_free};
   require_openssl(extension != nullptr, "failed to create libp2p public key extension");
   require_openssl(X509_add_ext(certificate.get(), extension.get(), -1) == 1,
                   "failed to add libp2p public key extension");
   require_openssl(X509_sign(certificate.get(), key.get(), nullptr) > 0, "failed to sign libp2p certificate");

   auto certificate_bio = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), &BIO_free};
   auto private_key_bio = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), &BIO_free};
   require_openssl(certificate_bio != nullptr && private_key_bio != nullptr, "failed to allocate libp2p PEM BIO");
   require_openssl(PEM_write_bio_X509(certificate_bio.get(), certificate.get()) == 1,
                   "failed to write libp2p certificate PEM");
   require_openssl(PEM_write_bio_PrivateKey(private_key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ==
                       1,
                   "failed to write libp2p private key PEM");

   return libp2p_identity{
       .certificate_pem = bio_to_string(certificate_bio.get()),
       .private_key_pem = bio_to_string(private_key_bio.get()),
       .public_key = public_key,
       .peer = forge::net::p2p::make_peer_id(forge::net::p2p::public_key{
           .type = forge::net::p2p::public_key::type::ed25519,
           .data = raw_public_key(key.get()),
       }),
   };
}

} // namespace forge::test::libp2p_interop
