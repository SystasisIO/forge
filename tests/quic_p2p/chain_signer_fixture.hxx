#pragma once

namespace forge::tests::p2p {

// Keep plugin composition and the P2P node in separate translation units. The
// test still calls the real plugin over real transport; no signer is mocked.
class chain_signer_fixture {
 public:
   chain_signer_fixture(forge::asio::runtime& runtime, forge::chain::protocol::chain_id chain,
                        forge::crypto::asymmetric::private_key key, forge::crypto::digest::sha256 caller);
   ~chain_signer_fixture();
   forge::api::core::registry& apis();

 private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace forge::tests::p2p
