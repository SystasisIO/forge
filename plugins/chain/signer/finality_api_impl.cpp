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
import forge.chain.api.finality_signer;
import forge.chain.protocol.block_signing;
import forge.chain.savanna.vote;
import forge.crypto.bls;
import forge.plugins.chain.signer.types;

#include "details/finality_api_impl.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::chain::signer {
namespace {

void validate_identity(const forge::crypto::bls::signer::description& identity,
                       const forge::crypto::bls::public_key& expected) {
   if (identity.key != expected ||
       !forge::crypto::bls::verify_proof_of_possession(identity.key, identity.proof_of_possession)) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed,
                            "Chain signer finality identity does not match its configured binding");
   }
}

boost::asio::awaitable<void> require_active_request(const admission_lease& lease) {
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (lease.cancelled() || cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain signer request was canceled");
   }
}

boost::asio::awaitable<forge::chain::api::finalizer_identity> identity_admitted(auto state, admission_lease lease) {
   try {
      co_await require_active_request(lease);
      auto selected = state->select_finality();
      auto identity = co_await selected.provider->describe();
      co_await require_active_request(lease);
      validate_identity(identity, selected.expected_key);
      co_return forge::chain::api::finalizer_identity{
          .public_key = std::move(identity.key),
          .proof_of_possession = std::move(identity.proof_of_possession),
      };
   } catch (const boost::system::system_error&) {
      if (lease.cancelled()) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain signer request was canceled");
      }
      throw;
   }
}

boost::asio::awaitable<forge::chain::savanna::finalizer_vote> sign_vote_admitted(auto state,
                                                                                 forge::chain::savanna::block_ref block,
                                                                                 forge::chain::savanna::vote_kind kind,
                                                                                 admission_lease lease) {
   try {
      co_await require_active_request(lease);
      auto selected = state->select_finality();
      auto identity = co_await selected.provider->describe();
      co_await require_active_request(lease);
      validate_identity(identity, selected.expected_key);

      auto message = forge::chain::savanna::message_for_vote(block.finality_digest, kind);
      auto signed_value = co_await selected.provider->sign(message);
      co_await require_active_request(lease);
      if (signed_value.key != selected.expected_key ||
          !forge::crypto::bls::verify(signed_value.key, message, signed_value.value)) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed,
                               "Chain signer finality provider returned an invalid signature");
      }

      co_return forge::chain::savanna::finalizer_vote{
          .block = block.id,
          .finalizer = std::move(signed_value.key),
          .kind = kind,
          .signature = std::move(signed_value.value),
      };
   } catch (const boost::system::system_error&) {
      if (lease.cancelled()) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain signer request was canceled");
      }
      throw;
   }
}

} // namespace

plugin::finality_api_impl::finality_api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<forge::chain::api::finalizer_identity> plugin::finality_api_impl::identity() {
   try {
      static_cast<void>(state_->select_finality());
      auto lease = co_await state_->acquire("finality-local", 0U);
      auto cancellation = lease.bind(co_await boost::asio::this_coro::cancellation_state);
      auto executor = co_await boost::asio::this_coro::executor;
      co_return co_await boost::asio::co_spawn(
          executor, identity_admitted(state_, std::move(lease)),
          boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_awaitable));
   } catch (const forge::chain::api::exceptions::unavailable&) {
      throw;
   } catch (const forge::chain::api::exceptions::resource_exhausted&) {
      throw;
   } catch (const forge::api::core::exceptions::cancelled&) {
      throw;
   } catch (const forge::chain::api::exceptions::signing_failed&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed, "Chain signer finality provider failed");
   }
}

boost::asio::awaitable<forge::chain::savanna::finalizer_vote>
plugin::finality_api_impl::sign_vote(forge::chain::savanna::block_ref block, forge::chain::savanna::vote_kind kind) {
   if (block.empty()) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request,
                            "Chain signer cannot sign an empty finality block reference");
   }

   try {
      static_cast<void>(state_->select_finality());
      auto lease = co_await state_->acquire("finality-local", 0U);
      auto cancellation = lease.bind(co_await boost::asio::this_coro::cancellation_state);
      auto executor = co_await boost::asio::this_coro::executor;
      co_return co_await boost::asio::co_spawn(
          executor, sign_vote_admitted(state_, std::move(block), kind, std::move(lease)),
          boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_awaitable));
   } catch (const forge::chain::api::exceptions::invalid_request&) {
      throw;
   } catch (const forge::chain::api::exceptions::unavailable&) {
      throw;
   } catch (const forge::chain::api::exceptions::resource_exhausted&) {
      throw;
   } catch (const forge::api::core::exceptions::cancelled&) {
      throw;
   } catch (const forge::chain::api::exceptions::signing_failed&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::signing_failed, "Chain signer finality provider failed");
   }
}

} // namespace forge::plugins::chain::signer
