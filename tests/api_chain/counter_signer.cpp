module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <memory>
#include <utility>

module test.api_chain.counter_client;

import forge.chain.api.transaction_signer;
import forge.chain.transaction.signing;

namespace demo {
namespace protocol = forge::chain::protocol;

class signer final : public forge::chain::api::transaction_signer {
 public:
   forge::crypto::asymmetric::private_key key = forge::crypto::asymmetric::private_key::generate();
   std::shared_ptr<std::atomic_uint> calls = std::make_shared<std::atomic_uint>(0);

   boost::asio::awaitable<forge::chain::transaction::prepared_transaction>
   sign(forge::chain::transaction::unsigned_transaction unsigned_tx, forge::api::auth::authenticated_caller) override {
      ++*calls;
      protocol::signed_transaction signed_tx;
      static_cast<protocol::transaction&>(signed_tx) = std::move(unsigned_tx.value);
      signed_tx.context_free_data = std::move(unsigned_tx.context_free_data);
      signed_tx.signatures.push_back(
          key.sign_digest(protocol::signature_digest(unsigned_tx.chain, signed_tx, signed_tx.context_free_data)));
      co_return forge::chain::transaction::pack(std::move(signed_tx), unsigned_tx.compression);
   }
};

signer_fixture make_signer() {
   auto signing = std::make_shared<signer>();
   return {signing->key.get_public_key(), signing->calls, [signing](forge::app::application_context& ctx) {
              ctx.apis().install<forge::chain::api::transaction_signer>(signing);
           }};
}

} // namespace demo
