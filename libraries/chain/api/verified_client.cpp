module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>

#include <coroutine>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

module forge.chain.api.verified_client;

import forge.api.core.exceptions;
import forge.chain.api.exceptions;
import forge.asio.exceptions;
import forge.crypto.digest.sha256;

namespace forge::chain::api {
namespace {

template <typename Exception, typename Function>
decltype(auto) invoke_verifier(const char* message, Function&& function) {
   try {
      return std::forward<Function>(function)();
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(Exception, message, forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(Exception, message);
   }
}

std::optional<protocol::block_num> block_num(const std::optional<protocol::block_id>& block) {
   return block ? std::optional{protocol::calculate_block_num_from_id(*block)} : std::nullopt;
}

std::optional<protocol::block_num> target_block(const protocol::block_request& request) {
   return request.num ? request.num : block_num(request.id);
}

std::optional<protocol::block_num> target_block(const protocol::block_range_request& request) {
   if (request.limit == 0U) {
      return request.first;
   }
   const auto last = static_cast<std::uint64_t>(request.first) + request.limit - 1U;
   return last <= std::numeric_limits<protocol::block_num>::max()
              ? std::optional{static_cast<protocol::block_num>(last)}
              : std::nullopt;
}

protocol::block_num transaction_hint_target(const protocol::transaction_id& expected,
                                            const protocol::transaction_status_response& response) {
   if (response.id != expected) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API transaction hint has the wrong transaction id");
   }
   if (response.state != protocol::transaction_lifecycle::included &&
       response.state != protocol::transaction_lifecycle::finalized) {
      FORGE_THROW_EXCEPTION(exceptions::audit_not_supported,
                            "chain API cannot prove a transaction before canonical inclusion");
   }
   if (!response.block_num) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API transaction hint omits its inclusion block number");
   }
   if (response.block && protocol::calculate_block_num_from_id(*response.block) != *response.block_num) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API transaction hint contains an inconsistent inclusion block");
   }
   return *response.block_num;
}

protocol::block_num change_witness_target(protocol::block_num from_block, protocol::block_num to_block) {
   return from_block < to_block ? from_block + 1U : to_block;
}

template <typename Request>
void require_audit(Request& request, audit_verifier& verifier,
                   std::optional<protocol::block_num> target = std::nullopt) {
   request.audit = protocol::audit_mode::required;
   if (!request.finality_from) {
      const auto selected =
          invoke_verifier<exceptions::anchor_unavailable>("chain API trust anchor provider failed", [&] {
             return target ? verifier.finality_anchor_at_or_before(*target) : verifier.preferred_finality_anchor();
          });
      if (target && !selected) {
         FORGE_THROW_EXCEPTION(exceptions::anchor_unavailable,
                               "chain API verifier has no trusted finality anchor for the requested target");
      }
      request.finality_from = selected;
   }
}

template <typename Response, typename Operation>
boost::asio::awaitable<Response> invoke_service(const char* method, const protocol::service_limits& limits,
                                                Operation&& operation) {
   try {
      auto response = co_await std::forward<Operation>(operation)();
      require_response_within_limits(response, limits);
      co_return response;
   } catch (const forge::asio::exceptions::canceled&) {
      throw;
   } catch (const forge::api::core::exceptions::cancelled&) {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "chain API request was canceled",
                            forge::exceptions::ctx("method", method));
   } catch (const forge::api::core::exceptions::deadline_exceeded&) {
      FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "chain API request deadline expired",
                            forge::exceptions::ctx("method", method));
   } catch (const forge::exceptions::base& error) {
      if (std::string_view{error.code().category().name()} == "forge.chain.api") {
         throw;
      }
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API service failed",
                            forge::exceptions::ctx("method", method), forge::exceptions::ctx("reason", error.what()));
   } catch (const boost::system::system_error& error) {
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "chain API request was canceled",
                               forge::exceptions::ctx("method", method));
      }
      if (error.code() == boost::asio::error::timed_out) {
         FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "chain API request deadline expired",
                               forge::exceptions::ctx("method", method));
      }
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API service failed",
                            forge::exceptions::ctx("method", method), forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API service failed",
                            forge::exceptions::ctx("method", method), forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "chain API service failed",
                            forge::exceptions::ctx("method", method));
   }
}

template <typename Response, typename Request, typename Operation>
boost::asio::awaitable<Response> invoke_service(const char* method, const Request& request,
                                                const protocol::service_limits& limits, Operation&& operation) {
   require_request_within_limits(request, limits);
   auto response = co_await invoke_service<Response>(method, limits, std::forward<Operation>(operation));
   require_response_within_limits(response, request, limits);
   co_return response;
}

template <typename Function> void verify_projection(const char* method, Function&& function) {
   try {
      std::forward<Function>(function)();
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, "chain API projection verifier failed",
                            forge::exceptions::ctx("method", method), forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, "chain API projection verifier failed",
                            forge::exceptions::ctx("method", method));
   }
}

template <typename Response> boost::asio::awaitable<Response> unsupported_audit(const char* method) {
   FORGE_THROW_EXCEPTION(exceptions::audit_not_supported, "verified chain API method has no content proof verifier",
                         forge::exceptions::ctx("method", method));
   co_return Response{};
}

[[noreturn]] void unsupported_projection(const char* method) {
   FORGE_THROW_EXCEPTION(exceptions::audit_not_supported,
                         "verified chain API method has no deterministic projection verifier",
                         forge::exceptions::ctx("method", method));
}

projection_verifier& require_projection(const std::shared_ptr<projection_verifier>& value, const char* method) {
   if (!value) {
      unsupported_projection(method);
   }
   return *value;
}

} // namespace

std::optional<protocol::block_id> audit_verifier::preferred_finality_anchor() const {
   return std::nullopt;
}

std::optional<protocol::block_id> audit_verifier::finality_anchor_at_or_before(protocol::block_num target) const {
   const auto preferred = preferred_finality_anchor();
   if (preferred && protocol::calculate_block_num_from_id(*preferred) > target) {
      return std::nullopt;
   }
   return preferred;
}

const protocol::bytes& require_content_witness(const protocol::audit_bundle& audit, protocol::digest expected,
                                               std::optional<std::uint64_t> expected_size) {
   const protocol::content_witness* match = nullptr;
   for (const auto& witness : audit.content) {
      if (witness.hash != expected) {
         continue;
      }
      if (match) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, "chain API audit contains duplicate content witnesses");
      }
      match = &witness;
   }

   if (!match) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, "chain API audit omits the required content witness");
   }
   if (expected_size && *expected_size != match->value.size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, "chain API content witness has an unexpected size");
   }
   if (forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{match->value}) != expected) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, "chain API content witness does not match its digest");
   }
   return match->value;
}

projection_verifier::~projection_verifier() = default;

void projection_verifier::verify(const protocol::block_request&, const protocol::block_state_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_block_state");
}

void projection_verifier::verify(const protocol::block_range_request&, const protocol::block_range_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_canonical_range");
}

void projection_verifier::verify(const protocol::protocol_features_request&,
                                 const protocol::protocol_features_response&, const protocol::audit_bundle&,
                                 audit_verifier&) {
   unsupported_projection("block.get_activated_protocol_features");
}

void projection_verifier::verify(const protocol::anchored_request&, const protocol::consensus_parameters_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_consensus_parameters");
}

void projection_verifier::verify(const protocol::producers_request&, const protocol::producers_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_producers");
}

void projection_verifier::verify(const protocol::producer_rewards_request&, const protocol::producer_rewards_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_producer_rewards");
}

void projection_verifier::verify(const protocol::anchored_request&, const protocol::producer_schedule_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_producer_schedule");
}

void projection_verifier::verify(const protocol::anchored_request&, const protocol::finalizer_info_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("block.get_finalizer_info");
}

void projection_verifier::verify(const protocol::account_request&, const protocol::account_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_account");
}

void projection_verifier::verify(const protocol::account_changes_request&, const protocol::account_changes_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_account_changes");
}

void projection_verifier::verify(const protocol::code_request&, const protocol::code_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_code");
}

void projection_verifier::verify(const protocol::permission_links_request&, const protocol::permission_links_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_permission_links");
}

void projection_verifier::verify(const protocol::table_rows_request&, const protocol::table_rows_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_table_rows");
}

void projection_verifier::verify(const protocol::table_changes_request&, const protocol::table_changes_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_table_changes");
}

void projection_verifier::verify(const protocol::table_scope_request&, const protocol::table_scope_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_table_scope");
}

void projection_verifier::verify(const protocol::currency_balance_request&, const protocol::currency_balance_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_currency_balance");
}

void projection_verifier::verify(const protocol::currency_stats_request&, const protocol::currency_stats_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_currency_stats");
}

void projection_verifier::verify(const protocol::scheduled_request&, const protocol::scheduled_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_scheduled_transactions");
}

void projection_verifier::verify(const protocol::authorizers_request&, const protocol::authorizers_response&,
                                 const protocol::audit_bundle&, audit_verifier&) {
   unsupported_projection("state.get_accounts_by_authorizers");
}

verified_client::verified_client(raw_client client, std::shared_ptr<audit_verifier> verifier,
                                 std::shared_ptr<projection_verifier> projections, protocol::service_limits limits)
    : client_{std::move(client)}, verifier_{std::move(verifier)}, projections_{std::move(projections)},
      limits_{limits} {
   if (!verifier_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "verified chain API client requires a trust verifier");
   }
}

const protocol::audit_bundle& verified_client::verify_envelope(const protocol::audited_response& response) {
   invoke_verifier<exceptions::invalid_state_proof>("chain API context verifier failed",
                                                    [&] { verifier_->verify_context(response.context); });
   if (!response.context.anchor || !response.audit) {
      FORGE_THROW_EXCEPTION(exceptions::audit_not_supported, "chain API response omits required audit data");
   }
   if (!response.audit->finality) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality, "chain API response omits its finality proof");
   }
   invoke_verifier<exceptions::invalid_finality>("chain API finality verifier failed", [&] {
      verifier_->verify_finality(*response.context.anchor, *response.audit->finality);
   });
   return *response.audit;
}

void verified_client::verify_requested_anchor(const std::optional<protocol::block_id>& requested,
                                              const protocol::audited_response& response) {
   if (requested && response.context.anchor->block != *requested) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "chain API response does not match the requested anchor block");
   }
}

void verified_client::verify_change_batches(std::uint32_t from_block, std::uint32_t to_block, bool has_request_cursor,
                                            const protocol::audited_response& response,
                                            std::span<const protocol::state_anchor> anchors, bool has_next,
                                            const protocol::audit_bundle& audit) {
   const auto reject = [](const char* message) { FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, message); };
   if (from_block > to_block || response.context.anchor->block_num != to_block) {
      reject("chain API change feed does not match its finalized interval");
   }
   if (from_block == to_block) {
      if (has_request_cursor || !anchors.empty() || has_next || !audit.state.empty() || audit.ancestry) {
         reject("chain API empty change interval contains data, proofs, or a cursor");
      }
      return;
   }
   if (anchors.empty() || audit.state.empty()) {
      reject("chain API change page omits its per-block batches or authenticated state proofs");
   }
   if (!has_request_cursor && anchors.front().block_num != from_block + 1U) {
      reject("chain API first change page does not start after from_block");
   }

   auto ancestry = std::vector<protocol::state_anchor>{};
   ancestry.reserve(anchors.size());
   auto expected_block = anchors.front().block_num;
   for (const auto& anchor : anchors) {
      if (anchor.chain != response.context.chain || anchor.block_num <= from_block || anchor.block_num > to_block ||
          anchor.block_num != expected_block) {
         reject("chain API change response contains a non-canonical block sequence");
      }
      if (anchor.block_num == to_block) {
         if (anchor != *response.context.anchor) {
            reject("chain API target batch does not match the finalized response anchor");
         }
      } else {
         ancestry.push_back(anchor);
      }
      ++expected_block;
   }

   if (!has_next && anchors.back() != *response.context.anchor) {
      reject("chain API final change page does not reach its finalized target anchor");
   }
   if (ancestry.empty()) {
      if (audit.ancestry) {
         reject("chain API change response contains an unrelated ancestry proof");
      }
   } else {
      if (!audit.ancestry) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                               "chain API change response omits its canonical ancestry proof");
      }
      invoke_verifier<exceptions::invalid_finality>("chain API ancestry verifier failed", [&] {
         verifier_->verify_ancestry(*response.context.anchor, ancestry, *audit.ancestry);
      });
   }
}

void verified_client::verify_transaction_status(const forge::chain::protocol::transaction_id& expected,
                                                const protocol::transaction_status_response& response) {
   const auto& audit = verify_envelope(response);
   if (response.head != response.context.head || response.finalized != response.context.finalized ||
       response.head_num != protocol::calculate_block_num_from_id(response.head) ||
       response.finalized_num != protocol::calculate_block_num_from_id(response.finalized)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "chain API transaction payload is inconsistent with its audited context");
   }
   if (response.id != expected) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API transaction response has the wrong transaction id");
   }
   if (response.trace) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API audited transaction response contains an unauthenticated execution trace");
   }
   if (response.state != protocol::transaction_lifecycle::included &&
       response.state != protocol::transaction_lifecycle::finalized) {
      FORGE_THROW_EXCEPTION(exceptions::audit_not_supported,
                            "chain API cannot prove a transaction before canonical inclusion");
   }
   if (!audit.transaction) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API response omits its transaction inclusion proof");
   }
   invoke_verifier<exceptions::invalid_transaction_proof>("chain API transaction verifier failed", [&] {
      verifier_->verify_transaction(*response.context.anchor, expected, response, *audit.transaction);
   });
}

boost::asio::awaitable<protocol::info_response> verified_client::get_info() {
   return get_info(protocol::anchored_request{});
}

boost::asio::awaitable<protocol::info_response> verified_client::get_info(protocol::anchored_request request) {
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::info_response>("info.get", request, limits_,
                                                                    [&] { return client_.info().get(request); });
   static_cast<void>(verify_envelope(response));
   verify_requested_anchor(requested_anchor, response);
   if (response.chain != response.context.chain) {
      FORGE_THROW_EXCEPTION(exceptions::wrong_chain, "chain API info payload belongs to another chain");
   }
   if (response.head != response.context.head || response.finalized != response.context.finalized ||
       response.head_num != protocol::calculate_block_num_from_id(response.head) ||
       response.finalized_num != protocol::calculate_block_num_from_id(response.finalized) ||
       response.context.anchor->block != response.finalized ||
       response.context.anchor->block_num != response.finalized_num) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "chain API info payload is inconsistent with its audited context");
   }
   co_return response;
}

boost::asio::awaitable<protocol::block_response> verified_client::get_block(protocol::block_request request) {
   require_audit(request, *verifier_, target_block(request));
   const auto requested_id = request.id;
   const auto requested_num = request.num;
   auto response = co_await invoke_service<protocol::block_response>(
       "block.get_block", request, limits_, [&] { return client_.blocks().get_block(request); });
   static_cast<void>(verify_envelope(response));
   if ((requested_id && response.id != *requested_id) || (requested_num && response.num != *requested_num) ||
       response.id != response.context.anchor->block || response.num != response.context.anchor->block_num ||
       response.block.calculate_id() != response.id || response.block.calculate_block_num() != response.num ||
       protocol::calculate_transaction_mroot(response.block.transactions) != response.block.transaction_mroot ||
       response.block.transaction_mroot != response.context.anchor->transaction_root || !response.canonical) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "chain API block response does not match its request and audited anchor");
   }
   co_return response;
}

boost::asio::awaitable<protocol::block_header_response> verified_client::get_header(protocol::block_request request) {
   require_audit(request, *verifier_, target_block(request));
   const auto requested_id = request.id;
   const auto requested_num = request.num;
   auto response = co_await invoke_service<protocol::block_header_response>(
       "block.get_header", request, limits_, [&] { return client_.blocks().get_header(request); });
   static_cast<void>(verify_envelope(response));
   if ((requested_id && response.id != *requested_id) || (requested_num && response.num != *requested_num) ||
       response.id != response.context.anchor->block || response.num != response.context.anchor->block_num ||
       response.header.calculate_id() != response.id || response.header.calculate_block_num() != response.num ||
       response.header.transaction_mroot != response.context.anchor->transaction_root || !response.canonical) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "chain API block header does not match its request and audited anchor");
   }
   co_return response;
}

boost::asio::awaitable<protocol::block_state_response>
verified_client::get_block_state(protocol::block_request request) {
   auto& projections = require_projection(projections_, "block.get_block_state");
   require_audit(request, *verifier_, target_block(request));
   auto response = co_await invoke_service<protocol::block_state_response>(
       "block.get_block_state", request, limits_, [&] { return client_.blocks().get_block_state(request); });
   const auto& audit = verify_envelope(response);
   verify_projection("block.get_block_state", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::block_range_response>
verified_client::get_canonical_range(protocol::block_range_request request) {
   auto& projections = require_projection(projections_, "block.get_canonical_range");
   require_audit(request, *verifier_, target_block(request));
   auto response = co_await invoke_service<protocol::block_range_response>(
       "block.get_canonical_range", request, limits_, [&] { return client_.blocks().get_canonical_range(request); });
   const auto& audit = verify_envelope(response);
   verify_projection("block.get_canonical_range", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::protocol_features_response>
verified_client::get_activated_protocol_features(protocol::protocol_features_request request) {
   auto& projections = require_projection(projections_, "block.get_activated_protocol_features");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::protocol_features_response>(
       "block.get_activated_protocol_features", request, limits_,
       [&] { return client_.blocks().get_activated_protocol_features(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("block.get_activated_protocol_features",
                     [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::consensus_parameters_response>
verified_client::get_consensus_parameters(protocol::anchored_request request) {
   auto& projections = require_projection(projections_, "block.get_consensus_parameters");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::consensus_parameters_response>(
       "block.get_consensus_parameters", request, limits_,
       [&] { return client_.blocks().get_consensus_parameters(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("block.get_consensus_parameters",
                     [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::producers_response>
verified_client::get_producers(protocol::producers_request request) {
   auto& projections = require_projection(projections_, "block.get_producers");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::producers_response>(
       "block.get_producers", request, limits_, [&] { return client_.blocks().get_producers(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("block.get_producers", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::producer_rewards_response>
verified_client::get_producer_rewards(protocol::producer_rewards_request request) {
   auto& projections = require_projection(projections_, "block.get_producer_rewards");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::producer_rewards_response>(
       "block.get_producer_rewards", request, limits_, [&] { return client_.blocks().get_producer_rewards(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("block.get_producer_rewards", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::producer_schedule_response>
verified_client::get_producer_schedule(protocol::anchored_request request) {
   auto& projections = require_projection(projections_, "block.get_producer_schedule");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::producer_schedule_response>(
       "block.get_producer_schedule", request, limits_,
       [&] { return client_.blocks().get_producer_schedule(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("block.get_producer_schedule", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::finalizer_info_response>
verified_client::get_finalizer_info(protocol::anchored_request request) {
   auto& projections = require_projection(projections_, "block.get_finalizer_info");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::finalizer_info_response>(
       "block.get_finalizer_info", request, limits_, [&] { return client_.blocks().get_finalizer_info(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("block.get_finalizer_info", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::account_response> verified_client::get_account(protocol::account_request request) {
   auto& projections = require_projection(projections_, "state.get_account");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::account_response>(
       "state.get_account", request, limits_, [&] { return client_.state().get_account(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_account", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::account_changes_response>
verified_client::get_account_changes(protocol::account_changes_request request) {
   auto& projections = require_projection(projections_, "state.get_account_changes");
   require_audit(request, *verifier_, change_witness_target(request.from_block, request.to_block));
   auto response = co_await invoke_service<protocol::account_changes_response>(
       "state.get_account_changes", request, limits_, [&] { return client_.state().get_account_changes(request); });
   const auto& audit = verify_envelope(response);
   auto anchors = std::vector<protocol::state_anchor>{};
   anchors.reserve(response.blocks.size());
   std::ranges::transform(response.blocks, std::back_inserter(anchors), &protocol::account_change_batch::anchor);
   verify_change_batches(request.from_block, request.to_block, request.cursor.has_value(), response, anchors,
                         response.next.has_value(), audit);
   verify_projection("state.get_account_changes", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::code_response> verified_client::get_code(protocol::code_request request) {
   auto& projections = require_projection(projections_, "state.get_code");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::code_response>("state.get_code", request, limits_,
                                                                    [&] { return client_.state().get_code(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_code", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::permission_links_response>
verified_client::get_permission_links(protocol::permission_links_request request) {
   auto& projections = require_projection(projections_, "state.get_permission_links");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::permission_links_response>(
       "state.get_permission_links", request, limits_, [&] { return client_.state().get_permission_links(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_permission_links", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::table_rows_response>
verified_client::get_table_rows(protocol::table_rows_request request) {
   auto& projections = require_projection(projections_, "state.get_table_rows");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::table_rows_response>(
       "state.get_table_rows", request, limits_, [&] { return client_.state().get_table_rows(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_table_rows", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::table_changes_response>
verified_client::get_table_changes(protocol::table_changes_request request) {
   auto& projections = require_projection(projections_, "state.get_table_changes");
   require_audit(request, *verifier_, change_witness_target(request.from_block, request.to_block));
   auto response = co_await invoke_service<protocol::table_changes_response>(
       "state.get_table_changes", request, limits_, [&] { return client_.state().get_table_changes(request); });
   const auto& audit = verify_envelope(response);
   auto anchors = std::vector<protocol::state_anchor>{};
   anchors.reserve(response.blocks.size());
   std::ranges::transform(response.blocks, std::back_inserter(anchors), &protocol::table_change_batch::anchor);
   verify_change_batches(request.from_block, request.to_block, request.cursor.has_value(), response, anchors,
                         response.next.has_value(), audit);
   verify_projection("state.get_table_changes", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::table_scope_response>
verified_client::get_table_scope(protocol::table_scope_request request) {
   auto& projections = require_projection(projections_, "state.get_table_scope");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::table_scope_response>(
       "state.get_table_scope", request, limits_, [&] { return client_.state().get_table_scope(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_table_scope", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::currency_balance_response>
verified_client::get_currency_balance(protocol::currency_balance_request request) {
   auto& projections = require_projection(projections_, "state.get_currency_balance");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::currency_balance_response>(
       "state.get_currency_balance", request, limits_, [&] { return client_.state().get_currency_balance(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_currency_balance", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::currency_stats_response>
verified_client::get_currency_stats(protocol::currency_stats_request request) {
   auto& projections = require_projection(projections_, "state.get_currency_stats");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::currency_stats_response>(
       "state.get_currency_stats", request, limits_, [&] { return client_.state().get_currency_stats(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_currency_stats", [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::scheduled_response>
verified_client::get_scheduled_transactions(protocol::scheduled_request request) {
   auto& projections = require_projection(projections_, "state.get_scheduled_transactions");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response =
       co_await invoke_service<protocol::scheduled_response>("state.get_scheduled_transactions", request, limits_, [&] {
          return client_.state().get_scheduled_transactions(request);
       });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_scheduled_transactions",
                     [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::authorizers_response>
verified_client::get_accounts_by_authorizers(protocol::authorizers_request request) {
   auto& projections = require_projection(projections_, "state.get_accounts_by_authorizers");
   require_audit(request, *verifier_, block_num(request.anchor));
   const auto requested_anchor = request.anchor;
   auto response = co_await invoke_service<protocol::authorizers_response>(
       "state.get_accounts_by_authorizers", request, limits_,
       [&] { return client_.state().get_accounts_by_authorizers(request); });
   const auto& audit = verify_envelope(response);
   verify_requested_anchor(requested_anchor, response);
   verify_projection("state.get_accounts_by_authorizers",
                     [&] { projections.verify(request, response, audit, *verifier_); });
   co_return response;
}

boost::asio::awaitable<protocol::transaction_status_response>
verified_client::get_transaction_status(protocol::transaction_status_request request) {
   const auto expected = request.id;
   auto response = protocol::transaction_status_response{};
   if (request.finality_from) {
      require_audit(request, *verifier_);
      response = co_await invoke_service<protocol::transaction_status_response>(
          "transaction.get_status", request, limits_, [&] { return client_.transactions().get_status(request); });
   } else {
      request.audit = protocol::audit_mode::none;
      const auto hint = co_await invoke_service<protocol::transaction_status_response>(
          "transaction.get_status", request, limits_, [&] { return client_.transactions().get_status(request); });
      auto confirmation = protocol::transaction_status_request{.id = expected};
      require_audit(confirmation, *verifier_, transaction_hint_target(expected, hint));
      response = co_await invoke_service<protocol::transaction_status_response>(
          "transaction.get_status", confirmation, limits_,
          [&] { return client_.transactions().get_status(confirmation); });
   }
   verify_transaction_status(expected, response);
   co_return response;
}

boost::asio::awaitable<protocol::transaction_status_response>
verified_client::await_transaction(protocol::transaction_await_request request) {
   const auto expected = request.id;
   const auto desired = request.desired;
   auto response = protocol::transaction_status_response{};
   if (request.finality_from) {
      require_audit(request, *verifier_);
      response = co_await invoke_service<protocol::transaction_status_response>(
          "transaction.await_transaction", request, limits_,
          [&] { return client_.transactions().await_transaction(request); });
   } else {
      request.audit = protocol::audit_mode::none;
      const auto hint = co_await invoke_service<protocol::transaction_status_response>(
          "transaction.await_transaction", request, limits_,
          [&] { return client_.transactions().await_transaction(request); });
      auto confirmation = protocol::transaction_status_request{.id = expected};
      require_audit(confirmation, *verifier_, transaction_hint_target(expected, hint));
      response = co_await invoke_service<protocol::transaction_status_response>(
          "transaction.get_status", confirmation, limits_,
          [&] { return client_.transactions().get_status(confirmation); });
   }
   verify_transaction_status(expected, response);
   if ((desired == protocol::transaction_lifecycle::finalized &&
        response.state != protocol::transaction_lifecycle::finalized) ||
       (desired != protocol::transaction_lifecycle::included &&
        desired != protocol::transaction_lifecycle::finalized)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transaction_proof,
                            "chain API transaction response does not satisfy the requested verified lifecycle");
   }
   co_return response;
}

boost::asio::awaitable<std::vector<protocol::public_key>>
verified_client::get_required_keys(protocol::transaction_required_keys_request) {
   return unsupported_audit<std::vector<protocol::public_key>>("transaction.get_required_keys");
}

boost::asio::awaitable<protocol::transaction_read_only_response>
verified_client::compute_transaction(protocol::transaction_read_only_request) {
   return unsupported_audit<protocol::transaction_read_only_response>("transaction.compute_transaction");
}

boost::asio::awaitable<protocol::transaction_read_only_response>
verified_client::send_read_only_transaction(protocol::transaction_read_only_request) {
   return unsupported_audit<protocol::transaction_read_only_response>("transaction.send_read_only_transaction");
}

} // namespace forge::chain::api
