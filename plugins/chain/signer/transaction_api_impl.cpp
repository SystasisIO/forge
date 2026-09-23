module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.chain.signer.plugin;

import forge.api.core.exceptions;
import forge.chain.api.exceptions;
import forge.chain.api.block_signer;
import forge.chain.api.transaction_signer;
import forge.chain.protocol.block_signing;
import forge.chain.protocol.transaction;
import forge.chain.transaction.signing;
import forge.crypto.asymmetric;
import forge.crypto.signer.provider;
import forge.exceptions;
import forge.plugins.chain.signer.types;
import forge.raw.raw;

#include "details/plugin_impl.hxx"
#include "details/transaction_api_impl.hxx"

namespace forge::plugins::chain::signer {
namespace {

[[nodiscard]] audit_entry make_audit(const forge::chain::transaction::unsigned_transaction& transaction,
                                     const forge::api::auth::authenticated_caller& caller) {
   return audit_entry{
       .local = !caller.transport_authenticated(),
       .caller_source = caller.source,
       .caller_fingerprint = caller.fingerprint,
       .chain = transaction.chain,
       .transaction = transaction.value.id(),
   };
}

[[nodiscard]] std::string caller_key(const forge::api::auth::authenticated_caller& caller) {
   if (!caller.transport_authenticated()) {
      return "local";
   }
   return std::to_string(static_cast<std::uint8_t>(caller.source)) + ':' + caller.fingerprint.str();
}

void set_error(audit_entry& audit, const forge::exceptions::base& error) {
   audit.error_category = error.code().category().name();
   audit.error_code = error.code().value();
}

boost::asio::awaitable<void> require_active_request(const admission_lease& lease) {
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (lease.cancelled() || cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain signer request was canceled");
   }
}

boost::asio::awaitable<forge::chain::transaction::prepared_transaction>
sign_admitted(auto state, forge::chain::transaction::unsigned_transaction transaction,
              forge::api::auth::authenticated_caller caller, admission_lease lease) {
   try {
      co_await require_active_request(lease);
      auto selected = state->select_transaction(transaction, caller);

      if (caller.transport_authenticated()) {
         if (selected.semantic_authorization != nullptr &&
             !(co_await selected.semantic_authorization->authorize_remote(selected.profile, transaction, caller))) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                                  "Chain signer semantic authorization denied the remote transaction");
         }
         co_await require_active_request(lease);
      }

      auto projected = forge::chain::protocol::signed_transaction{};
      static_cast<forge::chain::protocol::transaction&>(projected) = transaction.value;
      projected.context_free_data = transaction.context_free_data;
      projected.signatures.emplace_back(forge::crypto::asymmetric::k1_signature{});
      const auto prepared_size = forge::raw::pack_size(
          forge::chain::transaction::pack(std::move(projected), transaction.compression).packed);
      if (prepared_size > selected.max_packed_bytes) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                               "Chain signer prepared transaction exceeds the selected profile limit");
      }

      const auto key = co_await selected.provider->describe(selected.key.id);
      co_await require_active_request(lease);
      if (key.id != selected.key.id || key.public_key != selected.key.public_key) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed,
                               "Chain signer provider identity does not match its configured binding");
      }

      auto result = co_await forge::chain::transaction::sign(
          std::move(transaction), std::vector<forge::chain::transaction::signing_key>{std::move(selected.key)},
          *selected.provider);
      co_await require_active_request(lease);
      if (forge::raw::pack_size(result.packed) > selected.max_packed_bytes) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                               "Chain signer prepared transaction exceeds the selected profile limit");
      }
      co_return result;
   } catch (const boost::system::system_error&) {
      if (lease.cancelled()) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain signer request was canceled");
      }
      throw;
   }
}

} // namespace

plugin::transaction_api_impl::transaction_api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<forge::chain::transaction::prepared_transaction>
plugin::transaction_api_impl::sign(forge::chain::transaction::unsigned_transaction transaction,
                                   forge::api::auth::authenticated_caller caller) {
   auto audit = make_audit(transaction, caller);
   try {
      auto selected = state_->select_transaction(transaction, caller);
      audit.profile = selected.profile;
      const auto admission_bytes = forge::raw::pack(transaction).size();
      auto lease = co_await state_->acquire(caller_key(caller), admission_bytes);
      auto cancellation = lease.bind(co_await boost::asio::this_coro::cancellation_state);
      auto executor = co_await boost::asio::this_coro::executor;
      auto result = co_await boost::asio::co_spawn(
          executor, sign_admitted(state_, std::move(transaction), std::move(caller), std::move(lease)),
          boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_awaitable));
      audit.decision = audit_decision::allowed;
      state_->audit(std::move(audit));
      co_return result;
   } catch (const forge::chain::api::exceptions::authorization_denied& error) {
      audit.decision = audit_decision::denied;
      set_error(audit, error);
      state_->audit(std::move(audit));
      throw;
   } catch (const forge::chain::api::exceptions::resource_exhausted& error) {
      audit.decision = audit_decision::denied;
      set_error(audit, error);
      state_->audit(std::move(audit));
      throw;
   } catch (const forge::chain::api::exceptions::unavailable& error) {
      audit.decision = audit_decision::failed;
      set_error(audit, error);
      state_->audit(std::move(audit));
      throw;
   } catch (const forge::api::core::exceptions::cancelled& error) {
      audit.decision = audit_decision::failed;
      set_error(audit, error);
      state_->audit(std::move(audit));
      throw;
   } catch (const forge::chain::api::exceptions::signing_failed& error) {
      audit.decision = audit_decision::failed;
      set_error(audit, error);
      state_->audit(std::move(audit));
      throw;
   } catch (const forge::exceptions::base& error) {
      audit.decision = audit_decision::failed;
      set_error(audit, error);
      state_->audit(std::move(audit));
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed, "Chain signer provider failed");
   } catch (const std::exception&) {
      audit.decision = audit_decision::failed;
      state_->audit(std::move(audit));
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed, "Chain signer provider failed");
   }
}

} // namespace forge::plugins::chain::signer
