#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.asio.runtime;
import forge.asio.compute;
import forge.asio.blocking;
import forge.asio.notification;
import forge.crypto.pki.x509;
import forge.net.transport.stream;
import forge.net.tcp.connection;
import forge.net.tlsnotary.engine;

namespace tlsn_test {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace tlsn = forge::net::tlsnotary;
using namespace std::chrono_literals;

inline void check(bool value, const char* message) {
   if (!value)
      throw std::runtime_error{message};
}

struct identity {
   std::string certificate;
   std::string private_key;
   std::vector<std::uint8_t> root;
};

inline std::string pem(BIO* bio) {
   char* bytes = nullptr;
   const auto size = BIO_get_mem_data(bio, &bytes);
   check(size > 0, "test PEM encoding failed");
   return {bytes, static_cast<std::size_t>(size)};
}

inline identity make_identity() {
   using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
   using context_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
   using cert_ptr = std::unique_ptr<X509, decltype(&X509_free)>;
   using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;
   auto generate = [] {
      auto context = context_ptr{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
      check(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1,
            "test key setup failed");
      EVP_PKEY* key = nullptr;
      check(EVP_PKEY_keygen(context.get(), &key) == 1, "test key generation failed");
      return key_ptr{key, EVP_PKEY_free};
   };
   auto root_key = generate();
   auto server_key = generate();
   auto make_cert = [](EVP_PKEY* key, const char* name, X509* issuer, EVP_PKEY* issuer_key, bool ca) {
      auto cert = cert_ptr{X509_new(), X509_free};
      check(cert && X509_set_version(cert.get(), 2) == 1, "test certificate allocation failed");
      ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ca ? 1 : 2);
      X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
      X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400);
      check(X509_set_pubkey(cert.get(), key) == 1, "test certificate key failed");
      auto* subject = X509_get_subject_name(cert.get());
      check(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(name), -1,
                                       -1, 0) == 1,
            "test subject failed");
      X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : subject);
      auto context = X509V3_CTX{};
      X509V3_set_ctx(&context, issuer ? issuer : cert.get(), cert.get(), nullptr, nullptr, 0);
      auto extension = [&](int nid, const char* text) {
         auto* value = X509V3_EXT_conf_nid(nullptr, &context, nid, text);
         check(value != nullptr, "test extension failed");
         const auto result = X509_add_ext(cert.get(), value, -1);
         X509_EXTENSION_free(value);
         check(result == 1, "test extension insertion failed");
      };
      extension(NID_basic_constraints, ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
      extension(NID_key_usage, ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature,keyEncipherment");
      if (!ca) {
         extension(NID_ext_key_usage, "serverAuth");
         extension(NID_subject_alt_name, "DNS:tlsn.local,DNS:other.local");
      }

      check(X509_sign(cert.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0, "test certificate signing failed");
      return cert;
   };
   auto root = make_cert(root_key.get(), "test root", nullptr, nullptr, true);
   auto leaf = make_cert(server_key.get(), "tlsn.local", root.get(), root_key.get(), false);
   auto cert_bio = bio_ptr{BIO_new(BIO_s_mem()), BIO_free};
   auto key_bio = bio_ptr{BIO_new(BIO_s_mem()), BIO_free};
   check(PEM_write_bio_X509(cert_bio.get(), leaf.get()) == 1 &&
             PEM_write_bio_PrivateKey(key_bio.get(), server_key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1,
         "test identity encoding failed");
   auto size = i2d_X509(root.get(), nullptr);
   check(size > 0, "test root DER encoding failed");
   auto der = std::vector<std::uint8_t>(size);
   auto* output = der.data();
   check(i2d_X509(root.get(), &output) == size, "test root DER encoding failed");
   auto parsed = forge::crypto::pki::x509::certificate::from_der(der);
   check(!parsed.fingerprint_sha256().empty(), "Forge PKI root parsing failed");
   return {pem(cert_bio.get()), pem(key_bio.get()), std::move(der)};
}

inline forge::net::transport::stream transport(tcp::socket socket) {
   return std::move(forge::net::tcp::connection{std::move(socket)}).into_transport_stream().stream;
}

inline std::pair<forge::net::transport::stream, forge::net::transport::stream> pair(forge::asio::runtime& runtime) {
   auto listener = tcp::acceptor{runtime.context(), {tcp::v4(), 0}};
   auto first = tcp::socket{runtime.context()};
   first.connect({asio::ip::address_v4::loopback(), listener.local_endpoint().port()});
   auto second = listener.accept();
   return {transport(std::move(first)), transport(std::move(second))};
}

struct https_fixture {
   explicit https_fixture(forge::asio::runtime& runtime, const identity& identity)
       : context{asio::ssl::context::tls_server}, acceptor{runtime.context(), {tcp::v4(), 0}} {
      context.use_certificate_chain(asio::buffer(identity.certificate));
      context.use_private_key(asio::buffer(identity.private_key), asio::ssl::context::pem);
      check(SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) == 1 &&
                SSL_CTX_set_max_proto_version(context.native_handle(), TLS1_2_VERSION) == 1 &&
                SSL_CTX_set_cipher_list(context.native_handle(), "ECDHE-RSA-AES128-GCM-SHA256") == 1,
            "test TLS configuration failed");
   }

   asio::awaitable<void> serve() {
      auto socket = co_await acceptor.async_accept(asio::use_awaitable);
      auto tls = asio::ssl::stream<tcp::socket>{std::move(socket), context};
      co_await tls.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
      auto input = std::string{};
      co_await asio::async_read_until(tls, asio::dynamic_buffer(input, 65536), "\r\n\r\n", asio::use_awaitable);
      check(input.find("X-Private: secret-sentinel") != std::string::npos, "test private header missing");
      auto response =
          std::string{"HTTP/1.1 200 OK\r\nContent-Length: 17\r\nConnection: close\r\n\r\n{\"value\":7318921}"};
      co_await asio::async_write(tls, asio::buffer(response), asio::use_awaitable);
      auto error = boost::system::error_code{};
      co_await tls.async_shutdown(asio::redirect_error(asio::use_awaitable, error));
   }

   forge::net::transport::stream connect(forge::asio::runtime& runtime) {
      auto socket = tcp::socket{runtime.context()};
      socket.connect({asio::ip::address_v4::loopback(), acceptor.local_endpoint().port()});
      return transport(std::move(socket));
   }

   asio::ssl::context context;
   tcp::acceptor acceptor;
};

inline tlsn::session_options options(std::uint64_t id) {
   auto result = tlsn::session_options{};
   result.id = id;
   result.timeout = 90s;
   result.max_sent = 1024;
   result.max_received = 1024;
   result.max_sent_records = 32;
   result.max_received_records_online = 32;
   result.reveal_sent = {{0, 18}};
   result.reveal_received = {{0, 0xffffffff}};
   return result;
}

inline std::vector<std::uint8_t> request() {
   const auto text = std::string{
       "GET /probe HTTP/1.1\r\nHost: tlsn.local\r\nX-Private: secret-sentinel\r\nConnection: close\r\n\r\n"};
   return {text.begin(), text.end()};
}

struct engine_cleanup {
   forge::asio::runtime& runtime;
   tlsn::engine& owner;
   ~engine_cleanup() {
      try {
         forge::asio::blocking::run(runtime, owner.shutdown());
      } catch (...) {
      }
   }
};

inline void await_ready(std::future<tlsn::result>& future) {
   check(future.wait_for(100s) == std::future_status::ready, "test session completion timeout");
}
} // namespace tlsn_test
