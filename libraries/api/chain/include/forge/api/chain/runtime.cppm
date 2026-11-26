module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

export module forge.api.chain.runtime;

export import forge.api.chain.destination;
export import forge.api.chain.read_snapshot;

import forge.api.core.binding;
import forge.chain.protocol.block_query;
import forge.chain.protocol.transaction_query;
import forge.chain.transaction.builder;

export namespace forge::api::chain {

using block_reference = std::variant<::forge::chain::protocol::block_id, std::uint32_t>;

struct action_options {
   ::forge::chain::protocol::block_id reference_block;
   ::forge::chain::protocol::time_point_sec expiration;
};

class runtime : public core::contract<runtime> {
 public:
   virtual ~runtime() = default;

   virtual boost::asio::awaitable<::forge::chain::protocol::block_id> finalized(destination target) = 0;

   [[nodiscard]] virtual destination read_target(const core::api_ref& contract) const = 0;
   [[nodiscard]] virtual bool same_read_profile(const std::optional<core::api_ref>& requested,
                                                const core::api_ref& contract) const = 0;

   using read_handler = std::function<boost::asio::awaitable<void>(const read_snapshot&)>;
   virtual boost::asio::awaitable<void>
   read(destination target, std::optional<block_reference> block, read_handler handler,
        std::optional<core::api_ref> read_contract = {}) = 0;

   virtual boost::asio::awaitable<::forge::chain::protocol::block_response>
   get_block(destination target, ::forge::chain::protocol::block_id block) = 0;

   virtual boost::asio::awaitable<::forge::chain::transaction::builder>
   begin(destination target, std::optional<action_options> options = {}) = 0;

   virtual boost::asio::awaitable<::forge::chain::protocol::transaction_id>
   submit(destination target, core::api_ref contract, std::string method,
          ::forge::chain::transaction::unsigned_transaction transaction) = 0;

   virtual boost::asio::awaitable<::forge::chain::protocol::transaction_status_response>
   status(destination target, ::forge::chain::protocol::transaction_id id) = 0;
};

} // namespace forge::api::chain

FORGE_EXPORT_API(::forge::api::chain::runtime, FORGE_API_CONTRACT("forge.api.chain.runtime", 1, 0))
