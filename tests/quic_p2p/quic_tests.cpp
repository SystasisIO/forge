#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "../../libraries/net/quic/details/acknowledged_ranges.hxx"
#include "../../libraries/net/quic/details/client_token_cache.hxx"
#include "../../libraries/net/quic/details/initial_token.hxx"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.quic.connection;
import forge.net.quic.connector;
import forge.net.quic.endpoint;
import forge.net.quic.exceptions;
import forge.net.quic.framed_stream;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.runtime;
import forge.net.quic.security;
import forge.net.quic.stream;
import forge.net.quic.transport;
import forge.net.transport.buffer;
import forge.net.transport.connector;
import forge.net.transport.endpoint;
import forge.net.transport.listener;

namespace forge::net::quic {
namespace {

using udp = boost::asio::ip::udp;

constexpr auto initial_token_lifetime = 10 * NGTCP2_SECONDS;
constexpr auto regular_token_lifetime = 60 * 60 * NGTCP2_SECONDS;
constexpr auto initial_token_timestamp = ngtcp2_tstamp{3'600 * NGTCP2_SECONDS};

[[nodiscard]] detail::initial_token_validator::secret initial_token_secret(std::uint8_t seed = 0x10U) {
   auto value = detail::initial_token_validator::secret{};
   for (auto index = std::size_t{0}; index < value.size(); ++index) {
      value[index] = static_cast<std::uint8_t>(seed + index);
   }
   return value;
}

[[nodiscard]] ngtcp2_cid initial_token_cid(std::uint8_t seed) {
   auto value = ngtcp2_cid{};
   value.datalen = 8;
   for (auto index = std::size_t{0}; index < value.datalen; ++index) {
      value.data[index] = static_cast<std::uint8_t>(seed + index);
   }
   return value;
}

[[nodiscard]] ngtcp2_sockaddr_in initial_token_address(std::uint32_t address, std::uint16_t port) {
   auto value = ngtcp2_sockaddr_in{};
   value.sin_family = NGTCP2_AF_INET;
   value.sin_port = port;
   value.sin_addr.s_addr = address;
   return value;
}

[[nodiscard]] detail::initial_token_remote_address initial_token_remote(const ngtcp2_sockaddr_in& address) {
   return {
       .address = reinterpret_cast<const ngtcp2_sockaddr*>(&address),
       .length = static_cast<ngtcp2_socklen>(sizeof(address)),
   };
}

[[nodiscard]] std::vector<std::uint8_t>
generate_regular_initial_token(const detail::initial_token_validator::secret& secret,
                               detail::initial_token_remote_address remote,
                               ngtcp2_tstamp now = initial_token_timestamp) {
   auto token = std::vector<std::uint8_t>(NGTCP2_CRYPTO_MAX_REGULAR_TOKENLEN);
   const auto length = ngtcp2_crypto_generate_regular_token(token.data(), secret.data(), secret.size(), remote.address,
                                                            remote.length, now);
   BOOST_REQUIRE(length > 0);
   token.resize(static_cast<std::size_t>(length));
   return token;
}

[[nodiscard]] std::vector<std::uint8_t>
generate_legacy_retry_token(const detail::initial_token_validator::secret& secret,
                            detail::initial_token_remote_address remote, const ngtcp2_cid& retry_scid,
                            const ngtcp2_cid& original_dcid, ngtcp2_tstamp now = initial_token_timestamp) {
   auto token = std::vector<std::uint8_t>(NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN);
   const auto length =
       ngtcp2_crypto_generate_retry_token(token.data(), secret.data(), secret.size(), NGTCP2_PROTO_VER_V1,
                                          remote.address, remote.length, &retry_scid, &original_dcid, now);
   BOOST_REQUIRE(length > 0);
   token.resize(static_cast<std::size_t>(length));
   return token;
}

BOOST_AUTO_TEST_CASE(quic_acknowledged_ranges_require_complete_contiguous_coverage) {
   auto acknowledged = detail::acknowledged_ranges{};
   acknowledged.add(32, 32);
   BOOST_TEST(!acknowledged.covers(0, 32));
   BOOST_TEST(acknowledged.covers(32, 32));

   acknowledged.add(0, 16);
   BOOST_TEST(!acknowledged.covers(0, 32));
   acknowledged.add(16, 16);
   BOOST_TEST(acknowledged.covers(0, 64));

   acknowledged.discard_before(32);
   BOOST_TEST(!acknowledged.covers(0, 32));
   BOOST_TEST(acknowledged.covers(32, 32));
}

BOOST_AUTO_TEST_CASE(quic_initial_token_unknown_opaque_token_retries) {
   auto validator =
       detail::initial_token_validator{initial_token_secret(), initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto opaque = std::vector<std::uint8_t>(78, 0xA5U);
   const auto result = validator.validate(opaque, NGTCP2_PROTO_VER_V1, initial_token_remote(remote),
                                          initial_token_cid(0x31U), initial_token_timestamp);

   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::retry));
}

BOOST_AUTO_TEST_CASE(quic_initial_token_retry2_accepts_and_extracts_original_dcid) {
   auto validator =
       detail::initial_token_validator{initial_token_secret(), initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto retry_scid = initial_token_cid(0x21U);
   const auto original_dcid = initial_token_cid(0x11U);
   const auto token = validator.generate_retry(NGTCP2_PROTO_VER_V1, initial_token_remote(remote), retry_scid,
                                               original_dcid, initial_token_timestamp);
   BOOST_REQUIRE(token.has_value());
   BOOST_REQUIRE(!token->empty());
   BOOST_TEST(token->front() == NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2);

   const auto result = validator.validate(*token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), retry_scid,
                                          initial_token_timestamp);
   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::accept));
   BOOST_TEST(static_cast<int>(result.token_type) == static_cast<int>(NGTCP2_TOKEN_TYPE_RETRY));
   BOOST_TEST(ngtcp2_cid_eq(&result.original_dcid, &original_dcid) != 0);
}

BOOST_AUTO_TEST_CASE(quic_initial_token_foreign_retry2_is_unreadable_and_retries) {
   auto validator =
       detail::initial_token_validator{initial_token_secret(), initial_token_lifetime, regular_token_lifetime};
   auto foreign =
       detail::initial_token_validator{initial_token_secret(0x70U), initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto retry_scid = initial_token_cid(0x21U);
   const auto token = foreign.generate_retry(NGTCP2_PROTO_VER_V1, initial_token_remote(remote), retry_scid,
                                             initial_token_cid(0x11U), initial_token_timestamp);
   BOOST_REQUIRE(token.has_value());
   BOOST_TEST(token->front() == NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2);

   const auto result = validator.validate(*token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), retry_scid,
                                          initial_token_timestamp);
   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::retry));
}

BOOST_AUTO_TEST_CASE(quic_initial_token_legacy_retry_accepts_and_extracts_original_dcid) {
   const auto secret = initial_token_secret();
   auto validator = detail::initial_token_validator{secret, initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto retry_scid = initial_token_cid(0x21U);
   const auto original_dcid = initial_token_cid(0x11U);
   const auto token = generate_legacy_retry_token(secret, initial_token_remote(remote), retry_scid, original_dcid);
   BOOST_REQUIRE(!token.empty());
   BOOST_TEST(token.front() == NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY);

   const auto result = validator.validate(token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), retry_scid,
                                          initial_token_timestamp);
   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::accept));
   BOOST_TEST(static_cast<int>(result.token_type) == static_cast<int>(NGTCP2_TOKEN_TYPE_RETRY));
   BOOST_TEST(ngtcp2_cid_eq(&result.original_dcid, &original_dcid) != 0);
}

BOOST_AUTO_TEST_CASE(quic_initial_token_invalid_legacy_retry_is_unvalidated_and_retries) {
   auto validator =
       detail::initial_token_validator{initial_token_secret(), initial_token_lifetime, regular_token_lifetime};
   const auto foreign_secret = initial_token_secret(0x70U);
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto retry_scid = initial_token_cid(0x21U);
   const auto token =
       generate_legacy_retry_token(foreign_secret, initial_token_remote(remote), retry_scid, initial_token_cid(0x11U));
   BOOST_REQUIRE(!token.empty());
   BOOST_TEST(token.front() == NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY);

   const auto result = validator.validate(token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), retry_scid,
                                          initial_token_timestamp);
   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::retry));
}

BOOST_AUTO_TEST_CASE(quic_initial_token_retry2_wrong_address_and_expiry_reject) {
   auto validator =
       detail::initial_token_validator{initial_token_secret(), initial_token_lifetime, regular_token_lifetime};
   const auto original_remote = initial_token_address(0x0100007FU, 4433);
   const auto changed_remote = initial_token_address(0x0200007FU, 4433);
   const auto retry_scid = initial_token_cid(0x21U);
   const auto token = validator.generate_retry(NGTCP2_PROTO_VER_V1, initial_token_remote(original_remote), retry_scid,
                                               initial_token_cid(0x11U), initial_token_timestamp);
   BOOST_REQUIRE(token.has_value());

   const auto wrong_address = validator.validate(*token, NGTCP2_PROTO_VER_V1, initial_token_remote(changed_remote),
                                                 retry_scid, initial_token_timestamp);
   BOOST_TEST(static_cast<int>(wrong_address.disposition) ==
              static_cast<int>(detail::initial_token_disposition::reject_invalid));

   const auto expired = validator.validate(*token, NGTCP2_PROTO_VER_V1, initial_token_remote(original_remote),
                                           retry_scid, initial_token_timestamp + initial_token_lifetime + 1);
   BOOST_TEST(static_cast<int>(expired.disposition) ==
              static_cast<int>(detail::initial_token_disposition::reject_invalid));
}

BOOST_AUTO_TEST_CASE(quic_initial_token_regular_token_accepts_new_token) {
   const auto secret = initial_token_secret();
   auto validator = detail::initial_token_validator{secret, initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto current_dcid = initial_token_cid(0x41U);
   const auto token = generate_regular_initial_token(secret, initial_token_remote(remote));
   BOOST_REQUIRE(!token.empty());
   BOOST_TEST(token.front() == NGTCP2_CRYPTO_TOKEN_MAGIC_REGULAR);

   const auto result = validator.validate(token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), current_dcid,
                                          initial_token_timestamp);
   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::accept));
   BOOST_TEST(static_cast<int>(result.token_type) == static_cast<int>(NGTCP2_TOKEN_TYPE_NEW_TOKEN));
   BOOST_TEST(ngtcp2_cid_eq(&result.original_dcid, &current_dcid) != 0);
}

BOOST_AUTO_TEST_CASE(quic_initial_token_invalid_regular_token_retries) {
   auto validator =
       detail::initial_token_validator{initial_token_secret(), initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto token = generate_regular_initial_token(initial_token_secret(0x70U), initial_token_remote(remote));
   BOOST_REQUIRE(!token.empty());
   BOOST_TEST(token.front() == NGTCP2_CRYPTO_TOKEN_MAGIC_REGULAR);

   const auto result = validator.validate(token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote),
                                          initial_token_cid(0x41U), initial_token_timestamp);
   BOOST_TEST(static_cast<int>(result.disposition) == static_cast<int>(detail::initial_token_disposition::retry));
}

BOOST_AUTO_TEST_CASE(quic_initial_token_regular_token_uses_independent_lifetime) {
   const auto secret = initial_token_secret();
   auto validator = detail::initial_token_validator{secret, initial_token_lifetime, regular_token_lifetime};
   const auto remote = initial_token_address(0x0100007FU, 4433);
   const auto token = validator.generate_regular(initial_token_remote(remote), initial_token_timestamp);
   BOOST_REQUIRE(token.has_value());

   const auto accepted =
       validator.validate(*token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), initial_token_cid(0x41U),
                          initial_token_timestamp + initial_token_lifetime + 1);
   BOOST_TEST(static_cast<int>(accepted.disposition) == static_cast<int>(detail::initial_token_disposition::accept));
   const auto expired =
       validator.validate(*token, NGTCP2_PROTO_VER_V1, initial_token_remote(remote), initial_token_cid(0x41U),
                          initial_token_timestamp + regular_token_lifetime + 1);
   BOOST_TEST(static_cast<int>(expired.disposition) == static_cast<int>(detail::initial_token_disposition::retry));
}

BOOST_AUTO_TEST_CASE(quic_client_token_cache_is_atomic_bounded_and_expires) {
   auto limits = detail::client_token_cache::limits{
       .max_entries = 2,
       .max_token_bytes = 8,
       .max_key_bytes = 8,
       .max_raw_bytes = 128,
       .max_seen_digests = 3,
   };
   auto cache = detail::client_token_cache{limits};
   const auto now = detail::client_token_cache::clock::time_point{};
   BOOST_REQUIRE(cache.store("peer-a", {1, 2, 3}, now));
   BOOST_REQUIRE(cache.store("peer-b", {4, 5, 6}, now));
   BOOST_TEST(!cache.store("peer-c", {1, 2, 3}, now));

   auto taken = std::atomic_size_t{0};
   auto first = std::thread{[&] {
      if (cache.take("peer-a", now)) {
         taken.fetch_add(1, std::memory_order_relaxed);
      }
   }};
   auto second = std::thread{[&] {
      if (cache.take("peer-a", now)) {
         taken.fetch_add(1, std::memory_order_relaxed);
      }
   }};
   first.join();
   second.join();
   BOOST_TEST(taken.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(cache.take("peer-b", now).has_value());

   BOOST_REQUIRE(cache.store("peer-c", {7, 8, 9}, now));
   const auto active = cache.snapshot(now);
   BOOST_TEST(active.entries <= limits.max_entries);
   BOOST_TEST(active.seen_digests <= limits.max_seen_digests);
   BOOST_TEST(active.raw_bytes <= limits.max_raw_bytes);
   BOOST_TEST(!cache.take("peer-c", now + std::chrono::minutes{55}));
   const auto expired = cache.snapshot(now + std::chrono::minutes{55});
   BOOST_TEST(expired.entries == 0U);
   BOOST_TEST(expired.seen_digests == 0U);
   BOOST_TEST(expired.raw_bytes == 0U);
}

BOOST_AUTO_TEST_CASE(quic_client_token_cache_rejects_entry_that_cannot_fit_raw_limit) {
   auto cache = detail::client_token_cache{detail::client_token_cache::limits{
       .max_entries = 1,
       .max_token_bytes = 8,
       .max_key_bytes = 8,
       .max_raw_bytes = 8,
       .max_seen_digests = 1,
   }};
   const auto now = detail::client_token_cache::clock::time_point{};
   BOOST_TEST(!cache.store("key", {1, 2, 3, 4}, now));
   const auto state = cache.snapshot(now);
   BOOST_TEST(state.entries == 0U);
   BOOST_TEST(state.seen_digests == 0U);
   BOOST_TEST(state.raw_bytes == 0U);
}

BOOST_AUTO_TEST_CASE(quic_client_token_callbacks_support_default_custom_and_disabled_states) {
   auto options = client_options{.security = security_options{.verify_peer = false}};
   BOOST_CHECK_NO_THROW(validate(options));

   options.client_tokens = client_token_callbacks{};
   BOOST_CHECK_NO_THROW(validate(options));

   options.client_tokens = client_token_callbacks{
       .take = [] { return std::optional<std::vector<std::uint8_t>>{}; },
   };
   BOOST_CHECK_THROW(validate(options), forge::exceptions::base);

   options.client_tokens = client_token_callbacks{
       .take = [] { return std::optional<std::vector<std::uint8_t>>{}; },
       .store = [](std::vector<std::uint8_t>) {},
   };
   BOOST_CHECK_NO_THROW(validate(options));
}

std::string_view test_certificate() {
   return "-----BEGIN CERTIFICATE-----\n"
          "MIICpDCCAYwCCQCJjaEDxrQqBzANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAkx\n"
          "MjcuMC4wLjEwHhcNMjYwNDI5MDgwMTMzWhcNMjYwNDMwMDgwMTMzWjAUMRIwEAYD\n"
          "VQQDDAkxMjcuMC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDy\n"
          "sbPH/R4QUz725sY376knXjSDCA+O5+Udwqfl4qaXHTAooWfplVY/WFRCnnMV6+TX\n"
          "gl9tHkNpKmI92s4O/LuJ5xnCCPX8k5i70gSnaGpClYSx+0gix8QgddDDsbLbIU/+\n"
          "x7MRWXfKYd/ArGNelPMadlvmcoEhumVUAwjYSV26GhNAmUacJlho3ltyujYSGFOS\n"
          "lI/lDqIjZxo7jbAGMMpiyu1omQ5nxjTm+bfOTcksBRMQP8mDz0vYXHXirA+xDfuv\n"
          "M+mTj6eO4UQ42w+iVLqhSPEhfLURmR4NULtPmq9hT7d1wS/Ys9q4Hj/j+kcXRCXj\n"
          "nPOZzBinLRTDnE59HbDZAgMBAAEwDQYJKoZIhvcNAQELBQADggEBAHSOUQTEDgjC\n"
          "uwza9ayfThJTs43j+TziWHLlowqCiHt/ipRNFEW7L0ibTnbMdQBFGfaLkTAhc5Rd\n"
          "6O6x+9o76pgEYxEg0rDkgNXmprNmS+nL7Are+iiF6R+X8dts3MQgtONPApAXE96P\n"
          "/n5K4GDQTd3WCI37hkmJA6rmwziFDTlwqtKWts39g8PqAbXac27rVR/iD0gWdOws\n"
          "qiaoGj/0WW9qcgjYGdCc0/CbbnyiWbi48VVf0yyfm7wgcz90byaKIQchHdb/qjyU\n"
          "wy7nfU5TJ5MKQ5yeqPTWmPYZZp9TKa5VD6wZD/IH7jH3GdJ/fSyroVLZktVnmxJa\n"
          "dmG/9wwivwQ=\n"
          "-----END CERTIFICATE-----\n";
}

std::string_view test_private_key() {
   return "-----BEGIN PRIVATE KEY-----\n"
          "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDysbPH/R4QUz72\n"
          "5sY376knXjSDCA+O5+Udwqfl4qaXHTAooWfplVY/WFRCnnMV6+TXgl9tHkNpKmI9\n"
          "2s4O/LuJ5xnCCPX8k5i70gSnaGpClYSx+0gix8QgddDDsbLbIU/+x7MRWXfKYd/A\n"
          "rGNelPMadlvmcoEhumVUAwjYSV26GhNAmUacJlho3ltyujYSGFOSlI/lDqIjZxo7\n"
          "jbAGMMpiyu1omQ5nxjTm+bfOTcksBRMQP8mDz0vYXHXirA+xDfuvM+mTj6eO4UQ4\n"
          "2w+iVLqhSPEhfLURmR4NULtPmq9hT7d1wS/Ys9q4Hj/j+kcXRCXjnPOZzBinLRTD\n"
          "nE59HbDZAgMBAAECggEBAIWVjHhy+V5RA+JRCh/12ayirNLG2BF30OP9pf7iL4IT\n"
          "/dMPbKvkmDGLw+1bW8tgKXj5+N6N/trfCm4zhqI3OF7ihooH9qYM88/F/OvMjFiU\n"
          "BhMVVhJW1LxtPPjKUcFN58M8VnMhRM9v6gIaoSOJZvpU1abVtgBDocyJUxAB6gYp\n"
          "i7MzoRwHGsL5mW/luE5H92/S8NNwLWBDA7DIGfrTZ6POf92h5I5W3CuTcqR5FICz\n"
          "3pfU3i443yZmsmkc9duH2gZ9cb9j4pRtNLbbsGmRVrBlgnkVFk8JWbikc8MpLeKO\n"
          "VKP7A2NvxJIrc7oFYrf4hbw8P70YL7S9B3W3yBPPzJECgYEA+Y3nG8CtvVTE/Keo\n"
          "qb5Rljlnj9DEffrylLyYUYfSSNR4Olc2WCPBiz0rPCDdO0VGeXAwqLf2VP7IEyAx\n"
          "kvrnqhzHWMhiLv+k4tIVyKCwpuofN0JsoUCi7CwRf+H2Pg+t6ewLV116THKsd41H\n"
          "IRElWyEvZsmbbhlLrsxUtfFZWnUCgYEA+PZwXUn+cb8kRmfG959gMawTtcfvnBUX\n"
          "sIn7LQl/ZWUIiLMWCaS3FbqkiGjaEYo6om1invYNJNA9zp/ECauSDp58NICCL0ie\n"
          "L7z26sEa6Ocg2VdR4ezpN3cM6dyAKfTFGb9V6qjyqNIPCE4eey6ZJ+CU/mpEfSDu\n"
          "+RGMzfdDCFUCgYEA5FRUn0zk6jU0YyMXq+9pgLSXL7vI/Kdt6m7AQuCto1tbga2o\n"
          "GG7mt/pIo6RCJufUemoO62AeL1hKQU2UbjHJYxkfv/jf9LaM68dijQWRe7b8xres\n"
          "4sFcEBCmFkbt4YzBCCWjntT1gBrv+Ba4fOXOMxoi374Yy1yzpYRpAWuI4L0CgYAn\n"
          "u1SlXrivuHx2i/tR62pzou2mVhkkRK16LBsczeY57UzWXBZJRbM+UYIOjwU2RWQk\n"
          "JebWTZg9ZspmXlLv5CS0FpDl5BhiqWktXy/cuSKtRq2UYf4cWy3A/0vdSqZdi8Wk\n"
          "3Uc94uaPEK77eVQd/orMtWexzo3NlmLs9uMMv8g/3QKBgQCbik0UoJkkqNRMmWG8\n"
          "dKQzj58eRI8fmKdJlWNfj2QMspd2vXMbsWYgAbFbU1QcVs1n8PxNydM+cfy77w8q\n"
          "NWMlYP7rUFQ3ekYWqrRlshZdJ/h24PALd1nPCvhc4C9dvn+zW3BLVez1lBuFO8n8\n"
          "0YkgmTgW7Ieibqnf4DqYp//nkw==\n"
          "-----END PRIVATE KEY-----\n";
}

struct bio_deleter {
   void operator()(BIO* bio) const noexcept {
      BIO_free(bio);
   }
};

struct x509_deleter {
   void operator()(X509* certificate) const noexcept {
      X509_free(certificate);
   }
};

struct evp_pkey_deleter {
   void operator()(EVP_PKEY* key) const noexcept {
      EVP_PKEY_free(key);
   }
};

struct x509_extension_deleter {
   void operator()(X509_EXTENSION* extension) const noexcept {
      X509_EXTENSION_free(extension);
   }
};

struct generated_identity {
   std::string certificate_pem;
   std::string private_key_pem;
};

std::string bio_to_string(BIO* bio) {
   BUF_MEM* memory = nullptr;
   BIO_get_mem_ptr(bio, &memory);
   BOOST_REQUIRE(memory != nullptr);
   return std::string{memory->data, memory->length};
}

void add_certificate_extension(X509* certificate, int nid, std::string_view value) {
   auto context = X509V3_CTX{};
   X509V3_set_ctx_nodb(&context);
   X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
   auto extension = std::unique_ptr<X509_EXTENSION, x509_extension_deleter>{
       X509V3_EXT_conf_nid(nullptr, &context, nid, std::string{value}.c_str())};
   BOOST_REQUIRE(extension != nullptr);
   BOOST_REQUIRE(X509_add_ext(certificate, extension.get(), -1) == 1);
}

generated_identity generate_test_identity(std::string_view subject_alt_name) {
   auto key = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>{EVP_RSA_gen(2048)};
   BOOST_REQUIRE(key != nullptr);
   auto certificate = std::unique_ptr<X509, x509_deleter>{X509_new()};
   BOOST_REQUIRE(certificate != nullptr);

   BOOST_REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
   BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
   BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60 * 60) != nullptr);
   BOOST_REQUIRE(X509_set_pubkey(certificate.get(), key.get()) == 1);

   auto* name = X509_get_subject_name(certificate.get());
   BOOST_REQUIRE(name != nullptr);
   const auto common_name = std::string{"forge-quic-test"};
   BOOST_REQUIRE(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                            reinterpret_cast<const unsigned char*>(common_name.data()),
                                            static_cast<int>(common_name.size()), -1, 0) == 1);
   BOOST_REQUIRE(X509_set_issuer_name(certificate.get(), name) == 1);
   add_certificate_extension(certificate.get(), NID_basic_constraints, "critical,CA:TRUE");
   add_certificate_extension(certificate.get(), NID_subject_alt_name, subject_alt_name);
   BOOST_REQUIRE(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);

   auto certificate_bio = std::unique_ptr<BIO, bio_deleter>{BIO_new(BIO_s_mem())};
   auto private_key_bio = std::unique_ptr<BIO, bio_deleter>{BIO_new(BIO_s_mem())};
   BOOST_REQUIRE(certificate_bio != nullptr);
   BOOST_REQUIRE(private_key_bio != nullptr);
   BOOST_REQUIRE(PEM_write_bio_X509(certificate_bio.get(), certificate.get()) == 1);
   BOOST_REQUIRE(PEM_write_bio_PrivateKey(private_key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ==
                 1);
   return generated_identity{
       .certificate_pem = bio_to_string(certificate_bio.get()),
       .private_key_pem = bio_to_string(private_key_bio.get()),
   };
}

std::vector<std::uint8_t> test_certificate_der() {
   auto bio = std::unique_ptr<BIO, bio_deleter>{
       BIO_new_mem_buf(test_certificate().data(), static_cast<int>(test_certificate().size()))};
   BOOST_REQUIRE(bio != nullptr);
   auto certificate = std::unique_ptr<X509, x509_deleter>{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
   BOOST_REQUIRE(certificate != nullptr);

   const auto len = i2d_X509(certificate.get(), nullptr);
   BOOST_REQUIRE(len > 0);
   auto der = std::vector<std::uint8_t>(static_cast<std::size_t>(len));
   auto* out = der.data();
   BOOST_REQUIRE(i2d_X509(certificate.get(), &out) == len);
   return der;
}

server_options loopback_server_options(std::string alpn = "forge-p2p/1", transport_limits limits = {}) {
   return server_options{
       .alpn = std::move(alpn),
       .limits = limits,
       .security = security_options{.verify_peer = false},
       .certificate_pem = std::string{test_certificate()},
       .private_key_pem = std::string{test_private_key()},
   };
}

client_options loopback_client_options(std::string alpn = "forge-p2p/1", transport_limits limits = {}) {
   return client_options{
       .alpn = std::move(alpn),
       .handshake_timeout = std::chrono::milliseconds{5'000},
       .limits = limits,
       .security = security_options{.verify_peer = false},
   };
}

udp::endpoint to_udp_endpoint(const endpoint& value) {
   return udp::endpoint{boost::asio::ip::make_address(value.host), value.port};
}

endpoint to_quic_endpoint(const udp::endpoint& value) {
   using address_member = boost::asio::ip::address (udp::endpoint::*)() const;
   const auto host = std::invoke(static_cast<address_member>(&udp::endpoint::address), value).to_string();
   return endpoint{.host = host, .port = value.port()};
}

template <typename T>
T get_with_deadline(std::future<T>& future, std::chrono::milliseconds timeout, std::string_view label) {
   if (future.wait_for(timeout) != std::future_status::ready) {
      BOOST_FAIL(std::string{"timed out waiting for "} + std::string{label});
      throw std::runtime_error{std::string{"timed out waiting for "} + std::string{label}};
   }
   return future.get();
}

void get_with_deadline(std::future<void>& future, std::chrono::milliseconds timeout, std::string_view label) {
   if (future.wait_for(timeout) != std::future_status::ready) {
      BOOST_FAIL(std::string{"timed out waiting for "} + std::string{label});
      throw std::runtime_error{std::string{"timed out waiting for "} + std::string{label}};
   }
   future.get();
}

template <typename T>
T get_with_deadline_or_stop(forge::asio::runtime& runtime, std::future<T>& future, std::chrono::milliseconds timeout,
                            std::string_view label) {
   if (future.wait_for(timeout) != std::future_status::ready) {
      runtime.stop();
      BOOST_FAIL(std::string{"timed out waiting for "} + std::string{label});
      throw std::runtime_error{std::string{"timed out waiting for "} + std::string{label}};
   }
   return future.get();
}

template <typename Awaitable>
auto run_with_deadline(forge::asio::runtime& runtime, Awaitable&& awaitable, std::chrono::milliseconds timeout,
                       std::string_view label) {
   auto future = boost::asio::co_spawn(runtime.context(), std::forward<Awaitable>(awaitable), boost::asio::use_future);
   return get_with_deadline_or_stop(runtime, future, timeout, label);
}

struct fault_rule {
   std::uint32_t drop_every = 0;
   std::uint32_t drop_after = 0;
   std::uint32_t duplicate_every = 0;
   std::uint32_t duplicate_after = 0;
   std::uint32_t reorder_every = 0;
   std::uint32_t reorder_after = 0;
   std::chrono::milliseconds delay{0};
};

struct fault_proxy_rules {
   fault_rule client_to_server;
   fault_rule server_to_client;
};

struct fault_direction_metrics {
   std::uint64_t received = 0;
   std::uint64_t sent = 0;
   std::uint64_t dropped = 0;
   std::uint64_t duplicated = 0;
   std::uint64_t delayed = 0;
   std::uint64_t reordered = 0;
};

struct fault_proxy_metrics {
   fault_direction_metrics client_to_server;
   fault_direction_metrics server_to_client;
};

class udp_fault_proxy : public std::enable_shared_from_this<udp_fault_proxy> {
 public:
   udp_fault_proxy(boost::asio::io_context& context, endpoint server_endpoint, fault_proxy_rules rules)
       : strand_{boost::asio::make_strand(context)},
         socket_(strand_, udp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}),
         server_endpoint_(to_udp_endpoint(server_endpoint)), rules_(rules) {}

   ~udp_fault_proxy() {
      stopped_.store(true, std::memory_order_release);
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      socket_.close(ignored);
   }

   udp_fault_proxy(const udp_fault_proxy&) = delete;
   udp_fault_proxy& operator=(const udp_fault_proxy&) = delete;

   [[nodiscard]] endpoint local_endpoint() const {
      return to_quic_endpoint(socket_.local_endpoint());
   }

   [[nodiscard]] fault_proxy_metrics metrics() const {
      return metrics_;
   }

   [[nodiscard]] std::uint64_t server_packets_received() const noexcept {
      return server_packets_received_.load(std::memory_order_acquire);
   }

   [[nodiscard]] std::uint64_t server_packets_held() const noexcept {
      return server_packets_held_.load(std::memory_order_acquire);
   }

   void hold_server_to_client() noexcept {
      hold_server_to_client_.store(true, std::memory_order_release);
   }

   void release_server_to_client() {
      auto self = shared_from_this();
      boost::asio::dispatch(strand_, [self = std::move(self)] {
         self->hold_server_to_client_.store(false, std::memory_order_release);
         auto held = std::move(self->held_server_to_client_);
         self->held_server_to_client_.clear();
         for (auto& packet_value : held) {
            self->send_now(std::move(packet_value), self->metrics_.server_to_client);
         }
      });
   }

   void drop_next_client_to_server() noexcept {
      drop_next_client_to_server_.store(true, std::memory_order_release);
   }

   void start() {
      do_receive();
   }

   void stop() {
      if (stopped_.exchange(true, std::memory_order_acq_rel)) {
         return;
      }
      auto self = shared_from_this();
      boost::asio::dispatch(strand_, [self = std::move(self)] {
         boost::system::error_code ignored;
         self->socket_.cancel(ignored);
         self->socket_.close(ignored);
      });
   }

 private:
   struct packet {
      std::vector<std::uint8_t> bytes;
      udp::endpoint destination;
      bool to_server = true;
   };

   struct direction_state {
      std::uint64_t sequence = 0;
      std::optional<packet> reordered;
   };

   void do_receive() {
      auto self = shared_from_this();
      socket_.async_receive_from(boost::asio::buffer(buffer_), source_endpoint_,
                                 [self](boost::system::error_code ec, std::size_t bytes) {
                                    if (ec || self->stopped_.load(std::memory_order_acquire)) {
                                       return;
                                    }
                                    self->handle_packet(bytes);
                                    self->do_receive();
                                 });
   }

   void handle_packet(std::size_t bytes) {
      const auto from_server = source_endpoint_ == server_endpoint_;
      if (from_server) {
         server_packets_received_.fetch_add(1, std::memory_order_release);
      }
      if (!from_server) {
         client_endpoint_ = source_endpoint_;
         has_client_endpoint_ = true;
      }
      if (from_server && !has_client_endpoint_) {
         return;
      }

      auto packet_value = packet{
          .bytes = std::vector<std::uint8_t>{buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(bytes)},
          .destination = from_server ? client_endpoint_ : server_endpoint_,
          .to_server = !from_server,
      };
      route_packet(std::move(packet_value));
   }

   void route_packet(packet packet_value) {
      auto& state = packet_value.to_server ? client_to_server_ : server_to_client_;
      auto& metrics = packet_value.to_server ? metrics_.client_to_server : metrics_.server_to_client;
      const auto& rule = packet_value.to_server ? rules_.client_to_server : rules_.server_to_client;
      ++state.sequence;
      ++metrics.received;

      if (packet_value.to_server && drop_next_client_to_server_.exchange(false, std::memory_order_acq_rel)) {
         ++metrics.dropped;
         return;
      }

      if (!packet_value.to_server && hold_server_to_client_.load(std::memory_order_acquire)) {
         ++metrics.delayed;
         server_packets_held_.fetch_add(1, std::memory_order_release);
         held_server_to_client_.push_back(std::move(packet_value));
         return;
      }

      if (rule.drop_every > 0 && state.sequence > rule.drop_after && state.sequence % rule.drop_every == 0) {
         ++metrics.dropped;
         return;
      }

      const auto duplicate = rule.duplicate_every > 0 && state.sequence > rule.duplicate_after &&
                             state.sequence % rule.duplicate_every == 0;
      if (rule.reorder_every > 0 && state.sequence > rule.reorder_after && state.sequence % rule.reorder_every == 0) {
         if (!state.reordered) {
            ++metrics.reordered;
            state.reordered = std::move(packet_value);
            schedule_reorder_flush(state, metrics,
                                   rule.delay == std::chrono::milliseconds{0} ? std::chrono::milliseconds{5}
                                                                              : rule.delay * 2);
            return;
         }
      }

      send_or_delay(packet_value, rule, metrics);
      if (duplicate) {
         ++metrics.duplicated;
         send_or_delay(packet{.bytes = packet_value.bytes,
                              .destination = packet_value.destination,
                              .to_server = packet_value.to_server},
                       rule, metrics);
      }
      if (state.reordered) {
         auto held = std::move(*state.reordered);
         state.reordered.reset();
         send_or_delay(std::move(held), rule, metrics);
      }
   }

   void schedule_reorder_flush(direction_state& state, fault_direction_metrics& metrics,
                               std::chrono::milliseconds delay) {
      auto timer = std::make_shared<boost::asio::steady_timer>(socket_.get_executor());
      timer->expires_after(delay);
      auto self = shared_from_this();
      auto* state_ptr = &state;
      auto* metrics_ptr = &metrics;
      timer->async_wait([self, timer, state_ptr, metrics_ptr](boost::system::error_code ec) {
         if (ec || self->stopped_.load(std::memory_order_acquire) || !state_ptr->reordered) {
            return;
         }
         auto held = std::move(*state_ptr->reordered);
         state_ptr->reordered.reset();
         self->send_now(std::move(held), *metrics_ptr);
      });
   }

   void send_or_delay(packet packet_value, const fault_rule& rule, fault_direction_metrics& metrics) {
      if (rule.delay == std::chrono::milliseconds{0}) {
         send_now(std::move(packet_value), metrics);
         return;
      }
      ++metrics.delayed;
      auto timer = std::make_shared<boost::asio::steady_timer>(socket_.get_executor());
      timer->expires_after(rule.delay);
      auto self = shared_from_this();
      auto metrics_ptr = &metrics;
      timer->async_wait(
          [self, timer, packet_value = std::move(packet_value), metrics_ptr](boost::system::error_code ec) mutable {
             if (ec || self->stopped_.load(std::memory_order_acquire)) {
                return;
             }
             self->send_now(std::move(packet_value), *metrics_ptr);
          });
   }

   void send_now(packet packet_value, fault_direction_metrics& metrics) {
      auto self = shared_from_this();
      auto payload = std::make_shared<std::vector<std::uint8_t>>(std::move(packet_value.bytes));
      auto* metrics_ptr = &metrics;
      socket_.async_send_to(boost::asio::buffer(*payload), packet_value.destination,
                            [self, payload, metrics_ptr](boost::system::error_code ec, std::size_t) {
                               if (!ec) {
                                  ++metrics_ptr->sent;
                               }
                            });
   }

   boost::asio::strand<boost::asio::io_context::executor_type> strand_;
   udp::socket socket_;
   udp::endpoint server_endpoint_;
   udp::endpoint client_endpoint_;
   udp::endpoint source_endpoint_;
   bool has_client_endpoint_ = false;
   std::atomic_bool stopped_{false};
   std::array<std::uint8_t, 64 * 1024> buffer_{};
   fault_proxy_rules rules_;
   direction_state client_to_server_;
   direction_state server_to_client_;
   fault_proxy_metrics metrics_;
   std::vector<packet> held_server_to_client_;
   std::atomic_uint64_t server_packets_received_{0};
   std::atomic_uint64_t server_packets_held_{0};
   std::atomic_bool drop_next_client_to_server_{false};
   std::atomic_bool hold_server_to_client_{false};
};

boost::asio::awaitable<void> async_wait_for_new_token(connection& value) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   for (auto attempt = std::size_t{}; attempt < 100; ++attempt) {
      if (value.metrics().new_tokens_received != 0) {
         co_return;
      }
      timer.expires_after(std::chrono::milliseconds{10});
      auto error = boost::system::error_code{};
      co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      if (error) {
         throw std::runtime_error{"waiting for QUIC NEW_TOKEN was canceled"};
      }
   }
   throw std::runtime_error{"timed out waiting for QUIC NEW_TOKEN"};
}

BOOST_AUTO_TEST_CASE(quic_endpoint_parses_ipv4_authority) {
   const auto host = std::string{"127.0.0.1"};
   const auto authority = host + ":" + std::to_string(9443);
   const auto value = parse_endpoint(std::string{"quic"} + "://" + authority);

   BOOST_TEST(value.host == host);
   BOOST_TEST(value.port == 9443);
   BOOST_TEST(value.authority() == authority);
}

BOOST_AUTO_TEST_CASE(quic_endpoint_parses_bracketed_ipv6_authority) {
   const auto value = parse_endpoint(std::string{"quic"} + "://[::1]:" + std::to_string(9443));

   BOOST_TEST(value.host == "::1");
   BOOST_TEST(value.port == 9443);
}

BOOST_AUTO_TEST_CASE(quic_endpoint_rejects_non_quic_scheme) {
   try {
      (void)parse_endpoint(std::string{"https"} + "://" + std::string{"127.0.0.1"} + ":" + std::to_string(9443));
      BOOST_FAIL("expected typed QUIC exception");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::invalid_endpoint));
   }
}

BOOST_AUTO_TEST_CASE(quic_transport_roundtrips_dns_address_family) {
   const auto dns4 = forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::dns4,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = "peer.example.test",
       .port = 9443,
   };
   const auto dns6 = forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::dns6,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = "peer.example.test",
       .port = 9443,
   };

   const auto quic_dns4 = from_transport_endpoint(dns4);
   const auto quic_dns6 = from_transport_endpoint(dns6);
   BOOST_TEST(static_cast<int>(quic_dns4.family) == static_cast<int>(endpoint::address_family::ipv4));
   BOOST_TEST(static_cast<int>(quic_dns6.family) == static_cast<int>(endpoint::address_family::ipv6));
   BOOST_TEST(static_cast<int>(to_transport_endpoint(quic_dns4).host_type) == static_cast<int>(dns4.host_type));
   BOOST_TEST(static_cast<int>(to_transport_endpoint(quic_dns6).host_type) == static_cast<int>(dns6.host_type));

   const auto literal = endpoint{.host = "127.0.0.1", .port = 9443, .family = endpoint::address_family::ipv6};
   BOOST_TEST(static_cast<int>(to_transport_endpoint(literal).host_type) ==
              static_cast<int>(forge::net::transport::endpoint::host_kind::ip4));
}

BOOST_AUTO_TEST_CASE(quic_transport_dns4_connector_uses_ipv4_resolution) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "0.0.0.0", .port = 0}, loopback_server_options()};
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = make_session_connector(runtime, loopback_client_options());
   const auto remote = forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::dns4,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = boost::asio::ip::host_name(),
       .port = server.local_endpoint().port,
   };

   auto client_connection = run_with_deadline(runtime, client.async_connect(remote), std::chrono::milliseconds{5'000},
                                              "DNS4 QUIC transport connect");
   auto server_connection =
       get_with_deadline_or_stop(runtime, accepted, std::chrono::milliseconds{5'000}, "DNS4 QUIC transport accept");
   const auto selected = boost::asio::ip::make_address(client_connection.remote_endpoint.host);

   BOOST_TEST(static_cast<int>(client_connection.remote_endpoint.host_type) ==
              static_cast<int>(forge::net::transport::endpoint::host_kind::ip4));
   BOOST_TEST(selected.is_v4());

   run_with_deadline(runtime, client_connection.session.async_close(), std::chrono::milliseconds{5'000},
                     "DNS4 QUIC transport client close");
   run_with_deadline(runtime, server_connection.async_close(), std::chrono::milliseconds{5'000},
                     "DNS4 QUIC transport server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_connector_numeric_host_overrides_dns_family_constraint) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "0.0.0.0", .port = 0}, loopback_server_options()};
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   const auto remote = endpoint{
       .host = "127.0.0.1",
       .port = server.local_endpoint().port,
       .family = endpoint::address_family::ipv6,
   };

   auto client_connection = run_with_deadline(runtime, client.async_connect(remote, loopback_client_options()),
                                              std::chrono::milliseconds{5'000}, "numeric IPv4 QUIC connect");
   auto server_connection =
       get_with_deadline_or_stop(runtime, accepted, std::chrono::milliseconds{5'000}, "numeric IPv4 QUIC accept");
   BOOST_TEST(boost::asio::ip::make_address(client_connection.remote_endpoint().host).is_v4());

   run_with_deadline(runtime, client_connection.async_close(), std::chrono::milliseconds{5'000},
                     "numeric IPv4 QUIC client close");
   run_with_deadline(runtime, server_connection.async_close(), std::chrono::milliseconds{5'000},
                     "numeric IPv4 QUIC server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_connector_reuses_verified_new_token_without_retry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};

   auto first_accept = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto first = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()),
                                  std::chrono::milliseconds{5'000}, "initial tokenless connect");
   auto first_server = get_with_deadline(first_accept, std::chrono::milliseconds{5'000}, "initial token accept");
   run_with_deadline(runtime, async_wait_for_new_token(first), std::chrono::milliseconds{2'000}, "receive NEW_TOKEN");
   BOOST_TEST(first.metrics().retry_packets_received == 1U);
   BOOST_TEST(first_server.metrics().new_tokens_submitted == 1U);
   run_with_deadline(runtime, first.async_close(), std::chrono::milliseconds{5'000}, "initial token client close");
   run_with_deadline(runtime, first_server.async_close(), std::chrono::milliseconds{5'000},
                     "initial token server close");

   auto second_accept = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto second = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()),
                                   std::chrono::milliseconds{5'000}, "cached token connect");
   auto second_server = get_with_deadline(second_accept, std::chrono::milliseconds{5'000}, "cached token accept");
   BOOST_TEST(second.metrics().retry_packets_received == 0U);
   run_with_deadline(runtime, second.async_close(), std::chrono::milliseconds{5'000}, "cached token client close");
   run_with_deadline(runtime, second_server.async_close(), std::chrono::milliseconds{5'000},
                     "cached token server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_foreign_regular_token_retries_after_listener_restart) {
   struct token_state {
      std::mutex mutex;
      std::condition_variable changed;
      std::optional<std::vector<std::uint8_t>> token;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto state = std::make_shared<token_state>();
   const auto options_for = [state] {
      auto options = loopback_client_options();
      options.client_tokens = client_token_callbacks{
          .take =
              [state] {
                 auto lock = std::scoped_lock{state->mutex};
                 auto token = std::move(state->token);
                 state->token.reset();
                 return token;
              },
          .store =
              [state](std::vector<std::uint8_t> token) {
                 auto lock = std::scoped_lock{state->mutex};
                 state->token = std::move(token);
                 state->changed.notify_all();
              },
      };
      return options;
   };

   auto remote = endpoint{};
   {
      auto first_server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
      remote = first_server.local_endpoint();
      auto accepted = boost::asio::co_spawn(runtime.context(), first_server.async_accept(), boost::asio::use_future);
      auto client = connector{runtime};
      auto connection = run_with_deadline(runtime, client.async_connect(remote, options_for()),
                                          std::chrono::milliseconds{5'000}, "foreign token initial connect");
      auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "foreign token initial accept");
      {
         auto lock = std::unique_lock{state->mutex};
         BOOST_REQUIRE(
             state->changed.wait_for(lock, std::chrono::seconds{2}, [&] { return state->token.has_value(); }));
      }
      run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000},
                        "foreign token initial close");
      run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{5'000},
                        "foreign token initial inbound close");
      run_with_deadline(runtime, first_server.async_stop(), std::chrono::milliseconds{5'000},
                        "foreign token initial listener stop");

      auto replacement = listener{runtime, remote, loopback_server_options()};
      auto replacement_accept =
          boost::asio::co_spawn(runtime.context(), replacement.async_accept(), boost::asio::use_future);
      auto retrying = run_with_deadline(runtime, client.async_connect(remote, options_for()),
                                        std::chrono::milliseconds{5'000}, "foreign token retry connect");
      auto replacement_connection =
          get_with_deadline(replacement_accept, std::chrono::milliseconds{5'000}, "foreign token retry accept");
      BOOST_TEST(retrying.metrics().retry_packets_received == 1U);
      run_with_deadline(runtime, retrying.async_close(), std::chrono::milliseconds{5'000}, "foreign token retry close");
      run_with_deadline(runtime, replacement_connection.async_close(), std::chrono::milliseconds{5'000},
                        "foreign token retry inbound close");
      replacement.stop();
   }
}

BOOST_AUTO_TEST_CASE(quic_client_token_callback_failures_do_not_close_verified_connection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto options = loopback_client_options();
   options.client_tokens = client_token_callbacks{
       .take = []() -> std::optional<std::vector<std::uint8_t>> { throw std::runtime_error{"take failure"}; },
       .store = [](std::vector<std::uint8_t>) { throw std::runtime_error{"store failure"}; },
   };

   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "throwing token callback connect");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "throwing token callback accept");
   BOOST_TEST(connection.valid());
   BOOST_TEST(!connection.metrics().closed);
   BOOST_TEST(inbound.metrics().new_tokens_submitted == 1U);

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000}, "throwing callback close");
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{5'000},
                     "throwing callback inbound close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_failed_peer_verification_drops_pending_new_token) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto stores = std::atomic_size_t{0};
   auto options = loopback_client_options();
   options.security = security_options{
       .verify_peer = true,
       .expected_sha256_fingerprint = std::string(64, '0'),
   };
   options.client_tokens = client_token_callbacks{
       .take = [] { return std::optional<std::vector<std::uint8_t>>{}; },
       .store = [&stores](std::vector<std::uint8_t>) { stores.fetch_add(1, std::memory_order_relaxed); },
   };
   auto client = connector{runtime};
   try {
      (void)run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                              std::chrono::milliseconds{5'000}, "failed verified token connect");
      BOOST_FAIL("expected peer verification failure");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "failed verified token accept");
   BOOST_TEST(inbound.metrics().new_tokens_submitted == 1U);
   BOOST_TEST(stores.load(std::memory_order_relaxed) == 0U);
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{5'000},
                     "failed verified token inbound close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_transport_rejects_invalid_literal_host_kinds_before_io) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = make_session_connector(runtime, loopback_client_options());
   const auto expect_invalid_connect = [&](forge::net::transport::endpoint remote) {
      try {
         static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(std::move(remote))));
         BOOST_FAIL("expected typed QUIC transport endpoint failure");
      } catch (const forge::exceptions::base& error) {
         BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                    static_cast<int>(exceptions::code::invalid_endpoint));
      }
   };
   const auto expect_invalid_listener = [&](forge::net::transport::endpoint local) {
      try {
         static_cast<void>(make_session_listener(runtime, std::move(local), loopback_server_options()));
         BOOST_FAIL("expected typed QUIC transport listener endpoint failure");
      } catch (const forge::exceptions::base& error) {
         BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                    static_cast<int>(exceptions::code::invalid_endpoint));
      }
   };

   expect_invalid_connect(forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::ip4,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = "::1",
       .port = 9443,
   });
   expect_invalid_connect(forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::ip6,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = "127.0.0.1",
       .port = 9443,
   });
   expect_invalid_listener(forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::ip4,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = "not-an-ip-address",
       .port = 9443,
   });
   expect_invalid_listener(forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::ip6,
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = "not-an-ip-address",
       .port = 9443,
   });
}

BOOST_AUTO_TEST_CASE(quic_connect_timeout_wins_over_pre_connection_error_race) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = connector{runtime};
   auto options = loopback_client_options();
   options.connect_timeout = std::chrono::milliseconds{100};
   options.test_failpoint = [](std::string_view name) { return name == "timeout_before_pre_connection_error_finish"; };
   const auto started = std::chrono::steady_clock::now();

   try {
      (void)run_with_deadline(
          runtime, client.async_connect(endpoint{.host = "not a valid host name", .port = 443}, std::move(options)),
          std::chrono::milliseconds{2'000}, "pre-connection error timeout winner");
      BOOST_FAIL("expected QUIC connect timeout");
   } catch (const forge::exceptions::base& error) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
      BOOST_TEST(elapsed.count() < 1'000);
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::connect_timeout));
   }
}

BOOST_AUTO_TEST_CASE(quic_native_handshake_preserves_committed_terminal_winner) {
   struct lifetime_notification {
      std::promise<void> released;
      ~lifetime_notification() { released.set_value(); }
   };

   for (const auto inherited : {false, true}) {
      BOOST_TEST_CONTEXT((inherited ? "committed timeout before deferred inherited cancellation"
                                    : "committed connector cancellation before attempted timeout")) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
         auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
         auto client = connector{runtime};
         auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
         auto hook_calls = std::make_shared<std::atomic_size_t>(0);
         auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{&client, [&](void*) noexcept {
            server.stop();
            runtime.stop();
         }};
         auto lifetime = std::make_shared<lifetime_notification>();
         auto released = std::weak_ptr<lifetime_notification>{lifetime};
         auto terminal_release = lifetime->released.get_future();
         auto options = loopback_client_options();
         options.connect_timeout = std::chrono::seconds{10};
         options.handshake_timeout = std::chrono::seconds{10};
         options.connection_lifetime = std::move(lifetime);
         options.test_failpoint = [&client, cancellation, hook_calls, inherited](std::string_view name) {
            if (name != "timeout_before_pre_connection_error_finish") {
               return false;
            }
            hook_calls->fetch_add(1, std::memory_order_relaxed);
            if (inherited) {
               // Native co_spawn has completed: forwarding this cancellation
               // to active_connect is deferred until the engine's catch path.
               cancellation->emit(boost::asio::cancellation_type::terminal);
            } else {
               client.cancel();
            }
            return true;
         };
         auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
         auto pending = boost::asio::co_spawn(
             boost::asio::make_strand(runtime.context()),
             client.async_connect(server.local_endpoint(), std::move(options)),
             boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future));

         const auto expected = inherited ? exceptions::code::connect_timeout : exceptions::code::canceled;
         BOOST_CHECK_EXCEPTION(
             get_with_deadline(pending, std::chrono::seconds{5}, "committed native handshake terminal winner"),
             forge::exceptions::base, [expected](const auto& error) { return exceptions::code_of(error) == expected; });
         auto inbound = get_with_deadline(accepted, std::chrono::seconds{2}, "terminal winner native handshake accept");
         BOOST_TEST(inbound.valid());
         BOOST_TEST(inbound.metrics().handshakes_completed == 1U);
         BOOST_TEST(hook_calls->load(std::memory_order_relaxed) == 1U);
         BOOST_CHECK(terminal_release.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
         BOOST_TEST(released.expired());
         run_with_deadline(runtime, inbound.async_close(), std::chrono::seconds{2}, "close terminal winner peer");
      }
   }
}

BOOST_AUTO_TEST_CASE(quic_connect_awaits_dns_completion_after_inherited_cancellation) {
   struct resolution_barrier {
      std::mutex mutex;
      std::condition_variable entered_changed;
      std::condition_variable release_changed;
      bool entered = false;
      bool released = false;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = connector{runtime};
   auto barrier = std::make_shared<resolution_barrier>();
   auto options = loopback_client_options();
   options.connect_timeout = std::chrono::seconds{30};
   options.test_failpoint = [barrier](std::string_view name) {
      if (name != "after_resolution_completion") {
         return false;
      }
      auto lock = std::unique_lock{barrier->mutex};
      barrier->entered = true;
      barrier->entered_changed.notify_all();
      barrier->release_changed.wait(lock, [&] { return barrier->released; });
      return false;
   };
   const auto release_barrier = [](resolution_barrier* value) noexcept {
      auto lock = std::scoped_lock{value->mutex};
      value->released = true;
      value->release_changed.notify_all();
   };
   auto release_guard = std::unique_ptr<resolution_barrier, decltype(release_barrier)>{barrier.get(), release_barrier};
   auto cancellation = boost::asio::cancellation_signal{};
   auto pending = boost::asio::co_spawn(
       runtime.context(), client.async_connect(endpoint{.host = "127.0.0.1", .port = 443}, std::move(options)),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));

   {
      auto lock = std::unique_lock{barrier->mutex};
      BOOST_REQUIRE(barrier->entered_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return barrier->entered; }));
   }
   BOOST_CHECK(pending.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_CHECK(pending.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   release_guard.reset();
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(pending.get());
      BOOST_FAIL("expected inherited cancellation to win QUIC connect");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::canceled));
   }
}

BOOST_AUTO_TEST_CASE(quic_inherited_connect_cancellation_releases_native_lifetime_after_initial) {
   struct lifetime_notification {
      std::promise<void> released;
      ~lifetime_notification() { released.set_value(); }
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto strand = boost::asio::make_strand(runtime.context());
   auto blackhole = std::make_shared<udp::socket>(strand, udp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});
   auto packet = std::array<std::uint8_t, 65'536>{};
   auto source = udp::endpoint{};
   auto received = blackhole->async_receive_from(boost::asio::buffer(packet), source, boost::asio::use_future);
   auto client = connector{runtime};
   // Keep the runtime and silent peer alive throughout the assertions. On a
   // RED timeout, close/join our receive before stopping the runtime; never
   // wait for the long native handshake timeout or call connector.cancel().
   auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{blackhole.get(), [&](void*) noexcept {
      auto closed = std::make_shared<std::promise<void>>();
      auto closing = closed->get_future();
      boost::asio::post(strand, [blackhole, closed] {
         auto ignored = boost::system::error_code{};
         blackhole->close(ignored);
         closed->set_value();
      });
      if (closing.wait_for(std::chrono::seconds{2}) == std::future_status::ready && received.valid()) {
         static_cast<void>(received.wait_for(std::chrono::seconds{2}));
      }
      runtime.stop();
   }};
   auto lifetime = std::make_shared<lifetime_notification>();
   auto released = std::weak_ptr<lifetime_notification>{lifetime};
   auto terminal_release = lifetime->released.get_future();
   auto options = loopback_client_options();
   options.connect_timeout = std::chrono::seconds{10};
   options.handshake_timeout = std::chrono::seconds{10};
   options.connection_lifetime = std::move(lifetime);
   auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
   auto pending = boost::asio::co_spawn(
       strand, client.async_connect(to_quic_endpoint(blackhole->local_endpoint()), std::move(options)),
       boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future));

   const auto bytes = get_with_deadline(received, std::chrono::seconds{2}, "receive unanswered QUIC Initial");
   BOOST_REQUIRE(bytes >= 1200U);
   BOOST_TEST((packet.front() & 0xc0U) == 0xc0U);
   BOOST_TEST(!released.expired());
   BOOST_CHECK(pending.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   boost::asio::post(strand, [cancellation] {
      cancellation->emit(boost::asio::cancellation_type::terminal);
   });
   BOOST_REQUIRE(pending.wait_until(deadline) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(pending.get(), forge::exceptions::base, [](const auto& error) {
      return exceptions::code_of(error) == exceptions::code::canceled;
   });
   BOOST_CHECK(terminal_release.wait_until(deadline) == std::future_status::ready);
   BOOST_TEST(released.expired());
}

BOOST_AUTO_TEST_CASE(quic_frame_codec_round_trips_payload) {
   const auto payload = std::vector<std::uint8_t>{1, 2, 3, 4, 5};
   const auto encoded = encode_frame(payload);
   const auto decoded = decode_frame(encoded);

   BOOST_TEST(static_cast<int>(decoded.status) == static_cast<int>(frame_decode_status::complete));
   BOOST_TEST(decoded.consumed == encoded.size());
   BOOST_TEST(decoded.payload == payload, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(quic_frame_codec_reports_need_more_data) {
   const auto payload = std::vector<std::uint8_t>{1, 2, 3};
   auto encoded = encode_frame(payload);
   encoded.pop_back();

   const auto decoded = decode_frame(encoded);

   BOOST_TEST(static_cast<int>(decoded.status) == static_cast<int>(frame_decode_status::need_more_data));
   BOOST_TEST(decoded.consumed == 0U);
}

BOOST_AUTO_TEST_CASE(quic_frame_codec_rejects_oversized_payload) {
   const auto payload = std::vector<std::uint8_t>{1, 2, 3, 4};

   try {
      (void)encode_frame(payload, frame_codec_options{.max_frame_size = 3});
      BOOST_FAIL("expected typed QUIC exception");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::frame_too_large));
   }
}

BOOST_AUTO_TEST_CASE(quic_security_normalizes_fingerprint) {
   const auto raw =
       std::string{"AA:BB:CC:DD:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB"};
   const auto normalized = normalize_sha256_fingerprint(raw);

   BOOST_TEST(normalized == "aabbccdd00112233445566778899aabbccddeeff00112233445566778899aabb");
}

BOOST_AUTO_TEST_CASE(quic_security_verifies_pinned_certificate_digest) {
   const auto der = std::vector<std::uint8_t>{'s', 't', 'o', 'r', 'l', 'a', 'n', 'e'};
   const auto digest = sha256_fingerprint(der);
   const auto certificate = peer_certificate{.der = der, .sha256_fingerprint = digest};
   const auto options = security_options{.verify_peer = true, .expected_sha256_fingerprint = digest};

   BOOST_TEST(verify_peer_certificate(certificate, options));
}

BOOST_AUTO_TEST_CASE(quic_options_validation_rejects_bad_alpn) {
   auto options = client_options{};
   options.alpn.clear();

   try {
      validate(options);
      BOOST_FAIL("expected typed QUIC exception");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::invalid_options));
   }
}

BOOST_AUTO_TEST_CASE(quic_client_options_legacy_positional_initializer_keeps_test_failpoint) {
   const auto options = client_options{
       "legacy", std::chrono::milliseconds{1}, std::chrono::milliseconds{2}, std::chrono::milliseconds{3},
       transport_limits{}, security_options{}, "certificate", "private-key",
       [](std::string_view) { return true; },
   };

   BOOST_REQUIRE(static_cast<bool>(options.test_failpoint));
   BOOST_TEST(options.test_failpoint("test"));
   BOOST_TEST(!options.client_tokens);
}

BOOST_AUTO_TEST_CASE(quic_runtime_initializes_ngtcp2_crypto_ossl) {
   const auto capabilities = initialize_runtime();

   BOOST_TEST(!capabilities.ngtcp2_version.empty());
   BOOST_TEST(capabilities.tls_backend == "openssl");
   BOOST_TEST(capabilities.openssl_version_major >= 3U);
   BOOST_TEST(capabilities.crypto_ossl_initialized);
}

BOOST_AUTO_TEST_CASE(quic_loopback_handshake_and_echo_frame_over_udp) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{
       runtime,
       endpoint{.host = "127.0.0.1", .port = 0},
       loopback_server_options(),
   };

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   auto server_connection = accept_future.get();

   auto server_echo = boost::asio::co_spawn(
       runtime.context(),
       [server_connection =
            std::move(server_connection)]() mutable -> boost::asio::awaitable<std::vector<std::uint8_t>> {
          auto accepted = co_await server_connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          auto request = co_await framed.async_read_frame();
          co_await framed.async_write_frame(request);
          co_return request;
       },
       boost::asio::use_future);

   auto client_stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());
   auto framed = framed_stream{std::move(client_stream)};
   const auto payload = std::vector<std::uint8_t>{'p', 'i', 'n', 'g'};
   forge::asio::blocking::run(runtime, framed.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, framed.async_read_frame());
   const auto server_seen = server_echo.get();

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(server_seen == payload, boost::test_tools::per_element());
   BOOST_TEST(client_connection.metrics().handshakes_completed >= 1U);
   BOOST_TEST(client_connection.metrics().streams_opened >= 1U);

   forge::asio::blocking::run(runtime, client_connection.async_close());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(quic_public_operations_enter_transport_owner_strands) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_executor = boost::asio::make_strand(runtime.context());
   auto client_executor = boost::asio::make_strand(runtime.context());
   auto server_stream_executor = boost::asio::make_strand(runtime.context());
   auto client_stream_executor = boost::asio::make_strand(runtime.context());

   auto accept_future = boost::asio::co_spawn(accept_executor, server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto connect_future =
       boost::asio::co_spawn(client_executor, client.async_connect(server.local_endpoint(), loopback_client_options()),
                             boost::asio::use_future);
   auto client_connection =
       get_with_deadline_or_stop(runtime, connect_future, std::chrono::milliseconds{5'000}, "foreign-strand connect");
   auto server_connection =
       get_with_deadline_or_stop(runtime, accept_future, std::chrono::milliseconds{5'000}, "foreign-strand accept");

   auto server_echo = boost::asio::co_spawn(
       server_stream_executor,
       [connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<std::vector<std::uint8_t>> {
          auto accepted = co_await connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          auto request = co_await framed.async_read_frame();
          co_await framed.async_write_frame(request);
          co_return request;
       },
       boost::asio::use_future);
   const auto payload = std::vector<std::uint8_t>{'o', 'w', 'n', 'e', 'r'};
   auto client_echo = boost::asio::co_spawn(
       client_stream_executor,
       [&client_connection, payload]() -> boost::asio::awaitable<std::vector<std::uint8_t>> {
          auto opened = co_await client_connection.async_open_stream();
          co_await opened.async_write(std::span<const std::uint8_t>{});
          auto framed = framed_stream{std::move(opened)};
          co_await framed.async_write_frame(payload);
          co_return co_await framed.async_read_frame();
       },
       boost::asio::use_future);

   const auto reply =
       get_with_deadline_or_stop(runtime, client_echo, std::chrono::milliseconds{5'000}, "foreign-strand echo");
   const auto received =
       get_with_deadline_or_stop(runtime, server_echo, std::chrono::milliseconds{5'000}, "foreign-strand receive");
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(received == payload, boost::test_tools::per_element());

   const auto expected_local_endpoint = client_connection.local_endpoint();
   auto keep_reading_endpoint = std::atomic_bool{true};
   auto endpoint_reader = std::async(std::launch::async, [&] {
      auto reads = std::size_t{};
      auto mismatches = std::size_t{};
      while (keep_reading_endpoint.load(std::memory_order_acquire)) {
         const auto observed = client_connection.local_endpoint();
         if (observed.host != expected_local_endpoint.host || observed.port != expected_local_endpoint.port) {
            ++mismatches;
         }
         ++reads;
      }
      return std::pair{reads, mismatches};
   });
   auto close_future = boost::asio::co_spawn(client_executor, client_connection.async_close(), boost::asio::use_future);
   auto stop_future = boost::asio::co_spawn(accept_executor, server.async_stop(), boost::asio::use_future);
   get_with_deadline_or_stop(runtime, close_future, std::chrono::milliseconds{5'000}, "foreign-strand close");
   keep_reading_endpoint.store(false, std::memory_order_release);
   const auto [endpoint_reads, endpoint_mismatches] = endpoint_reader.get();
   BOOST_TEST(endpoint_reads > 0U);
   BOOST_TEST(endpoint_mismatches == 0U);
   BOOST_TEST(client_connection.local_endpoint().host == expected_local_endpoint.host);
   BOOST_TEST(client_connection.local_endpoint().port == expected_local_endpoint.port);
   get_with_deadline_or_stop(runtime, stop_future, std::chrono::milliseconds{5'000}, "foreign-strand stop");
}

BOOST_AUTO_TEST_CASE(quic_loopback_medium_frame_and_small_frame_burst) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   auto server_connection = accept_future.get();

   constexpr auto small_frame_count = 10'000U;
   auto server_task = boost::asio::co_spawn(
       runtime.context(),
       [server_connection =
            std::move(server_connection)]() mutable -> boost::asio::awaitable<std::pair<std::size_t, std::size_t>> {
          auto accepted = co_await server_connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          auto large = co_await framed.async_read_frame();
          co_await framed.async_write_frame(large);

          auto small_seen = std::size_t{0};
          for (auto index = 0U; index < small_frame_count; ++index) {
             auto frame = co_await framed.async_read_frame();
             small_seen += frame.size();
          }
          co_return std::pair{large.size(), small_seen};
       },
       boost::asio::use_future);

   auto client_stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());
   auto framed = framed_stream{std::move(client_stream)};
   auto large_payload = std::vector<std::uint8_t>(256 * 1024);
   for (std::size_t index = 0; index < large_payload.size(); ++index) {
      large_payload[index] = static_cast<std::uint8_t>(index % 251U);
   }
   forge::asio::blocking::run(runtime, framed.async_write_frame(large_payload));
   const auto large_reply = forge::asio::blocking::run(runtime, framed.async_read_frame());
   BOOST_TEST(large_reply == large_payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, [&framed]() -> boost::asio::awaitable<void> {
      for (auto index = 0U; index < small_frame_count; ++index) {
         const auto payload = std::vector<std::uint8_t>{static_cast<std::uint8_t>(index & 0xffU)};
         co_await framed.async_write_frame(payload);
      }
   }());
   const auto [large_seen, small_seen] = server_task.get();

   BOOST_TEST(large_seen == large_payload.size());
   BOOST_TEST(small_seen == small_frame_count);
   BOOST_TEST(client_connection.metrics().frames_sent >= small_frame_count + 1U);
   BOOST_TEST(client_connection.metrics().backpressure_rejections == 0U);

   forge::asio::blocking::run(runtime, client_connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_large_frame_over_real_quic_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   auto server_connection = accept_future.get();

   auto server_echo = boost::asio::co_spawn(
       runtime.context(),
       [server_connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<std::size_t> {
          auto accepted = co_await server_connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          auto request = co_await framed.async_read_frame();
          const auto size = request.size();
          co_await framed.async_write_frame(request);
          co_return size;
       },
       boost::asio::use_future);

   auto client_stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());
   auto framed = framed_stream{std::move(client_stream)};
   auto payload = std::vector<std::uint8_t>(4 * 1024 * 1024);
   for (std::size_t index = 0; index < payload.size(); ++index) {
      payload[index] = static_cast<std::uint8_t>((index * 17U) % 251U);
   }
   forge::asio::blocking::run(runtime, framed.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, framed.async_read_frame());
   const auto server_seen = server_echo.get();

   BOOST_TEST(server_seen == payload.size());
   BOOST_TEST(reply.size() == payload.size());
   BOOST_TEST(std::equal(reply.begin(), reply.end(), payload.begin(), payload.end()));
   BOOST_TEST(client_connection.metrics().backpressure_rejections == 0U);

   forge::asio::blocking::run(runtime, client_connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_many_parallel_streams_echo_frames) {
   auto limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 64, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection = forge::asio::blocking::run(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)));
   auto server_connection = accept_future.get();

   constexpr auto stream_count = 32U;
   auto server_echo = boost::asio::co_spawn(
       runtime.context(),
       [server_connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<std::size_t> {
          auto total = std::size_t{0};
          for (auto index = 0U; index < stream_count; ++index) {
             auto accepted = co_await server_connection.async_accept_stream();
             auto framed = framed_stream{std::move(accepted)};
             auto request = co_await framed.async_read_frame();
             total += request.size();
             co_await framed.async_write_frame(request);
          }
          co_return total;
       },
       boost::asio::use_future);

   auto replies = forge::asio::blocking::run(
       runtime, [&client_connection]() mutable -> boost::asio::awaitable<std::vector<std::vector<std::uint8_t>>> {
          auto streams = std::vector<framed_stream>{};
          auto expected = std::vector<std::vector<std::uint8_t>>{};
          streams.reserve(stream_count);
          expected.reserve(stream_count);
          for (auto index = 0U; index < stream_count; ++index) {
             auto stream_value = co_await client_connection.async_open_stream();
             streams.emplace_back(framed_stream{std::move(stream_value)});
             auto payload = std::vector<std::uint8_t>(128U + index);
             std::fill(payload.begin(), payload.end(), static_cast<std::uint8_t>(index));
             co_await streams.back().async_write_frame(payload);
             expected.push_back(std::move(payload));
          }

          auto replies = std::vector<std::vector<std::uint8_t>>{};
          replies.reserve(stream_count);
          for (auto& stream_value : streams) {
             replies.push_back(co_await stream_value.async_read_frame());
          }
          co_return replies;
       }());
   const auto server_total = server_echo.get();

   auto expected_total = std::size_t{0};
   for (auto index = 0U; index < stream_count; ++index) {
      expected_total += 128U + index;
      BOOST_TEST(replies[index].size() == 128U + index);
      BOOST_TEST(std::all_of(replies[index].begin(), replies[index].end(),
                             [index](std::uint8_t value) { return value == static_cast<std::uint8_t>(index); }));
   }
   BOOST_TEST(server_total == expected_total);
   BOOST_TEST(client_connection.metrics().streams_opened == stream_count);
   BOOST_TEST(client_connection.metrics().backpressure_rejections == 0U);

   forge::asio::blocking::run(runtime, client_connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_repeated_connect_transfer_close_soak) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};

   constexpr auto iteration_count = 10U;
   for (auto iteration = 0U; iteration < iteration_count; ++iteration) {
      auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
      auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
      auto client = connector{runtime};
      auto client_connection =
          forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
      auto server_connection = accept_future.get();

      auto server_echo = boost::asio::co_spawn(
          runtime.context(),
          [server_connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<void> {
             auto accepted = co_await server_connection.async_accept_stream();
             auto framed = framed_stream{std::move(accepted)};
             auto request = co_await framed.async_read_frame();
             co_await framed.async_write_frame(request);
          },
          boost::asio::use_future);

      auto client_stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());
      auto framed = framed_stream{std::move(client_stream)};
      const auto payload = std::vector<std::uint8_t>{static_cast<std::uint8_t>(iteration), 1, 2, 3};
      forge::asio::blocking::run(runtime, framed.async_write_frame(payload));
      const auto reply = forge::asio::blocking::run(runtime, framed.async_read_frame());
      server_echo.get();

      BOOST_TEST(reply == payload, boost::test_tools::per_element());
      forge::asio::blocking::run(runtime, client_connection.async_close());
      server.stop();
   }
}

BOOST_AUTO_TEST_CASE(quic_listener_shared_socket_serializes_concurrent_sends_and_stop) {
   constexpr auto connection_count = 8U;
   constexpr auto reply_size = 256U * 1024U;
   auto limits = transport_limits{
       .max_connections = connection_count,
       .max_streams_per_connection = 4,
       .max_queued_bytes = 32U * 1024U * 1024U,
   };
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = listener{
       runtime,
       endpoint{.host = "127.0.0.1", .port = 0},
       loopback_server_options("forge-p2p/1", limits),
   };
   auto connector_value = connector{runtime};
   auto clients = std::vector<connection>{};
   auto servers = std::vector<connection>{};
   clients.reserve(connection_count);
   servers.reserve(connection_count);

   for (auto index = 0U; index < connection_count; ++index) {
      auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);

      clients.push_back(run_with_deadline(
          runtime,
          connector_value.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
          std::chrono::milliseconds{5'000}, "shared socket client connect"));

      servers.push_back(
          get_with_deadline_or_stop(runtime, accepted, std::chrono::milliseconds{5'000}, "shared socket accept"));
   }

   auto server_tasks = std::vector<std::future<void>>{};
   server_tasks.reserve(connection_count);

   for (auto index = 0U; index < connection_count; ++index) {
      server_tasks.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&server_connection = servers[index], index]() -> boost::asio::awaitable<void> {
             auto stream_value = framed_stream{co_await server_connection.async_accept_stream()};

             auto request = co_await stream_value.async_read_frame();
             BOOST_REQUIRE(request == std::vector<std::uint8_t>{static_cast<std::uint8_t>(index)});

             auto reply = std::vector<std::uint8_t>(reply_size, static_cast<std::uint8_t>(index));
             co_await stream_value.async_write_frame(reply);
          },
          boost::asio::use_future));
   }

   auto client_streams = std::vector<framed_stream>{};
   client_streams.reserve(connection_count);

   for (auto index = 0U; index < connection_count; ++index) {
      client_streams.emplace_back(run_with_deadline(runtime, clients[index].async_open_stream(),
                                                    std::chrono::milliseconds{5'000}, "shared socket open stream"));

      const auto request = std::vector<std::uint8_t>{static_cast<std::uint8_t>(index)};
      run_with_deadline(runtime, client_streams.back().async_write_frame(request), std::chrono::milliseconds{5'000},
                        "shared socket request");
   }

   for (auto index = 0U; index < connection_count; ++index) {
      auto reply = run_with_deadline(runtime, client_streams[index].async_read_frame(),
                                     std::chrono::milliseconds{10'000}, "shared socket reply");

      BOOST_REQUIRE(reply.size() == reply_size);
      BOOST_TEST(std::ranges::all_of(
          reply, [index](std::uint8_t value) { return value == static_cast<std::uint8_t>(index); }));
   }

   for (auto& task : server_tasks) {
      get_with_deadline_or_stop(runtime, task, std::chrono::milliseconds{5'000}, "shared socket server send");
   }

   server_tasks.clear();

   for (auto index = 0U; index < connection_count; ++index) {
      server_tasks.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&server_connection = servers[index]]() -> boost::asio::awaitable<void> {
             auto stream_value = framed_stream{co_await server_connection.async_accept_stream()};
             (void)co_await stream_value.async_read_frame();

             auto reply = std::vector<std::uint8_t>(reply_size, 0xa5U);
             co_await stream_value.async_write_frame(reply);
          },
          boost::asio::use_future));

      client_streams[index] = framed_stream{run_with_deadline(
          runtime, clients[index].async_open_stream(), std::chrono::milliseconds{5'000}, "stop race open stream")};

      const auto request = std::vector<std::uint8_t>{0x01U};
      run_with_deadline(runtime, client_streams[index].async_write_frame(request), std::chrono::milliseconds{5'000},
                        "stop race request");
   }

   forge::asio::blocking::run(runtime, server.async_stop());

   for (auto& task : server_tasks) {
      BOOST_REQUIRE(task.wait_for(std::chrono::milliseconds{5'000}) == std::future_status::ready);

      try {
         task.get();
      } catch (const forge::exceptions::base&) {
         // Listener stop may abort a server write already queued on the shared socket.
      }
   }

   for (auto& client : clients) {
      client.cancel();
   }
}

BOOST_AUTO_TEST_CASE(quic_fault_proxy_handshake_survives_mild_loss) {
   auto limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto proxy = std::make_shared<udp_fault_proxy>(
       runtime.context(), server.local_endpoint(),
       fault_proxy_rules{
           .client_to_server = fault_rule{.drop_every = 7, .delay = std::chrono::milliseconds{1}},
           .server_to_client = fault_rule{.drop_every = 7, .delay = std::chrono::milliseconds{1}},
       });
   proxy->start();

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection = run_with_deadline(
       runtime, client.async_connect(proxy->local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{10'000}, "lossy handshake connect");
   auto server_connection =
       get_with_deadline(accept_future, std::chrono::milliseconds{10'000}, "lossy handshake accept");
   const auto proxy_metrics = proxy->metrics();

   BOOST_TEST(client_connection.valid());
   BOOST_TEST(server_connection.valid());
   BOOST_TEST(proxy_metrics.client_to_server.dropped + proxy_metrics.server_to_client.dropped > 0U);

   run_with_deadline(runtime, client_connection.async_close(), std::chrono::milliseconds{5'000},
                     "lossy handshake close");
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_stream_fin_retransmits_after_first_post_payload_datagram_loss) {
   auto limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto proxy = std::make_shared<udp_fault_proxy>(runtime.context(), server.local_endpoint(), fault_proxy_rules{});
   proxy->start();

   auto accept_connection = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection = run_with_deadline(
       runtime, client.async_connect(proxy->local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{5'000}, "FIN loss connect");
   auto server_connection =
       get_with_deadline_or_stop(runtime, accept_connection, std::chrono::milliseconds{5'000}, "FIN loss accept");

   auto accept_stream =
       boost::asio::co_spawn(runtime.context(), server_connection.async_accept_stream(), boost::asio::use_future);
   auto client_stream = run_with_deadline(runtime, client_connection.async_open_stream(),
                                          std::chrono::milliseconds{5'000}, "FIN loss open stream");
   const auto payload = std::vector<std::uint8_t>{'f', 'i', 'n'};
   run_with_deadline(runtime, client_stream.async_write(payload), std::chrono::milliseconds{5'000},
                     "FIN loss write payload");
   auto server_stream =
       get_with_deadline_or_stop(runtime, accept_stream, std::chrono::milliseconds{5'000}, "FIN loss accept stream");
   const auto received = run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{5'000},
                                           "FIN loss read payload");
   BOOST_TEST(received == payload, boost::test_tools::per_element());

   proxy->drop_next_client_to_server();
   run_with_deadline(runtime, client_stream.async_close(), std::chrono::milliseconds{5'000},
                     "FIN loss serialize close");
   try {
      static_cast<void>(run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{10'000},
                                          "FIN loss retransmitted close"));
      BOOST_FAIL("expected retransmitted QUIC FIN to close the remote stream");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::stream_closed));
   }
   BOOST_TEST(proxy->metrics().client_to_server.dropped >= 1U);

   run_with_deadline(runtime, client_connection.async_close(), std::chrono::milliseconds{5'000},
                     "FIN loss client close");
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_stream_large_payload_close_delivers_fin_without_followup_traffic) {
   auto limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto accept_connection = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{5'000}, "large FIN connect");
   auto server_connection =
       get_with_deadline_or_stop(runtime, accept_connection, std::chrono::milliseconds{5'000}, "large FIN accept");

   auto accept_stream =
       boost::asio::co_spawn(runtime.context(), server_connection.async_accept_stream(), boost::asio::use_future);
   auto client_stream = run_with_deadline(runtime, client_connection.async_open_stream(),
                                          std::chrono::milliseconds{5'000}, "large FIN open stream");
   const auto payload = std::vector<std::uint8_t>(13'950, 0x5aU);
   run_with_deadline(runtime, client_stream.async_write(payload), std::chrono::milliseconds{5'000},
                     "large FIN write payload");
   run_with_deadline(runtime, client_stream.async_close(), std::chrono::milliseconds{5'000},
                     "large FIN serialize close");

   auto server_stream =
       get_with_deadline_or_stop(runtime, accept_stream, std::chrono::milliseconds{5'000}, "large FIN accept stream");
   auto received = std::vector<std::uint8_t>{};
   while (received.size() < payload.size()) {
      auto chunk = run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{5'000},
                                     "large FIN read payload");
      received.insert(received.end(), chunk.begin(), chunk.end());
   }
   BOOST_TEST(received == payload, boost::test_tools::per_element());
   try {
      static_cast<void>(run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{5'000},
                                          "large FIN remote close"));
      BOOST_FAIL("expected QUIC FIN after a large payload");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::stream_closed));
   }

   run_with_deadline(runtime, client_connection.async_close(), std::chrono::milliseconds{5'000},
                     "large FIN client close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_fault_proxy_framed_echo_survives_loss_delay_reorder_duplicate) {
   constexpr auto lossy_connect_deadline = std::chrono::milliseconds{15'000};
   constexpr auto lossy_stream_deadline = std::chrono::milliseconds{10'000};
   constexpr auto lossy_transfer_deadline = std::chrono::milliseconds{30'000};
   constexpr auto lossy_close_deadline = std::chrono::milliseconds{10'000};
   auto limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto proxy = std::make_shared<udp_fault_proxy>(runtime.context(), server.local_endpoint(),
                                                  fault_proxy_rules{
                                                      .client_to_server =
                                                          fault_rule{
                                                              .drop_every = 53,
                                                              .drop_after = 16,
                                                              .duplicate_every = 17,
                                                              .duplicate_after = 8,
                                                              .reorder_every = 23,
                                                              .reorder_after = 8,
                                                              .delay = std::chrono::milliseconds{1},
                                                          },
                                                      .server_to_client =
                                                          fault_rule{
                                                              .drop_every = 59,
                                                              .drop_after = 16,
                                                              .duplicate_every = 19,
                                                              .duplicate_after = 8,
                                                              .reorder_every = 29,
                                                              .reorder_after = 8,
                                                              .delay = std::chrono::milliseconds{1},
                                                          },
                                                  });
   proxy->start();

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection = run_with_deadline(
       runtime, client.async_connect(proxy->local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       lossy_connect_deadline, "lossy echo connect");
   auto server_connection =
       get_with_deadline_or_stop(runtime, accept_future, lossy_connect_deadline, "lossy echo accept");

   auto server_echo = boost::asio::co_spawn(
       runtime.context(),
       [server_connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<std::size_t> {
          auto accepted = co_await server_connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          auto request = co_await framed.async_read_frame();
          const auto size = request.size();
          co_await framed.async_write_frame(request);
          co_return size;
       },
       boost::asio::use_future);

   auto client_stream = run_with_deadline(runtime, client_connection.async_open_stream(), lossy_stream_deadline,
                                          "lossy echo open stream");
   auto framed = framed_stream{std::move(client_stream)};
   auto payload = std::vector<std::uint8_t>(64 * 1024);
   for (std::size_t index = 0; index < payload.size(); ++index) {
      payload[index] = static_cast<std::uint8_t>((index * 23U) % 251U);
   }
   run_with_deadline(runtime, framed.async_write_frame(payload), lossy_transfer_deadline, "lossy echo write frame");
   const auto reply =
       run_with_deadline(runtime, framed.async_read_frame(), lossy_transfer_deadline, "lossy echo read frame");
   const auto server_seen =
       get_with_deadline_or_stop(runtime, server_echo, lossy_transfer_deadline, "lossy echo server task");
   const auto proxy_metrics = proxy->metrics();

   BOOST_TEST(server_seen == payload.size());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(proxy_metrics.client_to_server.dropped + proxy_metrics.server_to_client.dropped > 0U);
   BOOST_TEST(proxy_metrics.client_to_server.reordered + proxy_metrics.server_to_client.reordered > 0U);
   BOOST_TEST(proxy_metrics.client_to_server.duplicated + proxy_metrics.server_to_client.duplicated > 0U);
   BOOST_TEST(proxy_metrics.client_to_server.delayed > 0U);
   BOOST_TEST(proxy_metrics.server_to_client.delayed > 0U);
   BOOST_TEST(client_connection.metrics().backpressure_rejections == 0U);

   run_with_deadline(runtime, client_connection.async_close(), lossy_close_deadline, "lossy echo close");
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_fault_proxy_repeated_connect_transfer_close) {
   constexpr auto lossy_connect_deadline = std::chrono::milliseconds{15'000};
   constexpr auto lossy_stream_deadline = std::chrono::milliseconds{10'000};
   constexpr auto lossy_transfer_deadline = std::chrono::milliseconds{30'000};
   constexpr auto lossy_close_deadline = std::chrono::milliseconds{10'000};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};

   constexpr auto iteration_count = 3U;
   auto client = connector{runtime};
   for (auto iteration = 0U; iteration < iteration_count; ++iteration) {
      const auto label_prefix = std::string{"lossy reconnect iteration "} + std::to_string(iteration) + " ";
      auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
      auto proxy = std::make_shared<udp_fault_proxy>(runtime.context(), server.local_endpoint(),
                                                     fault_proxy_rules{
                                                         .client_to_server =
                                                             fault_rule{
                                                                 .drop_every = 23,
                                                                 .drop_after = 16,
                                                                 .delay = std::chrono::milliseconds{1},
                                                             },
                                                         .server_to_client =
                                                             fault_rule{
                                                                 .drop_every = 29,
                                                                 .drop_after = 16,
                                                                 .delay = std::chrono::milliseconds{1},
                                                             },
                                                     });
      proxy->start();
      auto server_task = boost::asio::co_spawn(
          runtime.context(),
          [&server]() -> boost::asio::awaitable<std::size_t> {
             auto server_connection = co_await server.async_accept();
             auto accepted = co_await server_connection.async_accept_stream();
             auto framed = framed_stream{std::move(accepted)};
             auto request = co_await framed.async_read_frame();
             const auto size = request.size();
             co_await framed.async_write_frame(request);
             co_return size;
          },
          boost::asio::use_future);

      auto client_connection =
          run_with_deadline(runtime, client.async_connect(proxy->local_endpoint(), loopback_client_options()),
                            lossy_connect_deadline, label_prefix + "connect");
      auto client_stream = run_with_deadline(runtime, client_connection.async_open_stream(), lossy_stream_deadline,
                                             label_prefix + "open stream");
      auto framed = framed_stream{std::move(client_stream)};
      auto payload = std::vector<std::uint8_t>(64 * 1024);
      for (std::size_t index = 0; index < payload.size(); ++index) {
         payload[index] = static_cast<std::uint8_t>((index + iteration * 11U) % 251U);
      }
      run_with_deadline(runtime, framed.async_write_frame(payload), lossy_transfer_deadline,
                        label_prefix + "write frame");
      const auto reply =
          run_with_deadline(runtime, framed.async_read_frame(), lossy_transfer_deadline, label_prefix + "read frame");
      BOOST_TEST(reply == payload, boost::test_tools::per_element());
      run_with_deadline(runtime, client_connection.async_close(), lossy_close_deadline, label_prefix + "close");
      BOOST_TEST(get_with_deadline_or_stop(runtime, server_task, lossy_transfer_deadline,
                                           label_prefix + "server task") == payload.size());
      const auto proxy_metrics = proxy->metrics();
      BOOST_TEST(proxy_metrics.client_to_server.dropped + proxy_metrics.server_to_client.dropped > 0U);
      proxy->stop();
      server.stop();
   }
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_alpn_mismatch) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1")};
   auto client = connector{runtime};

   try {
      (void)forge::asio::blocking::run(
          runtime,
          client.async_connect(server.local_endpoint(), client_options{
                                                            .alpn = "wrong-alpn",
                                                            .handshake_timeout = std::chrono::milliseconds{500},
                                                            .security = security_options{.verify_peer = false},
                                                        }));
      BOOST_FAIL("expected QUIC handshake/alpn failure");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::handshake_timeout ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::alpn_mismatch ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::internal;
      BOOST_TEST(acceptable);
   }
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_connect_timeout_limits_stalled_handshake_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto blackhole = udp::socket{runtime.context()};
   blackhole.open(udp::v4());
   blackhole.bind(udp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0});

   auto client = connector{runtime};
   auto options = loopback_client_options();
   options.connect_timeout = std::chrono::milliseconds{100};
   options.handshake_timeout = std::chrono::milliseconds{5'000};
   const auto started = std::chrono::steady_clock::now();

   try {
      (void)run_with_deadline(runtime,
                              client.async_connect(to_quic_endpoint(blackhole.local_endpoint()), std::move(options)),
                              std::chrono::milliseconds{2'000}, "blackhole connect timeout");
      BOOST_FAIL("expected QUIC connect timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::connect_timeout));
   }
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   BOOST_TEST(elapsed.count() < 2'000);
   blackhole.close();
}

BOOST_AUTO_TEST_CASE(quic_throwing_inbound_admission_surfaces_internal_accept_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto calls = std::atomic_size_t{0};
   auto options = loopback_server_options();
   options.inbound_admission = [&calls]() -> std::shared_ptr<void> {
      ++calls;
      throw std::bad_alloc{};
   };
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(options)};
   auto client = connector{runtime};
   auto client_options = loopback_client_options();
   client_options.handshake_timeout = std::chrono::milliseconds{500};
   client_options.connect_timeout = std::chrono::milliseconds{1'000};
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto attempted = boost::asio::co_spawn(runtime.context(),
                                           client.async_connect(server.local_endpoint(), client_options),
                                           boost::asio::use_future);

   auto result = std::optional<exceptions::code>{};
   try {
      static_cast<void>(get_with_deadline(accepted, std::chrono::milliseconds{2'000}, "throwing inbound admission"));
   } catch (const forge::exceptions::base& error) {
      result = exceptions::code_of(error);
   }

   BOOST_REQUIRE(calls.load(std::memory_order_relaxed) > 0U);
   BOOST_REQUIRE(result.has_value());
   BOOST_TEST(static_cast<int>(*result) == static_cast<int>(exceptions::code::internal));
   server.stop();
   static_cast<void>(attempted);
}

BOOST_AUTO_TEST_CASE(quic_failed_handshake_releases_listener_connection_slot) {
   auto limits =
       transport_limits{.max_connections = 1, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto client = connector{runtime};

   try {
      (void)run_with_deadline(runtime,
                              client.async_connect(server.local_endpoint(),
                                                   client_options{
                                                       .alpn = "wrong-alpn",
                                                       .handshake_timeout = std::chrono::milliseconds{500},
                                                       .limits = limits,
                                                       .security = security_options{.verify_peer = false},
                                                   }),
                              std::chrono::milliseconds{2'000}, "failed alpn connect");
      BOOST_FAIL("expected QUIC ALPN failure");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::handshake_timeout ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::alpn_mismatch ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::internal;
      BOOST_TEST(acceptable);
   }

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto valid = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{5'000}, "valid connect after failed alpn");
   auto accepted = get_with_deadline(accept_future, std::chrono::milliseconds{5'000}, "accept after failed alpn");
   BOOST_TEST(valid.valid());
   BOOST_TEST(accepted.valid());

   run_with_deadline(runtime, valid.async_close(), std::chrono::milliseconds{5'000}, "close valid after failed alpn");
   run_with_deadline(runtime, accepted.async_close(), std::chrono::milliseconds{5'000},
                     "close accepted after failed alpn");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_remote_close_during_active_read_is_reported) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   auto server_connection = accept_future.get();

   auto server_close = boost::asio::co_spawn(
       runtime.context(),
       [server_connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<void> {
          auto accepted = co_await server_connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          (void)co_await framed.async_read_frame();
          co_await framed.async_close();
       },
       boost::asio::use_future);

   auto client_stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());
   auto framed = framed_stream{std::move(client_stream)};
   forge::asio::blocking::run(runtime, framed.async_write_frame(std::vector<std::uint8_t>{'c', 'l', 'o', 's', 'e'}));
   try {
      (void)forge::asio::blocking::run(runtime, framed.async_read_frame());
      BOOST_FAIL("expected remote stream close to unblock read with typed error");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::stream_closed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::connection_closed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::stream_reset;
      BOOST_TEST(acceptable);
   }
   server_close.get();
   forge::asio::blocking::run(runtime, client_connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_listener_reuses_connection_slot_after_close) {
   auto limits =
       transport_limits{.max_connections = 1, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto client = connector{runtime};

   auto accept_first = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   BOOST_TEST_CHECKPOINT("connect first QUIC session");
   auto first = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{5'000}, "first connect with max one connection");
   BOOST_TEST_CHECKPOINT("accept first QUIC session");
   auto first_server =
       get_with_deadline(accept_first, std::chrono::milliseconds{5'000}, "first accept with max one connection");
   BOOST_TEST_CHECKPOINT("close first QUIC session");
   run_with_deadline(runtime, first.async_close(), std::chrono::milliseconds{5'000}, "first client close");
   run_with_deadline(runtime, first_server.async_close(), std::chrono::milliseconds{5'000}, "first server close");

   auto accept_second = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   BOOST_TEST_CHECKPOINT("connect replacement QUIC session");
   auto second = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{5'000}, "second connect after cleanup");
   BOOST_TEST_CHECKPOINT("accept replacement QUIC session");
   auto second_server =
       get_with_deadline(accept_second, std::chrono::milliseconds{5'000}, "second accept after cleanup");
   BOOST_TEST(second.valid());
   BOOST_TEST(second_server.valid());

   run_with_deadline(runtime, second.async_close(), std::chrono::milliseconds{5'000}, "second client close");
   run_with_deadline(runtime, second_server.async_close(), std::chrono::milliseconds{5'000}, "second server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_connection_cancel_rejects_new_streams) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   (void)accept_future.get();
   client_connection.cancel();

   try {
      (void)forge::asio::blocking::run(runtime, client_connection.async_open_stream());
      BOOST_FAIL("expected canceled connection to reject new streams");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::connection_closed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::canceled;
      BOOST_TEST(acceptable);
   }
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_connection_drop_releases_native_lifetime_without_explicit_close) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto options = loopback_client_options();
   options.connection_lifetime = std::move(lifetime);

   {
      auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
      auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                          std::chrono::milliseconds{5'000}, "connect disposable QUIC session");
      auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept disposable QUIC session");
      BOOST_TEST(!released.expired());
   }

   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{5'000}, "release dropped QUIC native lifetime");
   BOOST_TEST(released.expired());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_background_drain_exception_terminalizes_and_releases_lifetime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto injected = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.connection_lifetime = std::move(lifetime);
   options.test_failpoint = [injected](std::string_view name) {
      return name == "background_stream_drain_failure" && !injected->exchange(true, std::memory_order_acq_rel);
   };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect background-failure QUIC session");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept background-failure QUIC session");
   auto stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                   "open background-failure QUIC stream");

   run_with_deadline(runtime, stream.async_write(std::vector<std::uint8_t>{'f'}), std::chrono::milliseconds{5'000},
                     "queue injected background-failure QUIC write");
   run_with_deadline(
       runtime,
       [&connection]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!connection.metrics().closed) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "terminalize injected QUIC background failure");
   BOOST_TEST(injected->load(std::memory_order_acquire));

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "join injected QUIC background failure");
   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "release injected-failure QUIC native lifetime");
   BOOST_TEST(released.expired());
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close injected-failure QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_expiry_worker_exception_terminalizes_and_releases_lifetime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = loopback_server_options();
   server_options.idle_timeout = std::chrono::milliseconds{100};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(server_options)};
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto armed = std::make_shared<std::atomic_bool>(false);
   auto injected = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.idle_timeout = std::chrono::milliseconds{100};
   options.connection_lifetime = std::move(lifetime);
   options.test_failpoint = [armed, injected](std::string_view name) {
      return name == "expiry_worker_failure" && armed->load(std::memory_order_acquire) &&
             !injected->exchange(true, std::memory_order_acq_rel);
   };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect expiry-failure QUIC session");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept expiry-failure QUIC session");
   armed->store(true, std::memory_order_release);

   run_with_deadline(
       runtime,
       [&connection]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!connection.metrics().closed) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "terminalize injected QUIC expiry failure");
   BOOST_TEST(injected->load(std::memory_order_acquire));

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "join injected QUIC expiry failure");
   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "release expiry-failure QUIC native lifetime");
   BOOST_TEST(released.expired());
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close expiry-failure QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_concurrent_async_close_waits_for_terminal_cleanup_and_preserves_primary_error) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto barrier_mutex = std::make_shared<std::mutex>();
   auto entered_changed = std::make_shared<std::condition_variable>();
   auto release_changed = std::make_shared<std::condition_variable>();
   auto entered = std::make_shared<std::atomic_bool>(false);
   auto barrier_released = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.connection_lifetime = std::move(lifetime);
   options.test_failpoint =
       [barrier_mutex, entered_changed, release_changed, entered, barrier_released](std::string_view name) {
      if (name != "async_close_before_send") {
         return false;
      }
      auto lock = std::unique_lock{*barrier_mutex};
      entered->store(true, std::memory_order_release);
      entered_changed->notify_all();
      release_changed->wait(lock, [&] { return barrier_released->load(std::memory_order_acquire); });
      throw std::runtime_error{"injected QUIC close failure"};
   };
   auto release_guard =
       std::shared_ptr<void>{new int{0}, [barrier_released, release_changed](int* value) noexcept {
                               delete value;
                               barrier_released->store(true, std::memory_order_release);
                               release_changed->notify_all();
                            }};
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect concurrent-close QUIC session");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept concurrent-close QUIC session");
   auto first_close = boost::asio::co_spawn(runtime.context(), connection.async_close(), boost::asio::use_future);

   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(entered_changed->wait_for(lock, std::chrono::seconds{2},
                                               [&] { return entered->load(std::memory_order_acquire); }));
   }
   auto second_close = boost::asio::co_spawn(runtime.context(), connection.async_close(), boost::asio::use_future);
   BOOST_CHECK(second_close.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   release_guard.reset();
   BOOST_REQUIRE(first_close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_REQUIRE(second_close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(first_close.get(), std::runtime_error);
   BOOST_CHECK_THROW(second_close.get(), std::runtime_error);
   BOOST_TEST(connection.metrics().closed);
   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "release failed-close QUIC native lifetime");
   BOOST_TEST(released.expired());
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close concurrent-close QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_preterminal_stream_close_failure_resets_before_cached_error) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto barrier_mutex = std::make_shared<std::mutex>();
   auto entered_changed = std::make_shared<std::condition_variable>();
   auto release_changed = std::make_shared<std::condition_variable>();
   auto entered = std::make_shared<std::atomic_bool>(false);
   auto released = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.test_failpoint = [barrier_mutex, entered_changed, release_changed, entered, released](std::string_view name) {
      if (name != "stream_close_before_fin") {
         return false;
      }
      auto lock = std::unique_lock{*barrier_mutex};
      entered->store(true, std::memory_order_release);
      entered_changed->notify_all();
      release_changed->wait(lock, [&] { return released->load(std::memory_order_acquire); });
      return true;
   };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect stream-close failure QUIC session");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept stream-close failure QUIC session");
   auto stream = as_transport_stream(run_with_deadline(runtime, connection.async_open_stream(),
                                                        std::chrono::milliseconds{5'000},
                                                        "open stream-close failure QUIC stream"));

   auto first_close = boost::asio::co_spawn(runtime.context(), stream.async_close(), boost::asio::use_future);
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(entered_changed->wait_for(lock, std::chrono::seconds{2},
                                               [&] { return entered->load(std::memory_order_acquire); }));
   }
   auto second_close = boost::asio::co_spawn(runtime.context(), stream.async_close(), boost::asio::use_future);
   BOOST_TEST(static_cast<int>(second_close.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));

   released->store(true, std::memory_order_release);
   release_changed->notify_all();
   BOOST_REQUIRE(first_close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_REQUIRE(second_close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(first_close.get(), std::bad_alloc);
   BOOST_CHECK_THROW(second_close.get(), std::bad_alloc);
   const auto metrics = connection.metrics();
   BOOST_TEST(metrics.streams_reset == 1U);
   BOOST_TEST(metrics.active_streams == 0U);

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "close reset stream QUIC session");
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close reset stream QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_stream_close_owner_start_failure_hands_off_to_cancel_worker) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto options = loopback_client_options();
   options.test_failpoint = [](std::string_view name) { return name == "stream_close_before_owner_spawn"; };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect owner-start failure QUIC session");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept owner-start failure QUIC session");
   auto stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                   "open owner-start failure QUIC stream");

   BOOST_CHECK_THROW(run_with_deadline(runtime, stream.async_close(), std::chrono::milliseconds{2'000},
                                       "join cancel-worker stream reset"),
                     std::bad_alloc);
   const auto metrics = connection.metrics();
   BOOST_TEST(metrics.streams_reset == 1U);
   BOOST_TEST(metrics.active_streams == 0U);

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "close owner-start failure QUIC session");
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close owner-start failure QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_native_stream_close_does_not_complete_held_reset_owner) {
   for (const auto fail_close : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
      auto client = connector{runtime};
      auto mutex = std::make_shared<std::mutex>();
      auto changed = std::make_shared<std::condition_variable>();
      auto armed = std::make_shared<std::atomic_bool>(false);
      auto held = std::make_shared<std::atomic_bool>(false);
      auto released = std::make_shared<std::atomic_bool>(false);
      auto native_closed = std::make_shared<std::atomic_bool>(false);
      auto joining = std::make_shared<std::atomic_bool>(false);
      auto options = loopback_client_options();
      options.test_failpoint = [changed, armed, held, released, native_closed, joining, fail_close](std::string_view name) {
         if (name == "stream_reset_after_publish_before_drain") {
            if (!armed->load() || held->exchange(true)) { return false; }
            changed->notify_all();
            return true;
         }
         if (name == "stream_reset_after_publish_before_drain_wait") { return !released->load(); }
         if (name == "stream_native_close_after_terminal_cleanup" && held->load() && !released->load()) {
            native_closed->store(true);
            changed->notify_all();
         }
         if (name == "stream_close_before_state_check") { return fail_close && armed->load(); }
         if ((fail_close && name == "stream_reset_before_terminal_join") ||
             (!fail_close && name == "stream_close_before_terminal_join")) {
            joining->store(true);
            changed->notify_all();
         }
         return false;
      };
      auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
      auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                          std::chrono::seconds{5}, "connect native-close ownership fixture");
      auto inbound = get_with_deadline(accepted, std::chrono::seconds{5}, "accept native-close ownership fixture");
      auto stream = as_transport_stream(run_with_deadline(runtime, connection.async_open_stream(),
          std::chrono::seconds{2}, "open native-close target stream"));
      auto trigger = std::optional<forge::net::quic::stream>{};
      auto close = std::future<void>{};
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{held.get(), [&](void*) noexcept {
         released->store(true);
         armed->store(false);
         if (close.valid()) {
            if (close.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
               BOOST_ERROR("native-close fixture did not join its stream close");
            } else {
               try { close.get(); }
               catch (const std::bad_alloc&) {}
               catch (...) { BOOST_ERROR("native-close fixture returned an unexpected close error"); }
            }
         }
         try {
            run_with_deadline(runtime, connection.async_close(), std::chrono::seconds{2}, "join native-close client");
         } catch (...) { BOOST_ERROR("native-close client cleanup failed"); }
         try {
            run_with_deadline(runtime, inbound.async_close(), std::chrono::seconds{2}, "join native-close peer");
         } catch (...) { BOOST_ERROR("native-close peer cleanup failed"); }
         try {
            run_with_deadline(runtime, server.async_stop(), std::chrono::seconds{2}, "join native-close listener");
         } catch (...) { BOOST_ERROR("native-close listener cleanup failed"); }
      }};
      // Materialize the stream on the peer before resetting either direction.
      auto accepted_stream = boost::asio::co_spawn(runtime.context(), inbound.async_accept_stream(), boost::asio::use_future);
      const auto payload = std::vector<std::uint8_t>{1};
      run_with_deadline(runtime, stream.async_write(payload), std::chrono::seconds{2}, "write native-close target");
      auto peer_stream = get_with_deadline(accepted_stream, std::chrono::seconds{2}, "accept native-close target");
      BOOST_TEST(run_with_deadline(runtime, peer_stream.async_read(), std::chrono::seconds{2},
                                  "read native-close target") == payload);
      armed->store(true);
      stream.request_cancel();
      {
         auto lock = std::unique_lock{*mutex};
         BOOST_REQUIRE(changed->wait_for(lock, std::chrono::seconds{2}, [&] { return held->load(); }));
      }
      peer_stream.request_cancel();
      // This normal open drives the connection independently of the held cancel
      // worker. No synthetic callback or terminal-state mutation is injected.
      trigger.emplace(run_with_deadline(runtime, connection.async_open_stream(), std::chrono::seconds{2},
                                        "drive native RESET_STREAM independently"));
      {
         auto lock = std::unique_lock{*mutex};
         BOOST_REQUIRE(changed->wait_for(lock, std::chrono::seconds{2}, [&] { return native_closed->load(); }));
      }
      close = boost::asio::co_spawn(runtime.context(), stream.async_close(), boost::asio::use_future);
      {
         auto lock = std::unique_lock{*mutex};
         BOOST_REQUIRE(changed->wait_for(lock, std::chrono::seconds{2}, [&] { return joining->load(); }));
      }
      BOOST_CHECK(close.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
      released->store(true);
      BOOST_REQUIRE(close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      if (fail_close) { BOOST_CHECK_THROW(close.get(), std::bad_alloc); }
      else { BOOST_CHECK_NO_THROW(close.get()); }
      armed->store(false);
      run_with_deadline(runtime, peer_stream.async_close(), std::chrono::seconds{2}, "join native-close target peer");
      run_with_deadline(runtime, trigger->async_close(), std::chrono::seconds{2}, "close native-close drain trigger");
   }
}

BOOST_AUTO_TEST_CASE(quic_close_failure_joins_cancel_worker_terminal_drain) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto barrier_mutex = std::make_shared<std::mutex>();
   auto reset_changed = std::make_shared<std::condition_variable>();
   auto close_changed = std::make_shared<std::condition_variable>();
   auto arm_local_reset_count = std::make_shared<std::atomic_bool>(false);
   auto arm_close_reset_join = std::make_shared<std::atomic_bool>(false);
   auto local_reset_publications = std::make_shared<std::atomic_uint>(0);
   auto close_failure_entered = std::make_shared<std::atomic_bool>(false);
   auto terminal_join_entered = std::make_shared<std::atomic_bool>(false);
   auto release_reset_drain = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.test_failpoint = [reset_changed, close_changed, arm_local_reset_count, arm_close_reset_join,
                             local_reset_publications, close_failure_entered, terminal_join_entered,
                             release_reset_drain](std::string_view name) {
      if (name == "stream_reset_published") {
         if (arm_local_reset_count->load(std::memory_order_acquire)) {
            local_reset_publications->fetch_add(1, std::memory_order_acq_rel);
            reset_changed->notify_all();
         }
         return false;
      }
      if (name == "stream_reset_after_publish_before_drain") {
         return arm_local_reset_count->load(std::memory_order_acquire) &&
                local_reset_publications->load(std::memory_order_acquire) == 1U;
      }
      if (name == "stream_reset_after_publish_before_drain_wait") {
         return !release_reset_drain->load(std::memory_order_acquire);
      }
      if (name == "stream_close_before_state_check") {
         close_failure_entered->store(true, std::memory_order_release);
         close_changed->notify_all();
         return true;
      }
      if (name == "stream_reset_before_terminal_join") {
         if (!arm_close_reset_join->load(std::memory_order_acquire) ||
             !close_failure_entered->load(std::memory_order_acquire) ||
             local_reset_publications->load(std::memory_order_acquire) != 1U) {
            return false;
         }
         terminal_join_entered->store(true, std::memory_order_release);
         close_changed->notify_all();
         return false;
      }
      return false;
   };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect reset-race QUIC session");
   auto inbound = get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept reset-race QUIC session");
   auto stream = as_transport_stream(run_with_deadline(runtime, connection.async_open_stream(),
                                                        std::chrono::milliseconds{5'000},
                                                        "open reset-race QUIC stream"));

   arm_local_reset_count->store(true, std::memory_order_release);
   stream.request_cancel();
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(reset_changed->wait_for(lock, std::chrono::seconds{2},
                                             [&] { return local_reset_publications->load(std::memory_order_acquire) == 1U; }));
   }
   BOOST_TEST(local_reset_publications->load(std::memory_order_acquire) == 1U);
   arm_close_reset_join->store(true, std::memory_order_release);
   auto close = boost::asio::co_spawn(runtime.context(), stream.async_close(), boost::asio::use_future);
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(close_changed->wait_for(lock, std::chrono::seconds{2},
                                             [&] { return close_failure_entered->load(std::memory_order_acquire) &&
                                                          terminal_join_entered->load(std::memory_order_acquire); }));
   }
   BOOST_TEST(static_cast<int>(close.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));

   release_reset_drain->store(true, std::memory_order_release);
   BOOST_REQUIRE(close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(close.get(), std::bad_alloc);
   BOOST_TEST(local_reset_publications->load(std::memory_order_acquire) == 1U);
   BOOST_TEST(connection.metrics().active_streams == 0U);

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "close reset-race QUIC session");
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close reset-race QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_close_after_cancel_joins_reset_terminal_drain) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto barrier_mutex = std::make_shared<std::mutex>();
   auto reset_changed = std::make_shared<std::condition_variable>();
   auto close_changed = std::make_shared<std::condition_variable>();
   auto arm_local_reset_count = std::make_shared<std::atomic_bool>(false);
   auto arm_success_terminal_join = std::make_shared<std::atomic_bool>(false);
   auto local_reset_publications = std::make_shared<std::atomic_uint>(0);
   auto success_terminal_join_entered = std::make_shared<std::atomic_bool>(false);
   auto release_reset_drain = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.test_failpoint = [reset_changed, close_changed, arm_local_reset_count, arm_success_terminal_join,
                             local_reset_publications, success_terminal_join_entered,
                             release_reset_drain](std::string_view name) {
      if (name == "stream_reset_published") {
         if (arm_local_reset_count->load(std::memory_order_acquire)) {
            local_reset_publications->fetch_add(1, std::memory_order_acq_rel);
            reset_changed->notify_all();
         }
         return false;
      }
      if (name == "stream_reset_after_publish_before_drain") {
         return arm_local_reset_count->load(std::memory_order_acquire) &&
                local_reset_publications->load(std::memory_order_acquire) == 1U;
      }
      if (name == "stream_reset_after_publish_before_drain_wait") {
         return !release_reset_drain->load(std::memory_order_acquire);
      }
      if (name == "stream_close_before_terminal_join") {
         if (!arm_success_terminal_join->load(std::memory_order_acquire) ||
             local_reset_publications->load(std::memory_order_acquire) != 1U) {
            return false;
         }
         success_terminal_join_entered->store(true, std::memory_order_release);
         close_changed->notify_all();
         return false;
      }
      return false;
   };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect successful reset-race QUIC session");
   auto inbound =
       get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept successful reset-race QUIC session");
   auto stream = as_transport_stream(run_with_deadline(runtime, connection.async_open_stream(),
                                                        std::chrono::milliseconds{5'000},
                                                        "open successful reset-race QUIC stream"));

   arm_local_reset_count->store(true, std::memory_order_release);
   stream.request_cancel();
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(reset_changed->wait_for(lock, std::chrono::seconds{2},
                                             [&] { return local_reset_publications->load(std::memory_order_acquire) == 1U; }));
   }
   BOOST_TEST(local_reset_publications->load(std::memory_order_acquire) == 1U);
   arm_success_terminal_join->store(true, std::memory_order_release);
   auto close = boost::asio::co_spawn(runtime.context(), stream.async_close(), boost::asio::use_future);
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(close_changed->wait_for(lock, std::chrono::seconds{2}, [&] {
         return success_terminal_join_entered->load(std::memory_order_acquire);
      }));
   }
   BOOST_TEST(static_cast<int>(close.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));

   release_reset_drain->store(true, std::memory_order_release);
   BOOST_REQUIRE(close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(close.get());
   BOOST_TEST(local_reset_publications->load(std::memory_order_acquire) == 1U);
   BOOST_TEST(connection.metrics().active_streams == 0U);

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "close successful reset-race QUIC session");
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close successful reset-race QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_close_failure_first_reset_owner_joins_active_native_drain) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto barrier_mutex = std::make_shared<std::mutex>();
   auto drain_changed = std::make_shared<std::condition_variable>();
   auto close_changed = std::make_shared<std::condition_variable>();
   auto arm_drain_pause = std::make_shared<std::atomic_bool>(false);
   auto drain_pause_consumed = std::make_shared<std::atomic_bool>(false);
   auto drain_owner_entered = std::make_shared<std::atomic_bool>(false);
   auto arm_local_reset_count = std::make_shared<std::atomic_bool>(false);
   auto arm_close_reset_join = std::make_shared<std::atomic_bool>(false);
   auto local_reset_publications = std::make_shared<std::atomic_uint>(0);
   auto close_failure_entered = std::make_shared<std::atomic_bool>(false);
   auto drain_join_entered = std::make_shared<std::atomic_bool>(false);
   auto release_drain = std::make_shared<std::atomic_bool>(false);
   auto options = loopback_client_options();
   options.test_failpoint = [drain_changed, close_changed, arm_drain_pause, drain_pause_consumed, drain_owner_entered,
                             arm_local_reset_count, arm_close_reset_join, local_reset_publications,
                             close_failure_entered, drain_join_entered, release_drain](std::string_view name) {
      if (name == "drain_after_owner_claim_before_native_write") {
         if (!arm_drain_pause->load(std::memory_order_acquire) ||
             drain_pause_consumed->exchange(true, std::memory_order_acq_rel)) {
            return false;
         }
         drain_owner_entered->store(true, std::memory_order_release);
         drain_changed->notify_all();
         return true;
      }
      if (name == "drain_after_owner_claim_before_native_write_wait") {
         return !release_drain->load(std::memory_order_acquire);
      }
      if (name == "stream_reset_published") {
         if (arm_local_reset_count->load(std::memory_order_acquire)) {
            local_reset_publications->fetch_add(1, std::memory_order_acq_rel);
            close_changed->notify_all();
         }
         return false;
      }
      if (name == "stream_close_before_state_check") {
         close_failure_entered->store(true, std::memory_order_release);
         close_changed->notify_all();
         return true;
      }
      if (name == "drain_request_before_completion_wait") {
         if (!arm_close_reset_join->load(std::memory_order_acquire) ||
             !close_failure_entered->load(std::memory_order_acquire) ||
             local_reset_publications->load(std::memory_order_acquire) != 1U) {
            return false;
         }
         drain_join_entered->store(true, std::memory_order_release);
         close_changed->notify_all();
         return false;
      }
      return false;
   };
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                       std::chrono::milliseconds{5'000}, "connect first-owner drain-race QUIC session");
   auto inbound =
       get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept first-owner drain-race QUIC session");
   auto stream = as_transport_stream(run_with_deadline(runtime, connection.async_open_stream(),
                                                        std::chrono::milliseconds{5'000},
                                                        "open first-owner drain-race QUIC stream"));

   arm_drain_pause->store(true, std::memory_order_release);
   const auto payload = std::array<std::uint8_t, 1>{0x42};
   run_with_deadline(runtime, stream.async_write(payload), std::chrono::milliseconds{2'000},
                     "queue first-owner drain-race QUIC write");
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(drain_changed->wait_for(lock, std::chrono::seconds{2},
                                             [&] { return drain_owner_entered->load(std::memory_order_acquire); }));
   }

   arm_local_reset_count->store(true, std::memory_order_release);
   arm_close_reset_join->store(true, std::memory_order_release);
   auto close = boost::asio::co_spawn(runtime.context(), stream.async_close(), boost::asio::use_future);
   {
      auto lock = std::unique_lock{*barrier_mutex};
      BOOST_REQUIRE(close_changed->wait_for(lock, std::chrono::seconds{2},
                                             [&] { return close_failure_entered->load(std::memory_order_acquire) &&
                                                          drain_join_entered->load(std::memory_order_acquire) &&
                                                          local_reset_publications->load(std::memory_order_acquire) == 1U; }));
   }
   BOOST_TEST(local_reset_publications->load(std::memory_order_acquire) == 1U);
   BOOST_TEST(static_cast<int>(close.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));

   release_drain->store(true, std::memory_order_release);
   BOOST_REQUIRE(close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(close.get(), std::bad_alloc);
   BOOST_TEST(local_reset_publications->load(std::memory_order_acquire) == 1U);
   BOOST_TEST(connection.metrics().active_streams == 0U);

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{2'000},
                     "close first-owner drain-race QUIC session");
   run_with_deadline(runtime, inbound.async_close(), std::chrono::milliseconds{2'000},
                     "close first-owner drain-race QUIC peer");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_live_stream_survives_connection_facade_drop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto options = loopback_client_options();
   options.connection_lifetime = std::move(lifetime);
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client_stream = stream{};

   {
      auto connection = run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(options)),
                                          std::chrono::milliseconds{5'000}, "connect stream-owned QUIC session");
      client_stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                        "open stream-owned QUIC stream");
   }

   auto server_connection =
       get_with_deadline(accepted, std::chrono::milliseconds{5'000}, "accept stream-owned QUIC session");
   BOOST_TEST(!released.expired());

   const auto request = std::vector<std::uint8_t>{'s', 't', 'r', 'e', 'a', 'm'};
   const auto response = std::vector<std::uint8_t>{'a', 'l', 'i', 'v', 'e'};
   run_with_deadline(runtime, client_stream.async_write(request), std::chrono::milliseconds{5'000},
                     "write after QUIC connection facade drop");
   auto server_stream = run_with_deadline(runtime, server_connection.async_accept_stream(),
                                          std::chrono::milliseconds{5'000}, "accept stream-owned QUIC stream");
   const auto received = run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{5'000},
                                           "read after QUIC connection facade drop");
   BOOST_TEST(received == request, boost::test_tools::per_element());
   run_with_deadline(runtime, server_stream.async_write(response), std::chrono::milliseconds{5'000},
                     "reply after QUIC connection facade drop");
   const auto replied = run_with_deadline(runtime, client_stream.async_read(), std::chrono::milliseconds{5'000},
                                          "receive after QUIC connection facade drop");
   BOOST_TEST(replied == response, boost::test_tools::per_element());
   BOOST_TEST(!released.expired());

   client_stream = {};
   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{5'000}, "release stream-owned QUIC native lifetime");
   BOOST_TEST(released.expired());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_accepted_write_retains_connection_until_delayed_ack_after_facade_drop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto proxy = std::make_shared<udp_fault_proxy>(runtime.context(), server.local_endpoint(), fault_proxy_rules{});
   proxy->start();
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto options = loopback_client_options();
   options.connection_lifetime = std::move(lifetime);
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   const auto payload = std::vector<std::uint8_t>{'a', 'c', 'k'};

   {
      auto connection = run_with_deadline(runtime, client.async_connect(proxy->local_endpoint(), std::move(options)),
                                          std::chrono::milliseconds{10'000}, "connect delayed-ACK QUIC session");
      auto client_stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                             "open delayed-ACK QUIC stream");
      proxy->hold_server_to_client();
      run_with_deadline(runtime, client_stream.async_write(payload), std::chrono::milliseconds{5'000},
                        "accept held-ACK QUIC write");
   }

   auto server_connection =
       get_with_deadline(accepted, std::chrono::milliseconds{10'000}, "accept delayed-ACK QUIC session");
   auto server_stream = run_with_deadline(runtime, server_connection.async_accept_stream(),
                                          std::chrono::milliseconds{5'000}, "accept delayed-ACK QUIC stream");
   const auto received = run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{5'000},
                                           "deliver delayed-ACK QUIC write");
   BOOST_TEST(received == payload, boost::test_tools::per_element());
   run_with_deadline(
       runtime,
       [proxy]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (proxy->server_packets_held() == 0) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "observe held QUIC ACK");
   BOOST_TEST(proxy->server_packets_held() > 0U);
   BOOST_TEST(!released.expired());

   proxy->release_server_to_client();
   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{1});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{2'000}, "release held-ACK QUIC native lifetime");
   BOOST_TEST(released.expired());
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_detached_unacknowledged_write_has_bounded_native_lifetime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto proxy = std::make_shared<udp_fault_proxy>(runtime.context(), server.local_endpoint(), fault_proxy_rules{});
   proxy->start();
   auto client = connector{runtime};
   auto lifetime = std::make_shared<int>(1);
   auto released = std::weak_ptr<void>{lifetime};
   auto options = loopback_client_options();
   options.idle_timeout = std::chrono::milliseconds{30'000};
   options.connection_lifetime = std::move(lifetime);
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   const auto payload = std::vector<std::uint8_t>{'n', 'o', '-', 'a', 'c', 'k'};
   auto owner_released_at = std::chrono::steady_clock::time_point{};

   {
      auto connection = run_with_deadline(runtime, client.async_connect(proxy->local_endpoint(), std::move(options)),
                                          std::chrono::milliseconds{10'000}, "connect unacknowledged QUIC session");
      auto client_stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                             "open unacknowledged QUIC stream");
      proxy->hold_server_to_client();
      run_with_deadline(runtime, client_stream.async_write(payload), std::chrono::milliseconds{5'000},
                        "accept unacknowledged QUIC write");
   }
   owner_released_at = std::chrono::steady_clock::now();

   auto server_connection =
       get_with_deadline(accepted, std::chrono::milliseconds{10'000}, "accept unacknowledged QUIC session");
   auto server_stream = run_with_deadline(runtime, server_connection.async_accept_stream(),
                                          std::chrono::milliseconds{5'000}, "accept unacknowledged QUIC stream");
   const auto received = run_with_deadline(runtime, server_stream.async_read(), std::chrono::milliseconds{5'000},
                                           "deliver unacknowledged QUIC write");
   BOOST_TEST(received == payload, boost::test_tools::per_element());
   BOOST_TEST(!released.expired());

   const auto deadline = owner_released_at + std::chrono::milliseconds{7'000};
   const auto remaining =
       std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
   BOOST_REQUIRE(remaining.count() > 0);
   run_with_deadline(
       runtime,
       [released]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (!released.expired()) {
             timer.expires_after(std::chrono::milliseconds{5});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       remaining, "bound unacknowledged QUIC native lifetime from owner drop");
   const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::now() - owner_released_at);
   BOOST_TEST(elapsed.count() >= 4'000);
   BOOST_TEST(elapsed.count() < 7'000);
   BOOST_TEST(released.expired());
   BOOST_TEST(proxy->server_packets_held() > 0U);
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_verifies_pinned_peer_fingerprint) {
   const auto expected = sha256_fingerprint(test_certificate_der());
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};

   auto connection = forge::asio::blocking::run(
       runtime, client.async_connect(
                    server.local_endpoint(),
                    client_options{
                        .handshake_timeout = std::chrono::milliseconds{5'000},
                        .security = security_options{.verify_peer = true, .expected_sha256_fingerprint = expected},
                    }));

   BOOST_TEST(connection.valid());
   forge::asio::blocking::run(runtime, connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_verifies_ca_certificate_hostname) {
   const auto identity = generate_test_identity("DNS:localhost,IP:127.0.0.1");
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options_value = loopback_server_options();
   server_options_value.certificate_pem = identity.certificate_pem;
   server_options_value.private_key_pem = identity.private_key_pem;
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(server_options_value)};
   auto client = connector{runtime};

   auto client_options_value = loopback_client_options();
   client_options_value.security = security_options{
       .verify_peer = true,
       .trusted_ca_pem = identity.certificate_pem,
   };
   auto connection =
       run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(client_options_value)),
                         std::chrono::milliseconds{5'000}, "CA verified hostname connect");

   BOOST_TEST(connection.valid());
   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000}, "CA verified hostname close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_ca_certificate_hostname_mismatch) {
   const auto identity = generate_test_identity("DNS:example.invalid,IP:127.0.0.2");
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options_value = loopback_server_options();
   server_options_value.handshake_timeout = std::chrono::milliseconds{500};
   server_options_value.certificate_pem = identity.certificate_pem;
   server_options_value.private_key_pem = identity.private_key_pem;
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(server_options_value)};
   auto client = connector{runtime};

   auto client_options_value = loopback_client_options();
   client_options_value.security = security_options{
       .verify_peer = true,
       .trusted_ca_pem = identity.certificate_pem,
   };
   try {
      (void)run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(client_options_value)),
                              std::chrono::milliseconds{5'000}, "CA hostname mismatch connect");
      BOOST_FAIL("expected QUIC hostname verification failure");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::tls_failed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::peer_verification_failed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::handshake_timeout ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::canceled;
      BOOST_TEST_CONTEXT("error kind=" << static_cast<int>(forge::net::quic::exceptions::code_of(error).value())
                                       << " message=" << error.what()) {
         BOOST_TEST(acceptable);
      }
   }
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_accepts_mtls_client_certificate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options_value = loopback_server_options();
   server_options_value.security = security_options{
       .verify_peer = true,
       .expected_sha256_fingerprint = sha256_fingerprint(test_certificate_der()),
   };
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(server_options_value)};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);

   auto client_options_value = loopback_client_options();
   client_options_value.certificate_pem = std::string{test_certificate()};
   client_options_value.private_key_pem = std::string{test_private_key()};
   auto client = connector{runtime};
   auto connection =
       run_with_deadline(runtime, client.async_connect(server.local_endpoint(), std::move(client_options_value)),
                         std::chrono::milliseconds{5'000}, "mTLS client connect");
   auto accepted = get_with_deadline(accept_future, std::chrono::milliseconds{5'000}, "mTLS server accept");

   BOOST_TEST(connection.valid());
   BOOST_TEST(accepted.peer_certificate().has_value());
   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000}, "mTLS client close");
   run_with_deadline(runtime, accepted.async_close(), std::chrono::milliseconds{5'000}, "mTLS server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_missing_mtls_client_certificate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options_value = loopback_server_options();
   server_options_value.handshake_timeout = std::chrono::milliseconds{500};
   server_options_value.security = security_options{.verify_peer = true};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(server_options_value)};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};

   auto client_connected = false;
   try {
      auto connection =
          run_with_deadline(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()),
                            std::chrono::milliseconds{5'000}, "missing mTLS client cert connect");
      client_connected = connection.valid();
      run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000},
                        "close missing-cert client");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::peer_verification_failed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::tls_failed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::handshake_timeout;
      BOOST_TEST(acceptable);
   }
   try {
      (void)get_with_deadline(accept_future, std::chrono::milliseconds{5'000}, "missing-cert server accept");
      BOOST_FAIL("expected missing client certificate to reject server accept");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::peer_verification_failed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::tls_failed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::handshake_timeout ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::connection_closed;
      BOOST_TEST(acceptable);
   }
   (void)client_connected;
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_wrong_peer_fingerprint) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto client = connector{runtime};

   try {
      (void)forge::asio::blocking::run(
          runtime,
          client.async_connect(server.local_endpoint(),
                               client_options{
                                   .handshake_timeout = std::chrono::milliseconds{5'000},
                                   .security =
                                       security_options{
                                           .verify_peer = true,
                                           .expected_sha256_fingerprint =
                                               "0000000000000000000000000000000000000000000000000000000000000000",
                                       },
                               }));
      BOOST_FAIL("expected peer fingerprint rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_connection_close_unblocks_pending_stream_read) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   auto server_connection = accept_future.get();
   auto stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());

   auto read_future = boost::asio::co_spawn(runtime.context(), stream.async_read(), boost::asio::use_future);
   run_with_deadline(runtime, client_connection.async_close(), std::chrono::milliseconds{5'000},
                     "close while stream read is pending");

   try {
      (void)get_with_deadline(read_future, std::chrono::milliseconds{5'000}, "pending stream read after close");
      BOOST_FAIL("expected pending stream read to unblock with a close error");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::connection_closed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::stream_closed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::stream_reset;
      BOOST_TEST(acceptable);
   }
   run_with_deadline(runtime, server_connection.async_close(), std::chrono::milliseconds{5'000},
                     "server close after pending read");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_max_streams_backpressure) {
   auto limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 1, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto client = connector{runtime};
   auto connection = forge::asio::blocking::run(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)));

   auto first = forge::asio::blocking::run(runtime, connection.async_open_stream());
   BOOST_TEST(first.valid());
   try {
      (void)forge::asio::blocking::run(runtime, connection.async_open_stream());
      BOOST_FAIL("expected max streams rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   forge::asio::blocking::run(runtime, connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_allows_new_stream_after_previous_stream_closes) {
   auto client_limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 1, .max_queued_bytes = 16 * 1024 * 1024};
   auto server_limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0},
                          loopback_server_options("forge-p2p/1", server_limits)};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto connection = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", client_limits)),
       std::chrono::milliseconds{5'000}, "stream reuse connect");
   auto server_connection = std::make_shared<forge::net::quic::connection>(
       get_with_deadline(accept_future, std::chrono::milliseconds{5'000}, "stream reuse accept"));

   for (auto index = 0U; index < 128U; ++index) {
      auto server_task = boost::asio::co_spawn(
          runtime.context(),
          [server_connection]() -> boost::asio::awaitable<void> {
             auto accepted = co_await server_connection->async_accept_stream();
             auto framed = framed_stream{std::move(accepted)};
             (void)co_await framed.async_read_frame();
             co_await framed.async_close();
          },
          boost::asio::use_future);

      auto stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                      "open active-limited stream");
      auto framed = framed_stream{std::move(stream)};
      run_with_deadline(runtime, framed.async_write_frame(std::vector<std::uint8_t>{static_cast<std::uint8_t>(index)}),
                        std::chrono::milliseconds{5'000}, "write active-limited stream");
      run_with_deadline(runtime, framed.async_close(), std::chrono::milliseconds{5'000}, "close active-limited stream");
      try {
         (void)run_with_deadline(runtime, framed.async_read_frame(), std::chrono::milliseconds{5'000},
                                 "observe active-limited stream close");
      } catch (const forge::exceptions::base& error) {
         const auto acceptable =
             forge::net::quic::exceptions::code_of(error).value() == exceptions::code::stream_closed ||
             forge::net::quic::exceptions::code_of(error).value() == exceptions::code::connection_closed ||
             forge::net::quic::exceptions::code_of(error).value() == exceptions::code::stream_reset;
         BOOST_TEST(acceptable);
      }
      get_with_deadline(server_task, std::chrono::milliseconds{5'000}, "stream reuse server task");
      const auto closed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
      while (connection.metrics().active_streams != 0U && std::chrono::steady_clock::now() < closed_deadline) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      BOOST_TEST(connection.metrics().active_streams == 0U);
   }

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000},
                     "stream reuse connection close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_canceled_stream_credit_wait_does_not_consume_capacity) {
   auto client_limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 16 * 1024 * 1024};
   auto server_limits =
       transport_limits{.max_connections = 16, .max_streams_per_connection = 1, .max_queued_bytes = 16 * 1024 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0},
                          loopback_server_options("forge-p2p/1", server_limits)};
   auto accept_connection = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto connection = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", client_limits)),
       std::chrono::milliseconds{5'000}, "stream-credit cancellation connect");
   auto server_connection = std::make_shared<forge::net::quic::connection>(
       get_with_deadline(accept_connection, std::chrono::milliseconds{5'000}, "stream-credit cancellation accept"));

   auto first = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                  "stream-credit first open");
   const auto first_payload = std::array<std::uint8_t, 1>{0x01};
   run_with_deadline(runtime, first.async_write(first_payload), std::chrono::milliseconds{5'000},
                     "stream-credit first write");
   auto first_inbound = run_with_deadline(runtime, server_connection->async_accept_stream(),
                                          std::chrono::milliseconds{5'000}, "stream-credit first accept");

   for (auto attempt = 0U; attempt < 64U; ++attempt) {
      auto cancellation = boost::asio::cancellation_signal{};
      auto canceled_open =
          boost::asio::co_spawn(runtime.context(), connection.async_open_stream(),
                                boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
      BOOST_REQUIRE(canceled_open.wait_for(std::chrono::milliseconds{10}) == std::future_status::timeout);

      cancellation.emit(boost::asio::cancellation_type::terminal);
      try {
         (void)get_with_deadline(canceled_open, std::chrono::milliseconds{5'000}, "canceled stream-credit wait");
         BOOST_FAIL("expected canceled stream-credit wait");
      } catch (const forge::exceptions::base& error) {
         BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                    static_cast<int>(exceptions::code::canceled));
      }
   }
   first.cancel();

   auto replacement_accept =
       boost::asio::co_spawn(runtime.context(), server_connection->async_accept_stream(), boost::asio::use_future);
   auto replacement = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                        "replacement after canceled stream-credit wait");
   run_with_deadline(runtime, replacement.async_write(first_payload), std::chrono::milliseconds{5'000},
                     "replacement after canceled stream-credit wait write");
   auto replacement_inbound = get_with_deadline(replacement_accept, std::chrono::milliseconds{5'000},
                                                "replacement after canceled stream-credit wait accept");
   BOOST_TEST(replacement.valid());
   BOOST_TEST(replacement_inbound.valid());

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000},
                     "stream-credit cancellation client close");
   run_with_deadline(runtime, server_connection->async_close(), std::chrono::milliseconds{5'000},
                     "stream-credit cancellation server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_max_queued_bytes_backpressure) {
   auto limits = transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 3};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto client = connector{runtime};
   auto connection = forge::asio::blocking::run(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)));
   auto outbound = forge::asio::blocking::run(runtime, connection.async_open_stream());

   try {
      const auto payload = std::vector<std::uint8_t>{1, 2, 3, 4};
      forge::asio::blocking::run(runtime, outbound.async_write(payload));
      BOOST_FAIL("expected queued bytes rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   BOOST_TEST(connection.metrics().backpressure_rejections >= 1U);
   forge::asio::blocking::run(runtime, connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_stream_cancel_releases_queued_write_budget) {
   auto limits = transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 8};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto proxy = std::make_shared<udp_fault_proxy>(
       runtime.context(), server.local_endpoint(),
       fault_proxy_rules{.server_to_client = fault_rule{.delay = std::chrono::milliseconds{250}}});
   proxy->start();

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto connection = run_with_deadline(
       runtime, client.async_connect(proxy->local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{10'000}, "queued-budget cancel connect");
   auto server_connection =
       get_with_deadline(accept_future, std::chrono::milliseconds{10'000}, "queued-budget cancel accept");

   auto stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                   "queued-budget cancel open stream");
   const auto payload = std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6};
   run_with_deadline(runtime, stream.async_write(payload), std::chrono::milliseconds{5'000},
                     "queued-budget cancel write");
   BOOST_TEST(connection.metrics().queued_bytes >= payload.size());

   stream.cancel();
   run_with_deadline(
       runtime,
       []() -> boost::asio::awaitable<void> {
          auto executor = co_await boost::asio::this_coro::executor;
          auto timer = boost::asio::steady_timer{executor};
          timer.expires_after(std::chrono::milliseconds{50});
          co_await timer.async_wait(boost::asio::use_awaitable);
       }(),
       std::chrono::milliseconds{5'000}, "queued-budget cancel propagation");
   BOOST_TEST(connection.metrics().queued_bytes == 0U);
   BOOST_TEST(connection.metrics().streams_reset >= 1U);

   auto replacement = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                        "queued-budget replacement open stream");
   run_with_deadline(runtime, replacement.async_write(payload), std::chrono::milliseconds{5'000},
                     "queued-budget replacement write");

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000},
                     "queued-budget cancel connection close");
   run_with_deadline(runtime, server_connection.async_close(), std::chrono::milliseconds{5'000},
                     "queued-budget cancel server connection close");
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_large_partial_write_releases_lifetime_after_complete_ack) {
   constexpr auto payload_size = std::size_t{2 * 1024 * 1024};
   auto limits = transport_limits{
       .max_connections = 16,
       .max_streams_per_connection = 16,
       .max_queued_bytes = payload_size,
   };
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto accept_connection = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto connection = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{10'000}, "large ACK connect");
   auto server_connection =
       get_with_deadline(accept_connection, std::chrono::milliseconds{10'000}, "large ACK accept connection");
   auto accept_stream =
       boost::asio::co_spawn(runtime.context(), server_connection.async_accept_stream(), boost::asio::use_future);
   auto outbound = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                     "large ACK open stream");

   auto owner = std::make_shared<int>(42);
   auto weak_owner = std::weak_ptr<int>{owner};
   auto payload = forge::net::transport::chunk{std::vector<std::uint8_t>(payload_size, 0x5a)};
   forge::net::transport::detail::chunk_access::attach_lifetime(payload, owner);
   owner.reset();
   run_with_deadline(runtime, detail::stream_access::async_write_chunk(outbound, std::move(payload)),
                     std::chrono::milliseconds{5'000}, "large ACK write");
   auto inbound = get_with_deadline(accept_stream, std::chrono::milliseconds{5'000}, "large ACK accept stream");

   auto received = std::size_t{};
   while (received < payload_size) {
      received +=
          run_with_deadline(runtime, inbound.async_read(), std::chrono::milliseconds{10'000}, "large ACK read").size();
   }
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while ((!weak_owner.expired() || connection.metrics().queued_bytes != 0U) &&
          std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   BOOST_TEST(weak_owner.expired());
   BOOST_TEST(connection.metrics().queued_bytes == 0U);

   run_with_deadline(runtime, outbound.async_close(), std::chrono::milliseconds{5'000}, "large ACK stream close");
   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000}, "large ACK client close");
   run_with_deadline(runtime, server_connection.async_close(), std::chrono::milliseconds{5'000},
                     "large ACK server close");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_peer_reset_closes_only_the_remote_read_direction) {
   auto limits = transport_limits{.max_connections = 16, .max_streams_per_connection = 16, .max_queued_bytes = 8};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server =
       listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options("forge-p2p/1", limits)};
   auto proxy = std::make_shared<udp_fault_proxy>(
       runtime.context(), server.local_endpoint(),
       fault_proxy_rules{.server_to_client = fault_rule{.delay = std::chrono::milliseconds{250}}});
   proxy->start();

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto connection = run_with_deadline(
       runtime, client.async_connect(proxy->local_endpoint(), loopback_client_options("forge-p2p/1", limits)),
       std::chrono::milliseconds{10'000}, "queued-budget peer-reset connect");
   auto server_connection =
       get_with_deadline(accept_future, std::chrono::milliseconds{10'000}, "queued-budget peer-reset accept");

   auto server_reset = boost::asio::co_spawn(
       runtime.context(),
       [server_connection =
            std::move(server_connection)]() mutable -> boost::asio::awaitable<forge::net::quic::connection> {
          auto inbound = co_await server_connection.async_accept_stream();
          detail::stream_access::cancel_write(inbound);
          auto executor = co_await boost::asio::this_coro::executor;
          auto timer = boost::asio::steady_timer{executor};
          timer.expires_after(std::chrono::milliseconds{100});
          co_await timer.async_wait(boost::asio::use_awaitable);
          co_return std::move(server_connection);
       },
       boost::asio::use_future);

   auto stream = run_with_deadline(runtime, connection.async_open_stream(), std::chrono::milliseconds{5'000},
                                   "queued-budget peer-reset open stream");
   const auto opening_byte = std::array<std::uint8_t, 1>{0x01};
   run_with_deadline(runtime, stream.async_write(opening_byte), std::chrono::milliseconds{5'000},
                     "queued-budget peer-reset publish stream");

   server_connection =
       get_with_deadline(server_reset, std::chrono::milliseconds{10'000}, "queued-budget peer-reset server task");
   run_with_deadline(
       runtime,
       []() -> boost::asio::awaitable<void> {
          auto executor = co_await boost::asio::this_coro::executor;
          auto timer = boost::asio::steady_timer{executor};
          timer.expires_after(std::chrono::milliseconds{350});
          co_await timer.async_wait(boost::asio::use_awaitable);
       }(),
       std::chrono::milliseconds{5'000}, "queued-budget peer-reset propagation");
   BOOST_TEST(connection.metrics().streams_reset >= 1U);
   try {
      (void)run_with_deadline(runtime, stream.async_read(), std::chrono::milliseconds{5'000},
                              "queued-budget peer-reset read");
      BOOST_FAIL("expected remote read direction to report reset");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::stream_reset));
   }

   auto owner = std::make_shared<int>(42);
   auto weak_owner = std::weak_ptr<int>{owner};
   auto payload = forge::net::transport::chunk{std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}};
   forge::net::transport::detail::chunk_access::attach_lifetime(payload, owner);
   owner.reset();
   run_with_deadline(runtime, detail::stream_access::async_write_chunk(stream, std::move(payload)),
                     std::chrono::milliseconds{5'000}, "queued-budget write after remote reset");
   BOOST_TEST(connection.metrics().queued_bytes > 0U);
   BOOST_TEST(!weak_owner.expired());

   run_with_deadline(runtime, connection.async_close(), std::chrono::milliseconds{5'000},
                     "queued-budget peer-reset connection close");
   BOOST_TEST(connection.metrics().queued_bytes == 0U);
   BOOST_TEST(weak_owner.expired());
   run_with_deadline(runtime, server_connection.async_close(), std::chrono::milliseconds{5'000},
                     "queued-budget peer-reset server connection close");
   proxy->stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_loopback_rejects_inbound_packet_queue_overflow) {
   auto server_limits = transport_limits{
       .max_connections = 16,
       .max_streams_per_connection = 16,
       .max_queued_bytes = 16 * 1024 * 1024,
   };
   auto client_limits = transport_limits{
       .max_connections = 16,
       .max_streams_per_connection = 16,
       .max_queued_bytes = 16 * 1024 * 1024,
       .max_inbound_queued_bytes = 1,
       .max_inbound_queued_packets = 16,
   };
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0},
                          loopback_server_options("forge-p2p/1", server_limits)};
   auto client = connector{runtime};

   try {
      (void)run_with_deadline(
          runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", client_limits)),
          std::chrono::milliseconds{5'000}, "inbound overflow connect");
      BOOST_FAIL("expected inbound packet queue overflow to close the connection");
   } catch (const forge::exceptions::base& error) {
      const auto acceptable =
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::connection_closed ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::canceled ||
          forge::net::quic::exceptions::code_of(error).value() == exceptions::code::backpressure_rejected;
      BOOST_TEST(acceptable);
   }

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto valid = run_with_deadline(
       runtime, client.async_connect(server.local_endpoint(), loopback_client_options("forge-p2p/1", server_limits)),
       std::chrono::milliseconds{5'000}, "valid connect after inbound overflow");
   auto accepted = get_with_deadline(accept_future, std::chrono::milliseconds{5'000}, "accept after inbound overflow");
   BOOST_TEST(valid.valid());
   BOOST_TEST(accepted.valid());
   BOOST_TEST(!accepted.metrics().closed);
   run_with_deadline(runtime, valid.async_close(), std::chrono::milliseconds{5'000},
                     "valid close after inbound overflow");
   run_with_deadline(runtime, accepted.async_close(), std::chrono::milliseconds{5'000},
                     "accepted close after inbound overflow");
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_framed_stream_rejects_oversized_remote_frame) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_connection =
       forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoint(), loopback_client_options()));
   auto server_connection = accept_future.get();

   auto server_send = boost::asio::co_spawn(
       runtime.context(),
       [server_connection = std::move(server_connection)]() mutable -> boost::asio::awaitable<void> {
          auto accepted = co_await server_connection.async_accept_stream();
          auto framed = framed_stream{std::move(accepted)};
          (void)co_await framed.async_read_frame();
          const auto payload = std::vector<std::uint8_t>{1, 2, 3, 4};
          co_await framed.async_write_frame(payload);
       },
       boost::asio::use_future);

   auto client_stream = forge::asio::blocking::run(runtime, client_connection.async_open_stream());
   auto framed = framed_stream{std::move(client_stream), frame_codec_options{.max_frame_size = 3}};
   forge::asio::blocking::run(runtime, framed.async_write_frame(std::vector<std::uint8_t>{'g', 'o'}));
   try {
      (void)forge::asio::blocking::run(runtime, framed.async_read_frame());
      BOOST_FAIL("expected oversized frame rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::frame_too_large));
   }
   server_send.get();
   forge::asio::blocking::run(runtime, client_connection.async_close());
   server.stop();
}

BOOST_AUTO_TEST_CASE(quic_listener_async_stop_waits_for_pending_accept_and_concurrent_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto first_stop = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
   auto second_stop = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);

   get_with_deadline_or_stop(runtime, first_stop, std::chrono::milliseconds{5'000}, "first listener async stop");
   get_with_deadline_or_stop(runtime, second_stop, std::chrono::milliseconds{5'000}, "second listener async stop");
   BOOST_REQUIRE(accept_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready);

   try {
      (void)accept_future.get();
      BOOST_FAIL("expected stopped listener to unblock accept with an error");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::quic::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::connection_closed));
   }
}

BOOST_AUTO_TEST_CASE(quic_listener_async_stop_cancels_half_open_handshake_deadline) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = loopback_server_options();
   server_options.handshake_timeout = std::chrono::seconds{30};
   auto server = listener{runtime, endpoint{.host = "127.0.0.1", .port = 0}, std::move(server_options)};
   auto proxy = std::make_shared<udp_fault_proxy>(runtime.context(), server.local_endpoint(),
                                                  fault_proxy_rules{.server_to_client = fault_rule{.drop_every = 1}});
   proxy->start();

   auto accept_future = boost::asio::co_spawn(runtime.context(), server.async_accept(), boost::asio::use_future);
   auto client = connector{runtime};
   auto client_options = loopback_client_options();
   client_options.connect_timeout = std::chrono::seconds{30};
   client_options.handshake_timeout = std::chrono::seconds{30};
   auto connect_future = boost::asio::co_spawn(runtime.context(),
                                               client.async_connect(proxy->local_endpoint(), std::move(client_options)),
                                               boost::asio::use_future);

   run_with_deadline(
       runtime,
       [proxy]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          while (proxy->server_packets_received() == 0) {
             timer.expires_after(std::chrono::milliseconds{5});
             co_await timer.async_wait(boost::asio::use_awaitable);
          }
       }(),
       std::chrono::milliseconds{5'000}, "half-open server handshake");

   const auto started = std::chrono::steady_clock::now();
   run_with_deadline(runtime, server.async_stop(), std::chrono::milliseconds{2'000}, "half-open listener async stop");
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   BOOST_TEST(elapsed < std::chrono::milliseconds{2'000});

   proxy->stop();
   client.cancel();
   BOOST_CHECK_THROW(
       get_with_deadline_or_stop(runtime, accept_future, std::chrono::milliseconds{2'000}, "half-open listener accept"),
       forge::exceptions::base);
   BOOST_CHECK_THROW(
       get_with_deadline_or_stop(runtime, connect_future, std::chrono::milliseconds{2'000}, "half-open client connect"),
       forge::exceptions::base);
}

} // namespace
} // namespace forge::net::quic
