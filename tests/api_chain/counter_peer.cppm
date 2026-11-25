module;

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

export module test.api_chain.counter_peer;

import package.chain_api_component.p2p_runtime;
import forge.api.core.registry;
import forge.chain.api.finality;
import forge.chain.protocol.transaction;
import forge.crypto.asymmetric;

export namespace demo {

// These three services run on the peer. Finality is anchored to the fixture's
// block headers. The client verifies transaction inclusion with Forge.
class counter_peer {
 public:
   explicit counter_peer(forge::crypto::asymmetric::public_key signer, bool accept_pending = false);
   ~counter_peer();

   void install(forge::api::core::installer apis) const;
   package_chain_api_component::p2p_publication_callbacks publication() const;
   std::shared_ptr<forge::chain::api::finality_verifier> finality() const;
   void allow_execution();
   void include_first_submission(bool publish_receipt);
   bool wait_for_submissions(std::size_t count, std::chrono::seconds timeout) const;
   bool wait_for_proof(std::chrono::seconds timeout) const;
   std::vector<forge::chain::protocol::packed_transaction> submissions() const;
   std::uint64_t value() const;

 private:
   struct state;
   std::shared_ptr<state> state_;
};

} // namespace demo
