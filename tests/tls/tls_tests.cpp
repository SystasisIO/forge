#include <boost/test/unit_test.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/bio.h>
#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

import forge.net.tls.context;
import forge.net.tls.exceptions;

namespace {

namespace asio = boost::asio;

struct evp_pkey_deleter {
   void operator()(EVP_PKEY* value) const noexcept {
      EVP_PKEY_free(value);
   }
};

struct evp_pkey_context_deleter {
   void operator()(EVP_PKEY_CTX* value) const noexcept {
      EVP_PKEY_CTX_free(value);
   }
};

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

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>;
using evp_pkey_context_ptr = std::unique_ptr<EVP_PKEY_CTX, evp_pkey_context_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;
using bio_ptr = std::unique_ptr<BIO, bio_deleter>;

[[nodiscard]] evp_pkey_ptr make_key() {
   auto context = evp_pkey_context_ptr{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
   BOOST_REQUIRE(context);
   BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen_init(context.get()), 1);
   BOOST_REQUIRE_EQUAL(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048), 1);

   auto* value = static_cast<EVP_PKEY*>(nullptr);
   BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen(context.get(), &value), 1);
   return evp_pkey_ptr{value};
}

[[nodiscard]] x509_ptr make_certificate(EVP_PKEY* key) {
   auto certificate = x509_ptr{X509_new()};
   BOOST_REQUIRE(certificate);
   BOOST_REQUIRE_EQUAL(X509_set_version(certificate.get(), 2), 1);
   BOOST_REQUIRE_EQUAL(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1), 1);
   BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0));
   BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60 * 60));
   BOOST_REQUIRE_EQUAL(X509_set_pubkey(certificate.get(), key), 1);

   auto* subject = X509_get_subject_name(certificate.get());
   BOOST_REQUIRE(subject);
   BOOST_REQUIRE_EQUAL(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                                  reinterpret_cast<const unsigned char*>("forge tls test"), -1, -1, 0),
                       1);
   BOOST_REQUIRE_EQUAL(X509_set_issuer_name(certificate.get(), subject), 1);
   BOOST_REQUIRE(X509_sign(certificate.get(), key, EVP_sha256()));
   return certificate;
}

[[nodiscard]] std::string read_bio(BIO* value) {
   const auto* data = static_cast<char*>(nullptr);
   const auto size = BIO_get_mem_data(value, &data);
   BOOST_REQUIRE(size > 0);
   return {data, static_cast<std::size_t>(size)};
}

[[nodiscard]] std::string write_certificate_pem(X509* certificate) {
   auto output = bio_ptr{BIO_new(BIO_s_mem())};
   BOOST_REQUIRE(output);
   BOOST_REQUIRE_EQUAL(PEM_write_bio_X509(output.get(), certificate), 1);
   return read_bio(output.get());
}

[[nodiscard]] std::string write_private_key_pem(EVP_PKEY* key) {
   auto output = bio_ptr{BIO_new(BIO_s_mem())};
   BOOST_REQUIRE(output);
   BOOST_REQUIRE_EQUAL(PEM_write_bio_PrivateKey(output.get(), key, nullptr, nullptr, 0, nullptr, nullptr), 1);
   return read_bio(output.get());
}

struct identity_material {
   std::string certificate;
   std::string private_key;
};

struct handshake_outcome {
   boost::system::error_code server_error;
   boost::system::error_code client_error;
   std::exception_ptr server_exception;
   int requested_client_authority_count = -1;
   bool saw_client_certificate = false;
   bool peer_validation_succeeded = false;
   bool timed_out = false;
};

[[nodiscard]] identity_material make_identity_material() {
   auto key = make_key();
   auto certificate = make_certificate(key.get());
   return {.certificate = write_certificate_pem(certificate.get()), .private_key = write_private_key_pem(key.get())};
}

[[nodiscard]] forge::net::tls::context_options server_options(const identity_material& identity) {
   auto options = forge::net::tls::context_options{};
   options.role = forge::net::tls::endpoint_role::server;
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   options.certificate_chain_pem = identity.certificate;
   options.private_key_pem = identity.private_key;
   return options;
}

[[nodiscard]] handshake_outcome run_application_verified_handshake(const identity_material& server_identity,
                                                                   const identity_material* client_identity) {
   auto tls_options = server_options(server_identity);
   tls_options.verification = forge::net::tls::peer_verification::require_peer_certificate_for_application_verification;
   tls_options.trust_anchors_pem = {server_identity.certificate};
   const auto server_context = forge::net::tls::make_context(std::move(tls_options));

   auto client_options = forge::net::tls::context_options{};
   client_options.verification = forge::net::tls::peer_verification::none;
   client_options.use_default_verify_paths = false;
   if (client_identity != nullptr) {
      client_options.certificate_chain_pem = client_identity->certificate;
      client_options.private_key_pem = client_identity->private_key;
   }
   const auto client_context = forge::net::tls::make_context(std::move(client_options));

   auto io = asio::io_context{};
   auto listener = asio::ip::tcp::acceptor{io, {asio::ip::tcp::v4(), 0}};
   const auto endpoint = listener.local_endpoint();
   auto server = forge::net::tls::make_asio_stream(server_context, asio::ip::tcp::socket{io});
   auto client = forge::net::tls::make_asio_stream(client_context, asio::ip::tcp::socket{io});
   auto timeout = asio::steady_timer{io, std::chrono::seconds{2}};
   auto outcome = handshake_outcome{};
   auto server_done = false;
   auto client_done = false;
   const auto finish_if_done = [&] {
      if (server_done && client_done) {
         timeout.cancel();
      }
   };
   const auto cancel_stream = [](const std::shared_ptr<forge::net::tls::asio_tls_stream>& stream) {
      auto ignored = boost::system::error_code{};
      stream->lowest_layer().cancel(ignored);
      stream->lowest_layer().close(ignored);
   };

   listener.async_accept(server->lowest_layer(), [&](const boost::system::error_code& error) {
      if (error) {
         outcome.server_error = error;
         server_done = true;
         finish_if_done();
         return;
      }
      server->async_handshake(asio::ssl::stream_base::server, [&](const boost::system::error_code& handshake_error) {
         outcome.server_error = handshake_error;
         if (!handshake_error) {
            try {
               const auto certificate = forge::net::tls::extract_peer_certificate(server->native_handle());
               outcome.saw_client_certificate = certificate.has_value();
               if (certificate) {
                  forge::net::tls::validate_peer(server->native_handle(), *server_context,
                                                 {.expected_sha256_fingerprint = certificate->sha256_fingerprint,
                                                  .verifier = [](const forge::net::tls::certificate_chain& chain) {
                                                     return !chain.certificates.empty();
                                                  }});
                  outcome.peer_validation_succeeded = true;
               }
            } catch (...) {
               outcome.server_exception = std::current_exception();
               cancel_stream(server);
            }
         }
         server_done = true;
         finish_if_done();
      });
   });

   client->lowest_layer().async_connect(endpoint, [&](const boost::system::error_code& error) {
      if (error) {
         outcome.client_error = error;
         client_done = true;
         finish_if_done();
         return;
      }
      client->async_handshake(asio::ssl::stream_base::client, [&](const boost::system::error_code& handshake_error) {
         outcome.client_error = handshake_error;
         const auto* authorities = SSL_get_client_CA_list(client->native_handle());
         outcome.requested_client_authority_count = authorities == nullptr ? 0 : sk_X509_NAME_num(authorities);
         client_done = true;
         finish_if_done();
      });
   });

   timeout.async_wait([&](const boost::system::error_code& error) {
      if (error) {
         return;
      }
      outcome.timed_out = true;
      auto ignored = boost::system::error_code{};
      listener.cancel(ignored);
      listener.close(ignored);
      cancel_stream(server);
      cancel_stream(client);
   });

   io.run();

   auto ignored = boost::system::error_code{};
   listener.close(ignored);
   server->lowest_layer().close(ignored);
   client->lowest_layer().close(ignored);
   return outcome;
}

} // namespace

BOOST_AUTO_TEST_SUITE(forge_net_tls_tests)

BOOST_AUTO_TEST_CASE(rejects_malformed_certificate_pem) {
   auto options = forge::net::tls::context_options{};
   options.role = forge::net::tls::endpoint_role::server;
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   options.certificate_chain_pem = "not a certificate";
   options.private_key_pem = "not a private key";

   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(options)),
                     forge::net::tls::exceptions::identity_invalid);
}

BOOST_AUTO_TEST_CASE(rejects_unknown_context_option_enums) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;

   auto unknown_role = options;
   unknown_role.role = static_cast<forge::net::tls::endpoint_role>(255U);
   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(unknown_role)),
                     forge::net::tls::exceptions::invalid_options);

   auto unknown_protocols = options;
   unknown_protocols.protocols = static_cast<forge::net::tls::protocol_policy>(255U);
   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(unknown_protocols)),
                     forge::net::tls::exceptions::invalid_options);

   auto unknown_verification = options;
   unknown_verification.verification = static_cast<forge::net::tls::peer_verification>(255U);
   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(unknown_verification)),
                     forge::net::tls::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(rejects_malformed_private_key) {
   const auto identity = make_identity_material();
   auto options = server_options(identity);
   options.private_key_pem = "not a private key";

   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(options)),
                     forge::net::tls::exceptions::identity_invalid);
}

BOOST_AUTO_TEST_CASE(rejects_certificate_and_private_key_mismatch) {
   const auto identity = make_identity_material();
   auto different_key = make_key();
   auto options = server_options(identity);
   options.private_key_pem = write_private_key_pem(different_key.get());

   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(options)),
                     forge::net::tls::exceptions::identity_invalid);
}

BOOST_AUTO_TEST_CASE(rejects_malformed_trust_anchor) {
   const auto identity = make_identity_material();
   auto options = server_options(identity);
   options.verification = forge::net::tls::peer_verification::require_peer_certificate;
   options.trust_anchors_pem = {"not a certificate authority"};

   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(options)),
                     forge::net::tls::exceptions::trust_anchors_invalid);
}

BOOST_AUTO_TEST_CASE(requires_a_verifiable_trust_path_for_mutual_tls) {
   const auto identity = make_identity_material();
   auto options = server_options(identity);
   options.verification = forge::net::tls::peer_verification::require_peer_certificate;

   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(options)),
                     forge::net::tls::exceptions::verification_configuration_invalid);
}

BOOST_AUTO_TEST_CASE(allows_default_verify_paths_for_mutual_tls) {
   const auto identity = make_identity_material();
   auto options = server_options(identity);
   options.verification = forge::net::tls::peer_verification::require_peer_certificate;
   options.use_default_verify_paths = true;

   BOOST_CHECK_NO_THROW((void)forge::net::tls::make_context(std::move(options)));
}

BOOST_AUTO_TEST_CASE(application_verified_peer_certificate_is_server_only_and_does_not_require_trust_anchors) {
   const auto identity = make_identity_material();
   auto options = server_options(identity);
   options.verification = forge::net::tls::peer_verification::require_peer_certificate_for_application_verification;

   BOOST_CHECK_NO_THROW((void)forge::net::tls::make_context(options));

   options.role = forge::net::tls::endpoint_role::client;
   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(options)),
                     forge::net::tls::exceptions::verification_configuration_invalid);
}

BOOST_AUTO_TEST_CASE(application_verified_peer_certificate_requires_presence_but_permits_application_identity) {
   const auto server_identity = make_identity_material();
   const auto client_identity = make_identity_material();

   const auto with_certificate = run_application_verified_handshake(server_identity, &client_identity);
   BOOST_CHECK(!with_certificate.timed_out);
   BOOST_CHECK(!with_certificate.server_error);
   BOOST_CHECK(!with_certificate.client_error);
   BOOST_CHECK(!with_certificate.server_exception);
   BOOST_CHECK_EQUAL(with_certificate.requested_client_authority_count, 0);
   BOOST_CHECK(with_certificate.saw_client_certificate);
   BOOST_CHECK(with_certificate.peer_validation_succeeded);

   const auto without_certificate = run_application_verified_handshake(server_identity, nullptr);
   BOOST_CHECK(!without_certificate.timed_out);
   BOOST_CHECK(without_certificate.server_error);
   BOOST_CHECK(!without_certificate.server_exception);
   BOOST_CHECK(!without_certificate.saw_client_certificate);
   BOOST_CHECK(!without_certificate.peer_validation_succeeded);
}

BOOST_AUTO_TEST_CASE(rejects_oversized_pem_and_trust_material_before_openssl) {
   auto oversized_identity = forge::net::tls::context_options{};
   oversized_identity.verification = forge::net::tls::peer_verification::none;
   oversized_identity.use_default_verify_paths = false;
   oversized_identity.certificate_chain_pem = std::string(forge::net::tls::max_certificate_chain_pem_bytes + 1U, 'x');
   oversized_identity.private_key_pem = "x";
   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(oversized_identity)),
                     forge::net::tls::exceptions::invalid_options);

   auto oversized_trust = forge::net::tls::context_options{};
   oversized_trust.verification = forge::net::tls::peer_verification::none;
   oversized_trust.use_default_verify_paths = false;
   oversized_trust.trust_anchors_pem = {std::string(forge::net::tls::max_trust_anchor_pem_bytes + 1U, 'x')};
   BOOST_CHECK_THROW((void)forge::net::tls::make_context(std::move(oversized_trust)),
                     forge::net::tls::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(default_context_uses_tls_1_3_only) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   const auto snapshot = forge::net::tls::make_context(std::move(options));
   auto io = asio::io_context{};
   auto stream = forge::net::tls::make_asio_stream(snapshot, asio::ip::tcp::socket{io});

   BOOST_REQUIRE(snapshot);
   BOOST_CHECK_EQUAL(SSL_get_min_proto_version(stream->native_handle()), TLS1_3_VERSION);
   BOOST_CHECK_EQUAL(SSL_get_max_proto_version(stream->native_handle()), TLS1_3_VERSION);
}

BOOST_AUTO_TEST_CASE(stream_factory_creates_distinct_connection_ssl_handles) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   const auto snapshot = forge::net::tls::make_context(std::move(options));
   auto io = asio::io_context{};
   auto first = forge::net::tls::make_asio_stream(snapshot, asio::ip::tcp::socket{io});
   auto second = forge::net::tls::make_asio_stream(snapshot, asio::ip::tcp::socket{io});

   BOOST_REQUIRE(first->native_handle());
   BOOST_REQUIRE(second->native_handle());
   BOOST_CHECK_NE(first->native_handle(), second->native_handle());
   BOOST_CHECK_EQUAL(SSL_get_SSL_CTX(first->native_handle()), SSL_get_SSL_CTX(second->native_handle()));
}

BOOST_AUTO_TEST_CASE(rotates_context_snapshots_without_invalidating_the_previous_snapshot) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;

   auto provider = forge::net::tls::context_provider{options};
   const auto first = provider.snapshot();
   provider.replace(std::move(options));
   const auto second = provider.snapshot();

   BOOST_REQUIRE(first);
   BOOST_REQUIRE(second);
   BOOST_CHECK_NE(first.get(), second.get());
   BOOST_CHECK(first->client_alpn_wire().empty());
   BOOST_CHECK(second->client_alpn_wire().empty());
}

BOOST_AUTO_TEST_CASE(peer_validation_reports_missing_certificate_with_a_tls_error) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   const auto snapshot = forge::net::tls::make_context(std::move(options));
   auto io = asio::io_context{};
   auto stream = forge::net::tls::make_asio_stream(snapshot, asio::ip::tcp::socket{io});

   BOOST_CHECK_THROW(
       forge::net::tls::validate_peer(stream->native_handle(), *snapshot, {.expected_host = "forge.test"}),
       forge::net::tls::exceptions::peer_certificate_missing);
}

BOOST_AUTO_TEST_CASE(provider_allows_concurrent_snapshot_reads_and_rotations) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;

   auto provider = forge::net::tls::context_provider{options};
   const auto retained = provider.snapshot();
   std::barrier start{2};
   std::atomic_bool saw_null_snapshot = false;
   std::exception_ptr reader_error;
   std::exception_ptr writer_error;

   auto reader = std::thread{[&] {
      try {
         start.arrive_and_wait();
         for (auto index = 0U; index < 256U; ++index) {
            const auto snapshot = provider.snapshot();
            if (!snapshot) {
               saw_null_snapshot.store(true, std::memory_order_relaxed);
               return;
            }

            static_cast<void>(snapshot->client_alpn_wire());
         }
      } catch (...) {
         reader_error = std::current_exception();
      }
   }};
   auto writer = std::thread{[&] {
      try {
         start.arrive_and_wait();
         for (auto index = 0U; index < 64U; ++index) {
            provider.replace(options);
         }
      } catch (...) {
         writer_error = std::current_exception();
      }
   }};

   reader.join();
   writer.join();

   BOOST_CHECK(!reader_error);
   BOOST_CHECK(!writer_error);
   BOOST_CHECK(!saw_null_snapshot.load(std::memory_order_relaxed));
   BOOST_REQUIRE(retained);
   BOOST_CHECK(retained->client_alpn_wire().empty());
   BOOST_CHECK_NE(provider.snapshot().get(), retained.get());
}

BOOST_AUTO_TEST_CASE(provider_rotation_keeps_a_live_native_stream_on_its_original_snapshot) {
   const auto identity = make_identity_material();
   auto server_context = server_options(identity);
   auto provider = forge::net::tls::context_provider{server_context};
   auto original = provider.snapshot();
   const auto original_lifetime = std::weak_ptr<const forge::net::tls::context_snapshot>{original};
   auto io = asio::io_context{};
   auto listener = asio::ip::tcp::acceptor{io, {asio::ip::tcp::v4(), 0}};
   auto stream_ready = std::promise<void>{};
   auto ready = stream_ready.get_future();
   auto server_error = std::exception_ptr{};

   auto server = std::thread{[&listener, &stream_ready, &server_error, original = std::move(original)]() mutable {
      try {
         auto socket = listener.accept();
         auto stream = forge::net::tls::make_asio_stream(std::move(original), std::move(socket));
         stream_ready.set_value();
         stream->handshake(asio::ssl::stream_base::server);
      } catch (...) {
         server_error = std::current_exception();
         try {
            stream_ready.set_exception(server_error);
         } catch (...) {
         }
      }
   }};

   auto socket = asio::ip::tcp::socket{io};
   socket.connect(listener.local_endpoint());
   ready.get();
   provider.replace(server_context);
   BOOST_CHECK(!original_lifetime.expired());

   auto client_options = forge::net::tls::context_options{};
   client_options.verification = forge::net::tls::peer_verification::none;
   client_options.use_default_verify_paths = false;
   const auto client_context = forge::net::tls::make_context(std::move(client_options));
   auto client = forge::net::tls::make_asio_stream(client_context, std::move(socket));
   client->handshake(asio::ssl::stream_base::client);

   auto ignored = boost::system::error_code{};
   client->shutdown(ignored);
   client->lowest_layer().close(ignored);
   server.join();

   BOOST_CHECK(!server_error);
   BOOST_CHECK(original_lifetime.expired());
}

BOOST_AUTO_TEST_CASE(rejected_rotation_preserves_the_published_snapshot) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;

   auto provider = forge::net::tls::context_provider{options};
   const auto first = provider.snapshot();

   auto invalid = options;
   invalid.role = forge::net::tls::endpoint_role::server;
   BOOST_CHECK_THROW(provider.replace(std::move(invalid)), forge::net::tls::exceptions::invalid_options);
   BOOST_CHECK_EQUAL(provider.snapshot().get(), first.get());
}

BOOST_AUTO_TEST_CASE(prepared_exchange_rejects_null_without_changing_current) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   auto provider = forge::net::tls::context_provider{options};
   const auto current = provider.snapshot();
   BOOST_CHECK_THROW(static_cast<void>(provider.exchange({})), forge::net::tls::exceptions::invalid_options);
   BOOST_CHECK_EQUAL(provider.snapshot().get(), current.get());
}

BOOST_AUTO_TEST_CASE(prepared_exchange_preserves_identity_and_returns_retired_owner) {
   auto options = forge::net::tls::context_options{};
   options.verification = forge::net::tls::peer_verification::none;
   options.use_default_verify_paths = false;
   auto provider = forge::net::tls::context_provider{options};
   auto current = provider.snapshot();
   const auto* previous = current.get();
   auto lifetime = std::weak_ptr<const forge::net::tls::context_snapshot>{current};
   current.reset();
   auto prepared = forge::net::tls::make_context(options);
   const auto* next = prepared.get();
   auto retired = provider.exchange(std::move(prepared));
   BOOST_CHECK_EQUAL(provider.snapshot().get(), next);
   BOOST_CHECK_EQUAL(retired.get(), previous);
   BOOST_CHECK(!lifetime.expired());
   retired.reset();
   BOOST_CHECK(lifetime.expired());
   BOOST_CHECK_EQUAL(provider.snapshot().get(), next);
}

BOOST_AUTO_TEST_SUITE_END()
