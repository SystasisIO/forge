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

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.chain.signer.plugin;

import forge.api.core.exceptions;
import forge.chain.api.block_signer;
import forge.chain.api.exceptions;
import forge.chain.protocol.block;
import forge.chain.protocol.block_signing;
import forge.crypto.asymmetric;
import forge.crypto.signer.provider;
import forge.exceptions;
import forge.plugins.chain.signer.types;
import forge.raw.raw;

#include "details/block_api_impl.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::chain::signer {
namespace {

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

boost::asio::awaitable<std::vector<forge::chain::protocol::signature>>
sign_admitted(auto state, forge::chain::protocol::block_sign_request request,
              forge::api::auth::authenticated_caller caller, admission_lease lease) {
   try {
      co_await require_active_request(lease);
      auto selected = state->select_block(request, caller);
      if (forge::raw::pack_size(request.header) > selected.max_header_bytes) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                               "Chain signer block header exceeds the selected profile limit");
      }

      // Resolve every configured identity before any cryptographic operation can occur.
      for (const auto& key : selected.keys) {
         const auto identity = co_await key.provider->describe(key.key.id);
         co_await require_active_request(lease);
         if (identity.id != key.key.id || identity.public_key != key.key.public_key) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed,
                                  "Chain signer provider identity does not match its configured block binding");
         }
      }

      const auto digest = forge::chain::protocol::calculate_block_id(request.header);
      auto result = std::vector<forge::chain::protocol::signature>{};
      result.reserve(selected.keys.size());
      for (const auto& key : selected.keys) {
         auto response = co_await key.provider->sign_digest({.id = key.key.id, .digest = digest});
         co_await require_active_request(lease);
         auto valid = response.public_key == key.key.public_key &&
                      forge::crypto::asymmetric::type(response.signature) ==
                          forge::crypto::asymmetric::algorithm::secp256k1;
         try {
            valid = valid && forge::crypto::asymmetric::recover(response.signature, digest) == key.key.public_key;
         } catch (const forge::exceptions::base&) {
            valid = false;
         }
         if (!valid) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed,
                                  "Chain signer provider returned an invalid block signature");
         }
         result.push_back(std::move(response.signature));
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

plugin::block_api_impl::block_api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<std::vector<forge::chain::protocol::signature>>
plugin::block_api_impl::sign(forge::chain::protocol::block_sign_request request,
                             forge::api::auth::authenticated_caller caller) {
   auto audit = audit_entry{
       .operation = audit_operation::block,
       .local = !caller.transport_authenticated(),
       .caller_source = caller.source,
       .caller_fingerprint = caller.fingerprint,
       .chain = request.chain,
       .producer = request.header.producer,
       .key_count = static_cast<std::uint32_t>(request.keys.size()),
   };
   try {
      auto selected = state_->select_block(request, caller);
      audit.profile = selected.profile;
      if (forge::raw::pack_size(request.header) > selected.max_header_bytes) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                               "Chain signer block header exceeds the selected profile limit");
      }
      auto lease = co_await state_->acquire(caller_key(caller), forge::raw::pack_size(request));
      audit.block = forge::chain::protocol::calculate_block_id(request.header);
      auto cancellation = lease.bind(co_await boost::asio::this_coro::cancellation_state);
      auto executor = co_await boost::asio::this_coro::executor;
      auto result = co_await boost::asio::co_spawn(
          executor, sign_admitted(state_, std::move(request), std::move(caller), std::move(lease)),
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
   } catch (const forge::api::core::exceptions::cancelled& error) {
      audit.decision = audit_decision::failed;
      set_error(audit, error);
      state_->audit(std::move(audit));
      throw;
   } catch (const forge::chain::api::exceptions::unavailable& error) {
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
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed, "Chain signer block provider failed");
   } catch (const std::exception&) {
      audit.decision = audit_decision::failed;
      state_->audit(std::move(audit));
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed, "Chain signer block provider failed");
   }
}

} // namespace forge::plugins::chain::signer
