module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <string>

export module forge.api.chain.runtime;

export import forge.api.chain.destination;

import forge.api.core.binding;
import forge.chain.protocol.transaction_query;
import forge.chain.transaction.builder;

export namespace forge::api::chain {

class runtime : public core::contract<runtime> {
 public:
   virtual ~runtime() = default;

   virtual boost::asio::awaitable<::forge::chain::transaction::builder>
   begin(destination target) = 0;

   virtual boost::asio::awaitable<::forge::chain::protocol::transaction_id>
   submit(destination target, core::api_ref contract, std::string method,
          ::forge::chain::transaction::unsigned_transaction transaction) = 0;

   virtual boost::asio::awaitable<::forge::chain::protocol::transaction_status_response>
   status(destination target, ::forge::chain::protocol::transaction_id id) = 0;
};

} // namespace forge::api::chain

FORGE_EXPORT_API(::forge::api::chain::runtime, FORGE_API_CONTRACT("forge.api.chain.runtime", 1, 0))
