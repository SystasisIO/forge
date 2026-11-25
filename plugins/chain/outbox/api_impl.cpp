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
#include <vector>

module forge.plugins.chain.outbox.plugin;

import forge.api.chain.outbox;
import forge.api.chain.runtime;
import forge.api.core.exceptions;
import forge.asio.cancellation_scope;
import forge.asio.notification;
import forge.asio.task;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.info;
import forge.chain.transaction.builder;
import forge.db.object.transaction;
import forge.exceptions;

import forge.net.http.base_url;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::chain::outbox {

plugin::api_impl::api_impl(std::shared_ptr<impl> state) : _state{std::move(state)} {}

boost::asio::awaitable<forge::chain::transaction::builder>
plugin::api_impl::begin(forge::api::chain::destination target) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
   const auto parent = co_await boost::asio::this_coro::cancellation_state;
   auto cancellation = forge::asio::cancellation_scope{
       co_await boost::asio::this_coro::executor, std::array{state->stop.get_token()}, parent};
   const auto info = co_await cancellation.run<forge::chain::protocol::info_response>(
       [state, target = std::move(target)](std::stop_token) mutable { return state->info(std::move(target)); },
       state->options.delivery.timeout);
   state->require_running();
   co_return forge::chain::transaction::builder{{
       .chain = info.chain,
       .reference_block = info.finalized,
       .reference_time = forge::chain::protocol::time_point_sec{
           std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())},
   }};
}

boost::asio::awaitable<forge::chain::protocol::transaction_id>
plugin::api_impl::submit(forge::api::chain::destination target, forge::api::core::api_ref contract, std::string method,
                         forge::chain::transaction::unsigned_transaction transaction) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
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
   auto admit = outbox::admission{};
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

boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
plugin::api_impl::status(forge::api::chain::destination target, forge::chain::protocol::transaction_id id) {
   const auto state = _state;
   state->begin_call();
   auto finished = boost::scope::scope_exit{[&] { state->end_call(); }};
   auto result = co_await state->queue->result(state->options.verification.chain, std::move(target), id,
                                              state->stop.get_token());
   if (!result) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::audit_not_supported,
                            "Transaction status is not yet verified");
   }
   co_return std::move(*result);
}

} // namespace forge::plugins::chain::outbox
