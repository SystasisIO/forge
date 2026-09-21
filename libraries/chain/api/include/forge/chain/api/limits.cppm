module;

#include <forge/exceptions/macros.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

export module forge.chain.api.limits;

export import forge.chain.api.exceptions;
export import forge.api.core.descriptor;
export import forge.chain.protocol.admin;
export import forge.chain.protocol.audit;
export import forge.chain.protocol.block_query;
export import forge.chain.protocol.state_query;
export import forge.chain.protocol.transaction_query;

import forge.raw.raw;

export namespace forge::chain::api {

template <typename Value>
void require_request_within_limits(const Value& value, const protocol::service_limits& limits) {
   const auto bytes = forge::raw::pack_size(value);
   if (bytes > limits.max_request_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API request exceeds the configured byte limit",
                            forge::exceptions::ctx("bytes", bytes),
                            forge::exceptions::ctx("limit", limits.max_request_bytes));
   }
}

template <typename Value>
void require_response_within_limits(const Value& value, const protocol::service_limits& limits) {
   const auto bytes = forge::raw::pack_size(value);
   if (bytes > limits.max_response_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API response exceeds the configured byte limit",
                            forge::exceptions::ctx("bytes", bytes),
                            forge::exceptions::ctx("limit", limits.max_response_bytes));
   }
   if constexpr (requires { value.audit; }) {
      if (value.audit) {
         const auto proof_bytes = forge::raw::pack_size(*value.audit);
         if (proof_bytes > limits.max_proof_bytes) {
            FORGE_THROW_EXCEPTION(
                exceptions::resource_exhausted, "chain API response proof exceeds the configured byte limit",
                forge::exceptions::ctx("bytes", proof_bytes), forge::exceptions::ctx("limit", limits.max_proof_bytes));
         }
      }
   }
}

template <typename Response, typename Request>
void require_response_within_limits(const Response& response, const Request&, const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
}

void require_request_within_limits(const protocol::block_range_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::protocol_features_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::producers_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::producer_rewards_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::account_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::code_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::permission_links_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::account_changes_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::table_rows_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::table_changes_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::table_scope_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::scheduled_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::authorizers_request& value, const protocol::service_limits& limits);
void require_request_within_limits(const protocol::transaction_submit_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::transaction_submit_batch_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::transaction_await_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::ram_corrections_request& value,
                                   const protocol::service_limits& limits);
void require_request_within_limits(const protocol::unapplied_transactions_request& value,
                                   const protocol::service_limits& limits);

void require_response_within_limits(const protocol::block_range_response& response,
                                    const protocol::block_range_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::protocol_features_response& response,
                                    const protocol::protocol_features_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::producers_response& response,
                                    const protocol::producers_request& request, const protocol::service_limits& limits);
void require_response_within_limits(const protocol::producer_rewards_response& response,
                                    const protocol::producer_rewards_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::account_changes_response& response,
                                    const protocol::account_changes_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::permission_links_response& response,
                                    const protocol::permission_links_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::table_rows_response& response,
                                    const protocol::table_rows_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::table_changes_response& response,
                                    const protocol::table_changes_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::table_scope_response& response,
                                    const protocol::table_scope_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::scheduled_response& response,
                                    const protocol::scheduled_request& request, const protocol::service_limits& limits);
void require_response_within_limits(const protocol::authorizers_response& response,
                                    const protocol::authorizers_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::ram_corrections_response& response,
                                    const protocol::ram_corrections_request& request,
                                    const protocol::service_limits& limits);
void require_response_within_limits(const protocol::unapplied_transactions_response& response,
                                    const protocol::unapplied_transactions_request& request,
                                    const protocol::service_limits& limits);

void require_transaction_batch_response_within_limits(
    const std::vector<protocol::transaction_submit_response>& responses, std::size_t request_count,
    const protocol::service_limits& limits);

[[nodiscard]] forge::api::core::descriptor with_service_limits(forge::api::core::descriptor value,
                                                               protocol::service_limits limits);

template <typename Interface>
[[nodiscard]] forge::api::core::descriptor limited_descriptor(protocol::service_limits limits) {
   return with_service_limits(Interface::describe(), limits);
}

} // namespace forge::chain::api
