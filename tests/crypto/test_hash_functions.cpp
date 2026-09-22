#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

import forge.codec.hex;
import forge.crypto.digest.ripemd160;
import forge.crypto.digest.sha1;
import forge.crypto.digest.sha224;
import forge.crypto.digest.sha256;
import forge.crypto.digest.sha3;
import forge.crypto.digest.sha512;
import forge.crypto.digest.shake128;
import forge.core.utility;
import forge.exceptions;

using namespace forge;

BOOST_AUTO_TEST_SUITE(hash_functions)

BOOST_AUTO_TEST_CASE(shake128_variable_output_vectors_and_boundaries) {
   const auto empty = forge::crypto::digest::shake128({}, 32);
   BOOST_CHECK_EQUAL(forge::codec::hex::encode(empty),
                     "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");
   const auto input = std::array<std::uint8_t, 3>{'a', 'b', 'c'};
   const auto abc = forge::crypto::digest::shake128(input, 16);
   BOOST_CHECK_EQUAL(forge::codec::hex::encode(abc), "5881092dd818bf5cf8a3ddb793fbcba7");
   const auto full = forge::crypto::digest::shake128(input, 337);
   // SHAKE128's 168-byte rate must not impose a fixed digest size.
   for (const auto size : {0U, 1U, 16U, 32U, 167U, 168U, 169U, 336U, 337U}) {
      const auto output = forge::crypto::digest::shake128(input, size);
      BOOST_CHECK_EQUAL(output.size(), size);
      BOOST_CHECK(std::equal(output.begin(), output.end(), full.begin()));
   }
   BOOST_CHECK(forge::crypto::digest::shake128({}, 0).empty());
   BOOST_CHECK_THROW(static_cast<void>(forge::crypto::digest::shake128(
                         input, std::numeric_limits<std::size_t>::max())),
                     forge::crypto::digest::exceptions::invalid_size);
}

BOOST_AUTO_TEST_CASE(evp_digest_vectors) try {
   const std::string empty;
   const std::string abc = "abc";

   BOOST_CHECK_EQUAL(forge::crypto::digest::sha1::hash(empty).str(), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
   BOOST_CHECK_EQUAL(forge::crypto::digest::sha1::hash(abc).str(), "a9993e364706816aba3e25717850c26c9cd0d89d");

   BOOST_CHECK_EQUAL(forge::crypto::digest::sha224::hash(empty).str(),
                     "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f");
   BOOST_CHECK_EQUAL(forge::crypto::digest::sha224::hash(abc).str(),
                     "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");

   BOOST_CHECK_EQUAL(forge::crypto::digest::sha256::hash(empty).str(),
                     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
   BOOST_CHECK_EQUAL(forge::crypto::digest::sha256::hash(abc).str(),
                     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

   BOOST_CHECK_EQUAL(forge::crypto::digest::sha512::hash(empty).str(),
                     "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce4"
                     "7d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
   BOOST_CHECK_EQUAL(forge::crypto::digest::sha512::hash(abc).str(),
                     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a219"
                     "2992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

   BOOST_CHECK_EQUAL(forge::crypto::digest::ripemd160::hash(empty).str(), "9c1185a5c5e9fc54612808977ee8f548b2258d31");
   BOOST_CHECK_EQUAL(forge::crypto::digest::ripemd160::hash(abc).str(), "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sha3) try {

   using test_sha3 = std::tuple<std::string, std::string>;
   const std::vector<test_sha3> tests{// test
                                      {
                                          "",
                                          "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
                                      },

                                      // test
                                      {
                                          "abc",
                                          "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
                                      },

                                      // test
                                      {
                                          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                                          "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376",
                                      }};

   for (const auto& test : tests) {
      BOOST_CHECK_EQUAL(forge::crypto::digest::sha3::hash(std::get<0>(test), true).str(), std::get<1>(test));
   }
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(keccak256) try {

   using test_keccak256 = std::tuple<std::string, std::string>;
   const std::vector<test_keccak256> tests{// test
                                           {
                                               "",
                                               "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
                                           },

                                           // test
                                           {
                                               "abc",
                                               "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
                                           },

                                           // test
                                           {
                                               "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                                               "45d3b367a6904e6e8d502ee04999a7c27647f91fa845d456525fd352ae3d7371",
                                           }};

   for (const auto& test : tests) {
      BOOST_CHECK_EQUAL(forge::crypto::digest::sha3::hash(std::get<0>(test), false).str(), std::get<1>(test));
   }
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
