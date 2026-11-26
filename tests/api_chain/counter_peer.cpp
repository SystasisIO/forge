module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <forge/exceptions/macros.hpp>

#include <bit>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

module test.api_chain.counter_peer;

import forge.api.core.binding;
import forge.api.core.registry;
import forge.app.application_shell;
import forge.app.plugin_context;
import forge.chain.api.exceptions;
import forge.chain.api.info;
import forge.chain.api.block;
import forge.chain.api.state;
import test.api_chain.counter_read;
import forge.chain.api.submission;
import forge.chain.api.transaction;
import forge.chain.protocol.block;
import forge.raw.codec;

namespace demo {
namespace api = forge::chain::api;
namespace protocol = forge::chain::protocol;

struct counter_peer::state final : api::info, api::transaction, api::submission, api::finality_verifier {
   explicit state(forge::crypto::asymmetric::public_key expected, bool accept)
       : signer(std::move(expected)), accept_pending(accept) {
      genesis.chain = protocol::chain_id{"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
      checkpoint = counter_genesis(genesis.chain, genesis_time);
      genesis = *checkpoint.context.anchor;
   }

   std::optional<protocol::chain_id> trusted_chain() const override {
      return genesis.chain;
   }
   std::optional<protocol::block_id> preferred_trust_anchor() const override {
      return genesis.block;
   }
   std::optional<protocol::block_id> trust_anchor_at_or_before(protocol::block_num target) const override {
      return target >= genesis.block_num ? std::optional{genesis.block} : std::nullopt;
   }

   void verify(const protocol::state_anchor& anchor, const protocol::proof_blob& proof) override {
      std::lock_guard lock(mutex);
      if (proof.scheme != "fixture.counter" || (anchor != genesis && (!included || anchor != *included))) {
         throw std::runtime_error("unexpected counter fixture checkpoint");
      }
   }

   void verify_ancestry(const protocol::state_anchor&, std::span<const protocol::state_anchor>,
                        const protocol::proof_blob&) override {
      throw std::logic_error("counter fixture supplies direct checkpoints");
   }

   boost::asio::awaitable<protocol::info_response> get(protocol::anchored_request) override {
      std::lock_guard lock(mutex);
      const auto& anchor = included ? *included : genesis;
      protocol::info_response response;
      response.chain = anchor.chain;
      response.context = {anchor.chain, anchor.block, anchor.block, anchor};
      response.head = response.finalized = anchor.block;
      response.head_num = response.finalized_num = anchor.block_num;
      response.head_time = response.finalized_time = genesis_time;
      response.audit = protocol::audit_bundle{.finality = protocol::proof_blob{.scheme = "fixture.counter"}};
      co_return response;
   }

   void apply(const protocol::packed_transaction& packed) {
      const auto tx = packed.get_signed_transaction();
      if (tx.actions.size() != 1 || tx.signatures.size() != 1 ||
          forge::crypto::asymmetric::recover(
              tx.signatures.front(), protocol::signature_digest(genesis.chain, tx, tx.context_free_data)) != signer) {
         throw std::runtime_error("unexpected signed counter transaction");
      }
      const auto& action = tx.actions.front();
      if (action.name != protocol::action_name{"increment"} || action.account != protocol::account_name{"counter"} ||
          action.authorization != std::vector{protocol::permission_level{protocol::account_name{"alice"},
                                                                         protocol::permission_name{"active"}}}) {
         throw std::runtime_error("unexpected counter action or authority");
      }
      if (receipt && std::get<protocol::packed_transaction>(receipt->trx).id() == packed.id()) {
         return;
      }
      if (forge::raw::unpack_exact<std::uint64_t>(action.data) != counter) {
         throw std::runtime_error("counter compare-and-increment precondition failed");
      }
      ++counter;
      receipt.emplace();
      receipt->status = decltype(receipt->status)::executed;
      receipt->trx = packed;
      included = genesis;
      included->block_num = 2;
      included->transaction_root = receipt->digest();
      included->block = included->transaction_root;
      included->block._hash[0] = std::byteswap(included->block_num);
   }

   boost::asio::awaitable<protocol::transaction_submit_response>
   submit(protocol::transaction_submit_request req) override {
      std::lock_guard lock(mutex);
      received.push_back(req.transaction);
      changed.notify_all();
      if (!execute && !accept_pending) {
         FORGE_THROW_EXCEPTION(api::exceptions::unavailable, "counter fixture dropped the submission response");
      }
      if (execute) {
         apply(req.transaction);
         show_receipt = true;
      }
      co_return protocol::transaction_submit_response{.id = req.transaction.id(),
                                                      .state = protocol::transaction_lifecycle::accepted};
   }

   boost::asio::awaitable<protocol::transaction_status_response>
   get_status(protocol::transaction_status_request req) override {
      std::lock_guard lock(mutex);
      protocol::transaction_status_response status;
      status.id = req.id;
      if (!show_receipt || !receipt || std::get<protocol::packed_transaction>(receipt->trx).id() != req.id) {
         status.context = {genesis.chain, genesis.block, genesis.block, genesis};
         status.head = status.finalized = genesis.block;
         status.head_num = status.finalized_num = genesis.block_num;
         if (req.audit == protocol::audit_mode::required) {
            FORGE_THROW_EXCEPTION(api::exceptions::audit_not_supported, "counter not finalized");
         }
         co_return status;
      }
      status.state = protocol::transaction_lifecycle::finalized;
      status.context = {genesis.chain, included->block, included->block, included};
      status.block = status.head = status.finalized = included->block;
      status.block_num = status.head_num = status.finalized_num = included->block_num;
      status.receipt = receipt;
      if (req.audit == protocol::audit_mode::required) {
         status.audit = protocol::audit_bundle{.finality = protocol::proof_blob{.scheme = "fixture.counter"},
                                               .transaction = protocol::transaction_inclusion_proof{
                                                   .leaf = receipt->digest(), .index = 0, .leaf_count = 1}};
         served_proof = true;
         changed.notify_all();
      }
      co_return status;
   }

   boost::asio::awaitable<protocol::transaction_status_response>
   await_transaction(protocol::transaction_await_request req) override {
      co_return co_await get_status({.id = req.id, .finality_from = req.finality_from, .audit = req.audit});
   }
   boost::asio::awaitable<std::vector<protocol::public_key>>
   get_required_keys(protocol::transaction_required_keys_request) override {
      throw std::logic_error("unexpected get_required_keys");
      co_return std::vector<protocol::public_key>{};
   }
   boost::asio::awaitable<protocol::transaction_read_only_response>
   compute_transaction(protocol::transaction_read_only_request) override {
      throw std::logic_error("unexpected compute_transaction");
      co_return protocol::transaction_read_only_response{};
   }
   boost::asio::awaitable<protocol::transaction_read_only_response>
   send_read_only_transaction(protocol::transaction_read_only_request) override {
      throw std::logic_error("unexpected send_read_only_transaction");
      co_return protocol::transaction_read_only_response{};
   }
   boost::asio::awaitable<std::vector<protocol::transaction_submit_response>>
   submit_batch(protocol::transaction_submit_batch_request) override {
      throw std::logic_error("unexpected submit_batch");
      co_return std::vector<protocol::transaction_submit_response>{};
   }

   forge::crypto::asymmetric::public_key signer;
   protocol::block_header_response checkpoint;
   std::shared_ptr<read_counts> reads = std::make_shared<read_counts>();
   protocol::state_anchor genesis;
   const protocol::time_point genesis_time{
       std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now())};
   std::optional<protocol::state_anchor> included;
   std::optional<protocol::transaction_receipt> receipt;
   std::vector<protocol::packed_transaction> received;
   std::uint64_t counter = 7;
   std::mutex mutex;
   std::condition_variable changed;
   const bool accept_pending;
   bool execute = false;
   bool served_proof = false;
   bool show_receipt = false;
};

counter_peer::counter_peer(forge::crypto::asymmetric::public_key signer, bool accept_pending)
    : state_(std::make_shared<state>(std::move(signer), accept_pending)) {}
counter_peer::~counter_peer() = default;

void counter_peer::install(forge::api::core::installer apis) const {
   install_counter_reads(apis, state_->checkpoint, state_->reads);
   apis.install<api::info>(state_);
   apis.install<api::submission>(state_);
   apis.install<api::transaction>(state_);
}

unsigned counter_peer::header_reads() const { return state_->reads->headers.load(); }
unsigned counter_peer::row_reads() const { return state_->reads->rows.load(); }

package_chain_api_component::p2p_publication_callbacks counter_peer::publication() const {
   return {
       .install = [peer = *this](forge::app::application_context& ctx) -> boost::asio::awaitable<void> {
          peer.install(ctx.apis());
          co_return;
       },
       .binding =
           [](forge::app::plugin_context& ctx) {
              return forge::api::core::binding()
                  .serve(ctx.apis())
                  .export_api<api::block>(api::block::ref())
                  .export_api<api::state>(api::state::ref())
                  .export_api<api::info>(api::info::ref())
                  .export_api<api::submission>(api::submission::ref())
                  .export_api<api::transaction>(api::transaction::ref())
                  .build();
           },
   };
}

std::shared_ptr<api::finality_verifier> counter_peer::finality() const {
   return state_;
}

void counter_peer::allow_execution() {
   std::lock_guard lock(state_->mutex);
   state_->execute = true;
}

void counter_peer::include_first_submission(bool publish_receipt) {
   std::lock_guard lock(state_->mutex);
   state_->apply(state_->received.at(0));
   state_->show_receipt = publish_receipt;
}

bool counter_peer::wait_for_submissions(std::size_t count, std::chrono::seconds timeout) const {
   std::unique_lock lock(state_->mutex);
   return state_->changed.wait_for(lock, timeout, [&] { return state_->received.size() >= count; });
}

bool counter_peer::wait_for_proof(std::chrono::seconds timeout) const {
   std::unique_lock lock(state_->mutex);
   return state_->changed.wait_for(lock, timeout, [&] { return state_->served_proof; });
}

std::vector<protocol::packed_transaction> counter_peer::submissions() const {
   std::lock_guard lock(state_->mutex);
   return state_->received;
}

std::uint64_t counter_peer::value() const {
   std::lock_guard lock(state_->mutex);
   return state_->counter;
}
} // namespace demo
