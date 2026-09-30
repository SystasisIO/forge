module;

#include <boost/asio/awaitable.hpp>
#include <vector>

export module forge.plugins.chain.signer.block_execution_handler;

export import forge.chain.protocol.block_signing;

export namespace forge::plugins::chain::signer {

// Local composition extension, never a wire API. Called only after admission,
// policy and all provider identity checks. The lazy, move-only operation signs
// the already selected keys; an implementation may instead return a cached result.
// Implementations must finish their operation within execute(), not detach it.
// After execute() completes or fails the operation expires; retaining an unused
// operation cannot retain admission or authorize a later provider call.
// Forge validates every returned signature, including cached results.
class block_execution_handler {
 public:
   virtual ~block_execution_handler() = default;

   virtual boost::asio::awaitable<std::vector<forge::chain::protocol::signature>>
   execute(forge::chain::protocol::block_sign_request request,
           boost::asio::awaitable<std::vector<forge::chain::protocol::signature>> signing) = 0;
};

} // namespace forge::plugins::chain::signer
