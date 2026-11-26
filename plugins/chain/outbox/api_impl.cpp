module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <functional>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

module forge.plugins.chain.outbox.plugin;

import forge.api.chain.outbox;
import forge.api.chain.runtime;
import forge.plugins.chain.outbox.setup;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.asio.cancellation_scope;
import forge.asio.notification;
import forge.asio.task;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.block;
import forge.chain.protocol.info;
import forge.chain.transaction.builder;
import forge.db.object.transaction;
import forge.exceptions;

import forge.net.http.base_url;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::chain::outbox {

plugin::api_impl::api_impl(std::shared_ptr<impl> state) : _state{std::move(state)} {}

void plugin::api_impl::configure(std::string store, forge::chain::api::verified_client_options verification,
                                  decltype(forge::api::chain::outbox::options::prepare_first_send) prepare_first_send,
                                  forge::api::chain::outbox::admission admit) {
   if (!_state->options.deferred_configuration || _state->initialized || _state->options.verification.finality ||
       !verification.finality || !verification.projections || store != _state->options.store) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Invalid deferred chain configuration");
   }
   _state->options.verification = std::move(verification);
   _state->options.delivery.prepare_first_send = std::move(prepare_first_send);
   _state->admit = std::move(admit);
}

void plugin::api_impl::configure_read(forge::api::core::api_ref contract, forge::api::chain::destination target,
                                       forge::chain::api::verified_client_options verification,
                                       std::size_t max_calls, std::chrono::milliseconds timeout) {
   if (!_state->options.deferred_configuration || _state->initialized || contract.id.value.empty() ||
       verification.chain == forge::chain::protocol::chain_id{} || verification.state_domain.empty() ||
       !verification.finality || !verification.projections || !max_calls ||
       timeout <= std::chrono::milliseconds::zero() ||
       _state->profile_for(contract)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Invalid chain read profile");
   }
   _state->read_profiles.push_back({std::move(contract), std::move(target), std::move(verification), max_calls, timeout});
}

forge::api::chain::destination plugin::api_impl::read_target(const forge::api::core::api_ref& contract) const {
   _state->require_running();
   const auto* profile = _state->profile_for(contract);
   if (!profile) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Chain read target is not configured");
   }
   return profile->target;
}

bool plugin::api_impl::same_read_profile(const std::optional<forge::api::core::api_ref>& requested,
                                         const forge::api::core::api_ref& contract) const {
   _state->require_running();
   return (requested ? _state->profile_for(*requested) : nullptr) == _state->profile_for(contract);
}

const forge::chain::api::verified_client_options& plugin::api_impl::verification_for(
    const forge::api::chain::destination& target, const impl::read_profile* profile) const {
   if (profile) {
      if (target != profile->target) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Chain read target differs from its profile");
      }
      return profile->verification;
   }
   if (!_state->options.verification.finality) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Default chain profile is not configured");
   }
   return _state->options.verification;
}

boost::asio::awaitable<forge::chain::protocol::block_id>
plugin::api_impl::finalized(forge::api::chain::destination target) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
   const auto& verification = verification_for(target, nullptr);
   const auto parent = co_await boost::asio::this_coro::cancellation_state;
   auto cancellation = forge::asio::cancellation_scope{
       co_await boost::asio::this_coro::executor, std::array{state->stop.get_token()}, parent};
   const auto info = co_await cancellation.run<forge::chain::protocol::info_response>(
       [&](std::stop_token) { return state->info(std::move(target), verification); },
       state->options.delivery.timeout);
   state->require_running();
   co_return info.finalized;
}

boost::asio::awaitable<forge::chain::transaction::builder>
plugin::api_impl::begin(forge::api::chain::destination target, std::optional<forge::api::chain::action_options> options) {
   using namespace forge::chain;
   if (!options) {
      const auto block = co_await finalized(std::move(target));
      const auto now = protocol::time_point_sec{std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
      co_return transaction::builder{{.chain = _state->options.verification.chain, .reference_block = block, .reference_time = now}};
   }
   auto context = transaction::context{};
   co_await read(std::move(target), options->reference_block,
                 [&](const forge::api::chain::read_snapshot& snapshot) -> boost::asio::awaitable<void> {
                    context = {.chain = snapshot.anchor().chain, .reference_block = snapshot.anchor().block,
                               .reference_time = snapshot.time()};
                    co_return;
                 });
   if (options->expiration.chrono() <= std::chrono::system_clock::now()) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::deadline_exceeded, "Chain action expired before signing");
   }
   co_return transaction::builder{context, {.expiration = options->expiration}};
}

boost::asio::awaitable<forge::chain::protocol::transaction_id>
plugin::api_impl::submit(forge::api::chain::destination target, forge::api::core::api_ref contract, std::string method,
                         forge::chain::transaction::unsigned_transaction transaction) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
   if (!state->queue) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Chain write profile is not configured");
   }
   const auto chain = transaction.chain;
   if (chain != state->options.verification.chain) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Unexpected chain for signing");
   }
   using outbox = forge::api::chain::outbox;
   const auto policy = state->options.policy ? state->options.policy(contract, method) : outbox::delivery_policy{};
   const auto expected = transaction.value.id();
   const auto parent = co_await boost::asio::this_coro::cancellation_state;
   auto cancellation = forge::asio::cancellation_scope{
       co_await boost::asio::this_coro::executor, std::array{state->stop.get_token()}, parent};
   auto prepared = co_await cancellation.run<forge::chain::transaction::prepared_transaction>(
       [state, transaction = std::move(transaction)](std::stop_token) mutable {
          return state->sign(std::move(transaction));
       }, state->options.delivery.timeout);
   state->require_running();
   if (prepared.packed.id() != expected) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Signer returned another transaction");
   }
   auto admit = state->admit;
   if (state->options.admit) {
      admit = [state, contract = std::move(contract), method = std::move(method)](
                  forge::db::object::transaction& write, forge::chain::protocol::transaction_id id,
                  const forge::chain::protocol::packed_transaction& packed) {
         return state->options.admit(contract, method, write, id, packed);
      };
   }
   co_return co_await state->queue->submit(chain, std::move(target), std::move(prepared.packed), policy,
                                         std::move(admit), state->stop.get_token());
}

boost::asio::awaitable<void> plugin::api_impl::read(forge::api::chain::destination target,
                                                    std::optional<forge::api::chain::block_reference> block,
                                                    read_handler handler,
                                                    std::optional<forge::api::core::api_ref> read_contract) {
   const auto state = _state;
   const auto* profile = state->begin_call(read_contract);
   auto finished = boost::scope::scope_exit{[&] { state->end_call(profile); }};
   const auto& verification = verification_for(target, profile);
   const auto parent = co_await boost::asio::this_coro::cancellation_state;
   auto cancellation = forge::asio::cancellation_scope{
       co_await boost::asio::this_coro::executor, std::array{state->stop.get_token()}, parent};
   co_await cancellation.run<void>(
       [&](std::stop_token) -> boost::asio::awaitable<void> {
          if (!block) {
             const auto floor = verification.finality->preferred_trust_anchor();
             const auto info = co_await state->info(target, verification);
             if (floor && info.finalized != *floor &&
                 info.finalized_num <= forge::chain::protocol::calculate_block_num_from_id(*floor)) {
                FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_finality,
                                      "Chain read target returned a historical finalized block");
             }
             block = info.finalized;
          }
          auto client = co_await state->read_client(std::move(target), verification);
          auto request = forge::chain::protocol::block_request{.audit = forge::chain::protocol::audit_mode::required};
          if (const auto* id = std::get_if<forge::chain::protocol::block_id>(&*block)) {
             request.id = *id;
          } else {
             request.num = std::get<std::uint32_t>(*block);
          }
          const auto header = co_await client.get_header(std::move(request));
          const auto snapshot = forge::api::chain::read_snapshot{
              *header.context.anchor, forge::chain::protocol::time_point_sec{header.header.timestamp.to_time_point()},
              [&](auto request) { return client.get_table_rows(std::move(request)); },
              [&](auto request) { return client.get_code(std::move(request)); }};
          co_await handler(snapshot);
       }, profile ? profile->timeout : state->options.delivery.timeout);
}

boost::asio::awaitable<forge::chain::protocol::block_response>
plugin::api_impl::get_block(forge::api::chain::destination target, forge::chain::protocol::block_id block) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
   const auto& verification = verification_for(target, nullptr);
   auto cancellation = forge::asio::cancellation_scope{co_await boost::asio::this_coro::executor,
                                                       std::array{state->stop.get_token()},
                                                       co_await boost::asio::this_coro::cancellation_state};
   co_return co_await cancellation.run<forge::chain::protocol::block_response>(
       [&](std::stop_token) -> boost::asio::awaitable<forge::chain::protocol::block_response> {
          auto client = co_await state->read_client(std::move(target), verification);
          co_return co_await client.get_block({.id = block, .audit = forge::chain::protocol::audit_mode::required});
       }, state->options.delivery.timeout);
}

boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
plugin::api_impl::status(forge::api::chain::destination target, forge::chain::protocol::transaction_id id) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
   if (!state->queue) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Chain write profile is not configured");
   }
   auto result = co_await state->queue->result(state->options.verification.chain, std::move(target), id,
                                              state->stop.get_token());
   if (!result) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::audit_not_supported,
                            "Transaction status is not yet verified");
   }
   co_return std::move(*result);
}

} // namespace forge::plugins::chain::outbox
