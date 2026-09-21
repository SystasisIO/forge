module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module forge.chain.api.limits;

import forge.chain.api.table_key;
import forge.chain.protocol.entity_selector;
import forge.raw.exceptions;
import forge.raw.raw;

namespace forge::chain::api {
namespace {

void require_page(std::uint32_t value, std::uint32_t limit, std::string_view name, bool allow_zero = false) {
   if (!allow_zero && value == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "chain API page limit must be positive",
                            forge::exceptions::ctx("field", name));
   }
   if (value > limit) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API page limit exceeds the configured maximum",
                            forge::exceptions::ctx("field", name), forge::exceptions::ctx("value", value),
                            forge::exceptions::ctx("limit", limit));
   }
}

template <typename Value> void require_packed_request(const Value& value, const protocol::service_limits& limits) {
   require_request_within_limits<Value>(value, limits);
}

void require_items(std::size_t count, std::uint32_t requested, const protocol::service_limits& limits,
                   std::string_view field) {
   const auto allowed = std::min<std::size_t>(requested, limits.max_page_size);
   if (count > allowed) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API response item count exceeds the request limit",
                            forge::exceptions::ctx("field", field), forge::exceptions::ctx("count", count),
                            forge::exceptions::ctx("limit", allowed));
   }
}

void require_nonempty_next(const std::optional<protocol::bytes>& next) {
   if (next && next->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner returned an empty continuation cursor");
   }
}

template <typename Request> void require_account_selector(const Request& value) {
   if (!protocol::selects_exactly_one(static_cast<const protocol::account_selector&>(value))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request,
                            "chain API account selector must contain exactly one of id or key");
   }
}

template <typename Batch>
void require_mutations(const std::vector<Batch>& blocks, std::uint32_t requested,
                       const protocol::service_limits& limits) {
   auto count = std::size_t{};
   const auto allowed = std::min<std::size_t>(requested, limits.max_page_size);
   for (const auto& block : blocks) {
      if (block.mutations.size() > allowed - std::min(count, allowed)) {
         require_items(allowed + 1U, requested, limits, "mutations");
      }
      count += block.mutations.size();
   }
   require_items(count, requested, limits, "mutations");
}

forge::raw::unpack_limits allocation_limits(const forge::api::core::bytes& payload,
                                            const protocol::service_limits& limits, std::uint32_t byte_limit,
                                            std::uint32_t first_container_limit = forge::raw::max_array_elements,
                                            std::optional<std::uint32_t> total_container_limit = std::nullopt) {
   const auto total_limit = total_container_limit.value_or(limits.max_container_elements);
   return forge::raw::unpack_limits{
       .max_container_elements =
           static_cast<std::uint32_t>(std::min<std::size_t>(limits.max_container_elements, payload.size())),
       .max_total_container_elements = static_cast<std::uint32_t>(std::min<std::size_t>(total_limit, payload.size())),
       .max_bytes = static_cast<std::uint32_t>(std::min<std::size_t>(byte_limit, payload.size())),
       .first_container_elements = first_container_limit,
   };
}

forge::raw::unpack_limits request_allocation_limits(std::string_view api, std::string_view method,
                                                    const forge::api::core::bytes& payload,
                                                    const protocol::service_limits& limits) {
   if (api == "forge.chain.api.submission" && method == "submit_batch") {
      return allocation_limits(payload, limits, limits.max_request_bytes, limits.max_transaction_batch_size);
   }
   if (api == "forge.chain.api.state" && (method == "get_table_changes" || method == "get_account_changes")) {
      return allocation_limits(payload, limits, limits.max_request_bytes, limits.max_state_batch_size);
   }
   if (api == "forge.chain.api.state" && method == "get_accounts_by_authorizers") {
      return allocation_limits(payload, limits, limits.max_request_bytes, limits.max_state_batch_size,
                               limits.max_state_batch_size);
   }
   return allocation_limits(payload, limits, limits.max_request_bytes);
}

forge::raw::unpack_limits response_allocation_limits(std::string_view api, std::string_view method,
                                                     const forge::api::core::bytes& payload,
                                                     const protocol::service_limits& limits) {
   if (api == "forge.chain.api.submission" && method == "submit_batch") {
      return allocation_limits(payload, limits, limits.max_response_bytes, limits.max_transaction_batch_size);
   }
   return allocation_limits(payload, limits, limits.max_response_bytes);
}

template <typename Decoder>
void decode_request(const Decoder& decoder, std::string_view api, std::string_view method,
                    const forge::api::core::bytes& payload, const protocol::service_limits& limits) {
   if (!decoder) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API method has no canonical request decoder",
                            forge::exceptions::ctx("api", api), forge::exceptions::ctx("method", method));
   }
   try {
      decoder(payload, request_allocation_limits(api, method, payload, limits));
   } catch (const forge::raw::exceptions::allocation_limit& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API request exceeds allocation limits",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "chain API request payload is malformed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "chain API request payload is malformed");
   }
}

template <typename Decoder>
void decode_response(const Decoder& decoder, std::string_view api, std::string_view method,
                     const forge::api::core::bytes& payload, const protocol::service_limits& limits) {
   if (!decoder) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API method has no canonical response decoder",
                            forge::exceptions::ctx("api", api), forge::exceptions::ctx("method", method));
   }
   try {
      decoder(payload, response_allocation_limits(api, method, payload, limits));
   } catch (const forge::raw::exceptions::allocation_limit& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API response exceeds allocation limits",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner produced a malformed response payload",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner produced a malformed response payload");
   }
}

template <typename Value>
Value unpack_request(const forge::api::core::bytes& payload, const protocol::service_limits& limits,
                     std::uint32_t first_container_limit = forge::raw::max_array_elements,
                     std::optional<std::uint32_t> total_container_limit = std::nullopt) {
   try {
      return forge::raw::unpack_exact<Value>(payload, allocation_limits(payload, limits, limits.max_request_bytes,
                                                                        first_container_limit, total_container_limit));
   } catch (const forge::raw::exceptions::allocation_limit& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API request exceeds allocation limits",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "chain API request payload is malformed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "chain API request payload is malformed");
   }
}

template <typename Value>
Value unpack_response(const forge::api::core::bytes& payload, const protocol::service_limits& limits,
                      std::uint32_t first_container_limit = forge::raw::max_array_elements) {
   try {
      return forge::raw::unpack_exact<Value>(
          payload, allocation_limits(payload, limits, limits.max_response_bytes, first_container_limit));
   } catch (const forge::raw::exceptions::allocation_limit& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API response exceeds allocation limits",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner produced a malformed response payload",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner produced a malformed response payload");
   }
}

protocol::audited_response unpack_audited_prefix(const forge::api::core::bytes& payload,
                                                 const protocol::service_limits& limits) {
   try {
      return forge::raw::unpack<protocol::audited_response>(
          payload, allocation_limits(payload, limits, limits.max_response_bytes));
   } catch (const forge::raw::exceptions::allocation_limit& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API audit response exceeds allocation limits",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner produced a malformed audited response",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API owner produced a malformed audited response");
   }
}

std::optional<std::uint32_t> require_method_request(std::string_view api, std::string_view method,
                                                    const forge::api::core::bytes& payload,
                                                    const protocol::service_limits& limits) {
   if (payload.size() > limits.max_request_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API request exceeds the configured byte limit",
                            forge::exceptions::ctx("bytes", payload.size()),
                            forge::exceptions::ctx("limit", limits.max_request_bytes));
   }

   if (api == "forge.chain.api.block") {
      if (method == "get_canonical_range") {
         const auto request = unpack_request<protocol::block_range_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_activated_protocol_features") {
         const auto request = unpack_request<protocol::protocol_features_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_producers") {
         const auto request = unpack_request<protocol::producers_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_producer_rewards") {
         require_request_within_limits(unpack_request<protocol::producer_rewards_request>(payload, limits), limits);
         return 1U;
      }
   } else if (api == "forge.chain.api.state") {
      if (method == "get_account") {
         require_request_within_limits(unpack_request<protocol::account_request>(payload, limits), limits);
      }
      if (method == "get_code") {
         require_request_within_limits(unpack_request<protocol::code_request>(payload, limits), limits);
      }
      if (method == "get_permission_links") {
         const auto request = unpack_request<protocol::permission_links_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_table_changes") {
         const auto request =
             unpack_request<protocol::table_changes_request>(payload, limits, limits.max_state_batch_size);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_account_changes") {
         const auto request =
             unpack_request<protocol::account_changes_request>(payload, limits, limits.max_state_batch_size);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_table_rows") {
         const auto request = unpack_request<protocol::table_rows_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_table_scope") {
         const auto request = unpack_request<protocol::table_scope_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_scheduled_transactions") {
         const auto request = unpack_request<protocol::scheduled_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "get_accounts_by_authorizers") {
         const auto request = unpack_request<protocol::authorizers_request>(
             payload, limits, limits.max_state_batch_size, limits.max_state_batch_size);
         require_request_within_limits(request, limits);
         return request.limit;
      }
   } else if (api == "forge.chain.api.admin") {
      if (method == "account_ram_corrections") {
         const auto request = unpack_request<protocol::ram_corrections_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
      if (method == "unapplied_transactions") {
         const auto request = unpack_request<protocol::unapplied_transactions_request>(payload, limits);
         require_request_within_limits(request, limits);
         return request.limit;
      }
   } else if (api == "forge.chain.api.transaction" && method == "await_transaction") {
      require_request_within_limits(unpack_request<protocol::transaction_await_request>(payload, limits), limits);
   } else if (api == "forge.chain.api.submission") {
      if (method == "submit") {
         require_request_within_limits(unpack_request<protocol::transaction_submit_request>(payload, limits), limits);
      } else if (method == "submit_batch") {
         const auto request = unpack_request<protocol::transaction_submit_batch_request>(
             payload, limits, limits.max_transaction_batch_size);
         require_request_within_limits(request, limits);
         return static_cast<std::uint32_t>(request.transactions.size());
      }
   }
   return std::nullopt;
}

void require_method_response(std::string_view api, std::string_view method, bool audited_response,
                             const forge::api::core::bytes& payload,
                             const std::optional<std::uint32_t>& requested_items,
                             const protocol::service_limits& limits) {
   if (payload.size() > limits.max_response_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "chain API response exceeds the configured byte limit",
                            forge::exceptions::ctx("bytes", payload.size()),
                            forge::exceptions::ctx("limit", limits.max_response_bytes));
   }
   if (audited_response) {
      require_response_within_limits(unpack_audited_prefix(payload, limits), limits);
   }
   if (!requested_items) {
      return;
   }

   if (api == "forge.chain.api.block") {
      if (method == "get_canonical_range") {
         require_items(unpack_response<protocol::block_range_response>(payload, limits).blocks.size(), *requested_items,
                       limits, "blocks");
      } else if (method == "get_activated_protocol_features") {
         require_items(unpack_response<protocol::protocol_features_response>(payload, limits).features.size(),
                       *requested_items, limits, "features");
      } else if (method == "get_producers") {
         const auto response = unpack_response<protocol::producers_response>(payload, limits);
         require_nonempty_next(response.next);
         require_items(response.rows.size(), *requested_items, limits, "rows");
      } else if (method == "get_producer_rewards") {
         const auto response = unpack_response<protocol::producer_rewards_response>(payload, limits);
         require_items(response.system.rows.size(), *requested_items, limits, "eosio.system rows");
         require_items(response.bpay.rows.size(), *requested_items, limits, "eosio.bpay rows");
      }
   } else if (api == "forge.chain.api.state") {
      if (method == "get_table_changes") {
         const auto response = unpack_response<protocol::table_changes_response>(payload, limits);
         require_nonempty_next(response.next);
         require_mutations(response.blocks, *requested_items, limits);
      } else if (method == "get_account_changes") {
         const auto response = unpack_response<protocol::account_changes_response>(payload, limits);
         require_nonempty_next(response.next);
         require_mutations(response.blocks, *requested_items, limits);
      } else if (method == "get_table_rows") {
         require_items(unpack_response<protocol::table_rows_response>(payload, limits).rows.size(), *requested_items,
                       limits, "rows");
      } else if (method == "get_table_scope") {
         require_items(unpack_response<protocol::table_scope_response>(payload, limits).tables.size(), *requested_items,
                       limits, "tables");
      } else if (method == "get_permission_links") {
         const auto response = unpack_response<protocol::permission_links_response>(payload, limits);
         require_nonempty_next(response.next);
         require_items(response.links.size(), *requested_items, limits, "links");
      } else if (method == "get_scheduled_transactions") {
         const auto response = unpack_response<protocol::scheduled_response>(payload, limits);
         require_nonempty_next(response.next);
         require_items(response.transactions.size(), *requested_items, limits, "transactions");
      } else if (method == "get_accounts_by_authorizers") {
         const auto response = unpack_response<protocol::authorizers_response>(payload, limits);
         require_nonempty_next(response.next);
         require_items(response.accounts.size(), *requested_items, limits, "accounts");
      }
   } else if (api == "forge.chain.api.submission" && method == "submit_batch") {
      require_transaction_batch_response_within_limits(
          unpack_response<std::vector<protocol::transaction_submit_response>>(payload, limits,
                                                                              limits.max_transaction_batch_size),
          *requested_items, limits);
   } else if (api == "forge.chain.api.admin") {
      if (method == "account_ram_corrections") {
         require_items(unpack_response<protocol::ram_corrections_response>(payload, limits).rows.size(),
                       *requested_items, limits, "rows");
      } else if (method == "unapplied_transactions") {
         require_items(unpack_response<protocol::unapplied_transactions_response>(payload, limits).transactions.size(),
                       *requested_items, limits, "transactions");
      }
   }
}

} // namespace

void require_request_within_limits(const protocol::block_range_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit");
}

void require_request_within_limits(const protocol::protocol_features_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit");
   if (value.lower_bound && value.upper_bound && *value.upper_bound < *value.lower_bound) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "protocol feature lower bound exceeds its upper bound");
   }
}

void require_request_within_limits(const protocol::producers_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit", true);
   if (value.cursor && value.cursor->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "producer cursor must not be empty");
   }
}

void require_request_within_limits(const protocol::producer_rewards_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   if (!static_cast<bool>(value.producer)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "producer rewards require a producer account");
   }
}

void require_request_within_limits(const protocol::account_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_account_selector(value);
}

void require_request_within_limits(const protocol::code_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_account_selector(value);
}

void require_request_within_limits(const protocol::permission_links_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_account_selector(value);
   require_page(value.limit, limits.max_page_size, "limit", true);
   if (value.cursor && value.cursor->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "permission links cursor must not be empty");
   }
}

void require_request_within_limits(const protocol::account_changes_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit");
   if (value.from_block > value.to_block) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "account changes interval is reversed");
   }
   if (value.accounts.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "account changes require at least one account");
   }
   if (value.accounts.size() > limits.max_state_batch_size) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted,
                            "account changes account count exceeds the configured maximum",
                            forge::exceptions::ctx("count", value.accounts.size()),
                            forge::exceptions::ctx("limit", limits.max_state_batch_size));
   }
   if (!std::ranges::is_sorted(value.accounts) || std::ranges::adjacent_find(value.accounts) != value.accounts.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "account changes accounts must be sorted and unique");
   }
   if (value.cursor && value.cursor->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "account changes cursor must not be empty");
   }
}

void require_request_within_limits(const protocol::table_rows_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit", true);
   validate_table_rows_request(value);
}

void require_request_within_limits(const protocol::table_changes_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit");
   if (value.from_block > value.to_block) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "table changes interval is reversed");
   }
   if (value.tables.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "table changes require at least one selector");
   }
   if (value.tables.size() > limits.max_state_batch_size) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted,
                            "table changes selector count exceeds the configured maximum",
                            forge::exceptions::ctx("count", value.tables.size()),
                            forge::exceptions::ctx("limit", limits.max_state_batch_size));
   }
   if (!std::ranges::is_sorted(value.tables) || std::ranges::adjacent_find(value.tables) != value.tables.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "table changes selectors must be sorted and unique");
   }
   if (value.cursor && value.cursor->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "table changes cursor must not be empty");
   }
}

void require_request_within_limits(const protocol::table_scope_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit", true);
}

void require_request_within_limits(const protocol::scheduled_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit", true);
   if (value.lower_bound && value.upper_bound && *value.upper_bound < *value.lower_bound) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "scheduled transaction lower bound exceeds its upper bound");
   }
   if (value.cursor && value.cursor->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "scheduled transaction cursor must not be empty");
   }
}

void require_request_within_limits(const protocol::authorizers_request& value, const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit");
   const auto inputs = value.accounts.size() + value.keys.size();
   if (inputs > limits.max_state_batch_size) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "authorizer input count exceeds the configured maximum",
                            forge::exceptions::ctx("count", inputs),
                            forge::exceptions::ctx("limit", limits.max_state_batch_size));
   }
   if (value.cursor && value.cursor->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "authorizer cursor must not be empty");
   }
}

void require_request_within_limits(const protocol::transaction_submit_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   if (value.timeout_ms == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "transaction submit timeout must be positive");
   }
   if (value.timeout_ms > limits.max_await_ms) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "transaction submit timeout exceeds the configured maximum",
                            forge::exceptions::ctx("timeout_ms", value.timeout_ms),
                            forge::exceptions::ctx("limit", limits.max_await_ms));
   }
}

void require_request_within_limits(const protocol::transaction_submit_batch_request& value,
                                   const protocol::service_limits& limits) {
   if (value.transactions.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "transaction batch must not be empty");
   }
   if (value.transactions.size() > limits.max_transaction_batch_size) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "transaction batch count exceeds the configured maximum",
                            forge::exceptions::ctx("count", value.transactions.size()),
                            forge::exceptions::ctx("limit", limits.max_transaction_batch_size));
   }
   if (value.timeout_ms == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "transaction submit batch timeout must be positive");
   }
   if (value.timeout_ms > limits.max_await_ms) {
      FORGE_THROW_EXCEPTION(
          exceptions::resource_exhausted, "transaction submit batch timeout exceeds the configured maximum",
          forge::exceptions::ctx("timeout_ms", value.timeout_ms), forge::exceptions::ctx("limit", limits.max_await_ms));
   }
   require_packed_request(value, limits);
   for (const auto& transaction : value.transactions) {
      require_request_within_limits(transaction, limits);
   }
}

void require_request_within_limits(const protocol::transaction_await_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   if (value.timeout_ms == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "transaction await timeout must be positive");
   }
   if (value.timeout_ms > limits.max_await_ms) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "transaction await timeout exceeds the configured maximum",
                            forge::exceptions::ctx("timeout_ms", value.timeout_ms),
                            forge::exceptions::ctx("limit", limits.max_await_ms));
   }
}

void require_request_within_limits(const protocol::ram_corrections_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit", true);
}

void require_request_within_limits(const protocol::unapplied_transactions_request& value,
                                   const protocol::service_limits& limits) {
   require_packed_request(value, limits);
   require_page(value.limit, limits.max_page_size, "limit", true);
}

void require_response_within_limits(const protocol::block_range_response& response,
                                    const protocol::block_range_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.blocks.size(), request.limit, limits, "blocks");
}

void require_response_within_limits(const protocol::protocol_features_response& response,
                                    const protocol::protocol_features_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.features.size(), request.limit, limits, "features");
}

void require_response_within_limits(const protocol::producers_response& response,
                                    const protocol::producers_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_nonempty_next(response.next);
   require_items(response.rows.size(), request.limit, limits, "rows");
}

void require_response_within_limits(const protocol::producer_rewards_response& response,
                                    const protocol::producer_rewards_request&, const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.system.rows.size(), 1U, limits, "eosio.system rows");
   require_items(response.bpay.rows.size(), 1U, limits, "eosio.bpay rows");
}

void require_response_within_limits(const protocol::account_changes_response& response,
                                    const protocol::account_changes_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_nonempty_next(response.next);
   require_mutations(response.blocks, request.limit, limits);
}

void require_response_within_limits(const protocol::permission_links_response& response,
                                    const protocol::permission_links_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_nonempty_next(response.next);
   require_items(response.links.size(), request.limit, limits, "links");
}

void require_response_within_limits(const protocol::table_changes_response& response,
                                    const protocol::table_changes_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_nonempty_next(response.next);
   require_mutations(response.blocks, request.limit, limits);
}

void require_response_within_limits(const protocol::table_rows_response& response,
                                    const protocol::table_rows_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.rows.size(), request.limit, limits, "rows");
}

void require_response_within_limits(const protocol::table_scope_response& response,
                                    const protocol::table_scope_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.tables.size(), request.limit, limits, "tables");
}

void require_response_within_limits(const protocol::scheduled_response& response,
                                    const protocol::scheduled_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_nonempty_next(response.next);
   require_items(response.transactions.size(), request.limit, limits, "transactions");
}

void require_response_within_limits(const protocol::authorizers_response& response,
                                    const protocol::authorizers_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_nonempty_next(response.next);
   require_items(response.accounts.size(), request.limit, limits, "accounts");
}

void require_response_within_limits(const protocol::ram_corrections_response& response,
                                    const protocol::ram_corrections_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.rows.size(), request.limit, limits, "rows");
}

void require_response_within_limits(const protocol::unapplied_transactions_response& response,
                                    const protocol::unapplied_transactions_request& request,
                                    const protocol::service_limits& limits) {
   require_response_within_limits(response, limits);
   require_items(response.transactions.size(), request.limit, limits, "transactions");
}

void require_transaction_batch_response_within_limits(
    const std::vector<protocol::transaction_submit_response>& responses, std::size_t request_count,
    const protocol::service_limits& limits) {
   require_response_within_limits(responses, limits);
   if (responses.size() > limits.max_transaction_batch_size) {
      FORGE_THROW_EXCEPTION(
          exceptions::resource_exhausted, "chain API transaction response count exceeds the request limit",
          forge::exceptions::ctx("count", responses.size()), forge::exceptions::ctx("request_count", request_count),
          forge::exceptions::ctx("limit", limits.max_transaction_batch_size));
   }
   if (responses.size() != request_count) {
      FORGE_THROW_EXCEPTION(
          exceptions::unavailable, "chain API owner returned a transaction batch with mismatched cardinality",
          forge::exceptions::ctx("count", responses.size()), forge::exceptions::ctx("request_count", request_count));
   }
}

forge::api::core::descriptor with_service_limits(forge::api::core::descriptor value, protocol::service_limits limits) {
   const auto api = value.id.value;
   for (auto& method : value.methods) {
      auto name = method.name;
      const auto audited_response = method.has_response_trait<protocol::audited_response>();
      auto request_decoder = method.request_decoder;
      auto response_decoder = method.response_decoder;
      method.request_validator = [api, name, limits, request_decoder](const forge::api::core::bytes& payload) {
         decode_request(request_decoder, api, name, payload, limits);
         static_cast<void>(require_method_request(api, name, payload, limits));
      };
      method.response_validator = [api, name, audited_response, limits, request_decoder, response_decoder](
                                      const forge::api::core::bytes& request, const forge::api::core::bytes& response) {
         decode_request(request_decoder, api, name, request, limits);
         decode_response(response_decoder, api, name, response, limits);
         require_method_response(api, name, audited_response, response,
                                 require_method_request(api, name, request, limits), limits);
      };
   }
   return value;
}

} // namespace forge::chain::api
