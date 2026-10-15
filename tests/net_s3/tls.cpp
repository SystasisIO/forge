#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

import forge.asio.compute;
import forge.net.s3.client;

namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace s3 = forge::net::s3;
using tcp = asio::ip::tcp;
using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using certificate_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

void require(bool value, const char* message) {
   if (!value) {
      throw std::runtime_error{message};
   }
}

key_ptr make_key() {
   auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>{
       EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
   require(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
               EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1,
           "S3 TLS fixture key setup failed");
   EVP_PKEY* value = nullptr;
   require(EVP_PKEY_keygen(context.get(), &value) == 1, "S3 TLS fixture key generation failed");
   return {value, EVP_PKEY_free};
}

certificate_ptr make_certificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key) {
   auto certificate = certificate_ptr{X509_new(), X509_free};
   require(certificate && X509_set_version(certificate.get(), 2) == 1 &&
               ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), issuer ? 2 : 1) == 1 &&
               X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) &&
               X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) &&
               X509_set_pubkey(certificate.get(), key) == 1,
           "S3 TLS fixture certificate setup failed");
   auto* subject = X509_get_subject_name(certificate.get());
   const auto* name = issuer ? "127.0.0.1" : "S3 test CA";
   require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>(name), -1, -1, 0) == 1 &&
               X509_set_issuer_name(certificate.get(), issuer ? X509_get_subject_name(issuer) : subject) == 1,
           "S3 TLS fixture certificate name setup failed");
   X509V3_CTX context;
   X509V3_set_ctx(&context, issuer ? issuer : certificate.get(), certificate.get(), nullptr, nullptr, 0);
   const auto extension = [&](int nid, const char* value) {
      auto item = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>{
          X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free};
      require(item && X509_add_ext(certificate.get(), item.get(), -1) == 1,
              "S3 TLS fixture certificate extension failed");
   };
   extension(NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
   extension(NID_key_usage, issuer ? "critical,digitalSignature,keyEncipherment" : "critical,keyCertSign,cRLSign");
   if (issuer) {
      extension(NID_ext_key_usage, "serverAuth");
      // Only this IP is valid. Connecting as localhost must still fail hostname verification.
      extension(NID_subject_alt_name, "IP:127.0.0.1");
   }
   require(X509_sign(certificate.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0,
           "S3 TLS fixture certificate signing failed");
   return certificate;
}

std::string pem(X509* certificate) {
   auto output = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), BIO_free};
   require(output && PEM_write_bio_X509(output.get(), certificate) == 1,
           "S3 TLS fixture certificate encoding failed");
   char* data = nullptr;
   const auto size = BIO_get_mem_data(output.get(), &data);
   require(size > 0, "S3 TLS fixture certificate encoding is empty");
   return {data, static_cast<std::size_t>(size)};
}

struct tls_endpoint : std::enable_shared_from_this<tls_endpoint> {
   tcp::acceptor listener;
   asio::ssl::context context{asio::ssl::context::tls_server};
   unsigned accepted_connections = 0;
   unsigned requests = 0;

   tls_endpoint(asio::any_io_executor executor, X509* certificate, EVP_PKEY* key)
       : listener{executor, {asio::ip::make_address("127.0.0.1"), 0}} {
      require(SSL_CTX_use_certificate(context.native_handle(), certificate) == 1 &&
                  SSL_CTX_use_PrivateKey(context.native_handle(), key) == 1,
              "S3 TLS fixture server identity setup failed");
   }

   std::string address(std::string_view host = "127.0.0.1") const {
      return "https://" + std::string{host} + ":" + std::to_string(listener.local_endpoint().port());
   }

   asio::awaitable<void> serve() {
      const auto self = shared_from_this();
      while (listener.is_open()) {
         boost::system::error_code error;
         auto socket = co_await listener.async_accept(asio::redirect_error(asio::use_awaitable, error));
         if (error) {
            break;
         }
         ++accepted_connections;
         asio::co_spawn(listener.get_executor(), exchange(self, std::move(socket)), asio::detached);
      }
   }

   static asio::awaitable<void> exchange(std::shared_ptr<tls_endpoint> self, tcp::socket socket) {
      try {
         asio::ssl::stream<tcp::socket> stream{std::move(socket), self->context};
         co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
         boost::beast::flat_buffer buffer;
         http::request<http::empty_body> request;
         co_await http::async_read(stream, buffer, request, asio::use_awaitable);
         ++self->requests;
         http::response<http::empty_body> response{http::status::ok, 11};
         response.set(http::field::etag, "\"tls-fixture\"");
         response.content_length(42);
         response.keep_alive(false);
         co_await http::async_write(stream, response, asio::use_awaitable);
      } catch (const boost::system::system_error&) {
         // Rejected trust and hostname checks abort the peer's TLS handshake.
      }
   }
};

struct temporary_directory {
   std::filesystem::path path = std::filesystem::temp_directory_path() /
       ("forge-s3-tls-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

   temporary_directory() {
      require(std::filesystem::create_directory(path), "S3 TLS fixture directory creation failed");
   }
   ~temporary_directory() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
   }

   std::string write(const char* name, std::string_view value) const {
      const auto file_path = path / name;
      std::ofstream file{file_path, std::ios::binary};
      file.write(value.data(), static_cast<std::streamsize>(value.size()));
      file.close();
      require(static_cast<bool>(file), "S3 TLS fixture CA file creation failed");
      return file_path.string();
   }
};

} // namespace

boost::asio::awaitable<void> exercise_tls(forge::asio::compute::pool& workers) {
   const auto ca_key = make_key();
   const auto ca = make_certificate(ca_key.get(), nullptr, nullptr);
   const auto server_key = make_key();
   const auto server_certificate = make_certificate(server_key.get(), ca.get(), ca_key.get());
   const auto foreign_key = make_key();
   const auto foreign_ca = make_certificate(foreign_key.get(), nullptr, nullptr);
   const auto directory = temporary_directory{};
   const auto ca_file = directory.write("ca.pem", pem(ca.get()));
   auto server = std::make_shared<tls_endpoint>(co_await asio::this_coro::executor,
                                              server_certificate.get(), server_key.get());
   struct close_listener {
      std::shared_ptr<tls_endpoint> server;
      ~close_listener() {
         boost::system::error_code ignored;
         server->listener.close(ignored);
      }
   } close{server};
   asio::co_spawn(server->listener.get_executor(), server->serve(), asio::detached);

   s3::config config;
   config.endpoint = server->address();
   config.identity = {"test-access", "test-secret", "", {}};
   config.connect_timeout = std::chrono::milliseconds{1000};
   config.request_timeout = std::chrono::milliseconds{1000};
   config.operation_timeout = std::chrono::milliseconds{5000};
   const auto target = s3::object{"bucket", "tls", {}};
   const auto rejected = [&](s3::config options) -> asio::awaitable<void> {
      auto client = s3::client{workers.get_executor(), std::move(options)};
      auto refused = false;
      try {
         (void)co_await client.head(target);
      } catch (const s3::exceptions::transport& error) {
         require(std::string_view{error.code().category().name()} == "forge.net.s3" &&
                     error.code().value() == static_cast<int>(s3::exceptions::code::transport),
                 "S3 TLS error lost its typed transport category");
         refused = true;
      }
      co_await client.shutdown();
      require(refused, "S3 TLS request succeeded with untrusted CA or wrong hostname");
   };

   co_await rejected(config);
   require(server->requests == 0, "default trust admitted the fixture's private CA");
   config.ca_file = ca_file;
#ifdef _WIN32
   auto refused = false;
   try {
      auto client = s3::client{workers.get_executor(), config};
   } catch (const s3::exceptions::invalid_options&) {
      refused = true;
   }
   require(refused, "S3 accepted a private CA file on an unvalidated Windows HTTP backend");
#else
   {
      auto client = s3::client{workers.get_executor(), config};
      auto authenticated = false;
      try {
         const auto metadata = co_await client.head(target);
         authenticated = metadata.size == 42 && metadata.etag == "\"tls-fixture\"";
      } catch (const s3::exceptions::transport&) {
      }
      co_await client.shutdown();
      require(authenticated, "configured S3 private CA did not authenticate the TLS endpoint");
   }
   require(server->requests == 1, "trusted S3 TLS request did not reach the endpoint exactly once");

   config.ca_file = directory.write("foreign.pem", pem(foreign_ca.get()));
   co_await rejected(config);
   config.ca_file = ca_file;
   config.endpoint = server->address("localhost");
   const auto before_hostname_check = server->accepted_connections;
   co_await rejected(config);
   require(server->accepted_connections > before_hostname_check,
           "hostname rejection did not reach the S3 TLS listener");
   config.endpoint = server->address();
   config.ca_file = directory.write("malformed.pem", "not a PEM certificate\n");
   co_await rejected(config);
   config.ca_file = directory.write("empty.pem", "");
   co_await rejected(config);
   require(server->requests == 1, "an invalid trust configuration reached an authenticated S3 request");

   const auto invalid_path = [&](std::string path) {
      config.ca_file = std::move(path);
      try {
         auto client = s3::client{workers.get_executor(), config};
      } catch (const s3::exceptions::invalid_options&) {
         return;
      }
      throw std::runtime_error{"S3 invalid CA pathname was not rejected before SDK construction"};
   };
   invalid_path((directory.path / "absent.pem").string());
   invalid_path(directory.path.string());
   invalid_path(ca_file + std::string{"\0suffix", 7});
   const auto loop = directory.path / "loop";
   std::filesystem::create_symlink("loop", loop);
   invalid_path(loop.string());
   if (::geteuid() != 0) {
      const auto unreadable = directory.write("unreadable.pem", pem(ca.get()));
      std::filesystem::permissions(unreadable, std::filesystem::perms::none);
      invalid_path(unreadable);
   }
   const auto fifo = directory.path / "ca-fifo";
   require(::mkfifo(fifo.c_str(), 0600) == 0, "S3 TLS fixture FIFO creation failed");
   invalid_path(fifo.string());
#endif
}
