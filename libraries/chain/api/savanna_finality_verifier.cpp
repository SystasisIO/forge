module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

module forge.chain.api.savanna_finality_verifier;

import forge.chain.api.exceptions;
import forge.crypto.digest.sha256;

#include "details/savanna_finality_trust_store.hxx"

namespace forge::chain::api {
namespace {

template <typename Exception>
[[noreturn]] void throw_public(const forge::exceptions::base& error,
                               std::source_location location = std::source_location::current()) {
   throw Exception{error.message(), error.context(), location};
}

bool is_public_chain_api_failure(const forge::exceptions::base& error) {
   return error.code().category() == exceptions::forge_exceptions_category(exceptions::code::invalid_request);
}

template <typename Operation> decltype(auto) translate_finality_failure(Operation&& operation) {
   try {
      return std::invoke(std::forward<Operation>(operation));
   } catch (const exceptions::trust_required&) {
      throw;
   } catch (const exceptions::invalid_request&) {
      throw;
   } catch (const exceptions::wrong_chain&) {
      throw;
   } catch (const exceptions::resource_exhausted&) {
      throw;
   } catch (const exceptions::invalid_finality&) {
      throw;
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "Savanna finality witness allocation failed");
   } catch (const std::length_error& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "Savanna finality witness allocation exceeds its limit",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const savanna::exceptions::finality_witness_limit_exceeded& error) {
      throw_public<exceptions::resource_exhausted>(error);
   } catch (const savanna::exceptions::finality_witness_wrong_chain& error) {
      throw_public<exceptions::wrong_chain>(error);
   } catch (const savanna::exceptions::untrusted_finality_bootstrap& error) {
      throw_public<exceptions::trust_required>(error);
   } catch (const savanna::exceptions::invalid_finality_witness& error) {
      throw_public<exceptions::invalid_finality>(error);
   } catch (const savanna::exceptions::finality_anchor_mismatch& error) {
      throw_public<exceptions::invalid_finality>(error);
   } catch (const forge::exceptions::base& error) {
      throw_public<exceptions::invalid_finality>(error);
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality, "Savanna finality witness verification failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "Savanna finality witness verification failed with a non-standard error");
   }
}

savanna::finality_replay project_replay_through(savanna::finality_replay replay,
                                                 const protocol::state_anchor& expected) {
   const auto anchor = std::ranges::find(replay.anchors, expected);
   if (anchor == replay.anchors.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "Savanna finality replay does not contain the requested anchor");
   }
   replay.anchors.erase(std::next(anchor), replay.anchors.end());

   const auto opportunity = std::ranges::find_if(replay.producer_opportunities, [&](const auto& value) {
      return value.produced_block && *value.produced_block == expected.block;
   });
   if (opportunity == replay.producer_opportunities.end()) {
      if (replay.anchors.size() != 1U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                               "Savanna finality replay has no producer opportunity for the requested anchor");
      }
      replay.producer_opportunities.clear();
   } else {
      replay.producer_opportunities.erase(std::next(opportunity), replay.producer_opportunities.end());
   }
   replay.finalized_block_num = std::min(replay.finalized_block_num, expected.block_num);
   replay.validated_block_num = std::min(replay.validated_block_num, expected.block_num);
   return replay;
}

} // namespace

struct savanna_finality_verifier::impl {
   impl(savanna::finality_trust trust, std::vector<savanna::finality_trust> additional_trusts,
        savanna::finality_witness_limits limits)
       : trust_store{std::move(trust), std::move(additional_trusts), limits} {}

   detail::savanna_finality_trust_store trust_store;
};

std::unique_ptr<savanna_finality_verifier::impl>
savanna_finality_verifier::make_impl(savanna::finality_trust trust,
                                     std::vector<savanna::finality_trust> additional_trusts,
                                     savanna::finality_witness_limits limits) {
   try {
      return std::make_unique<impl>(std::move(trust), std::move(additional_trusts), limits);
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "Savanna finality verifier allocation failed");
   } catch (const std::length_error& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "Savanna finality verifier allocation exceeds its limit",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const forge::exceptions::base& error) {
      if (is_public_chain_api_failure(error)) {
         throw;
      }
      throw_public<exceptions::trust_required>(error);
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality trust is invalid",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality trust is invalid");
   }
}

savanna_finality_verifier::savanna_finality_verifier(savanna::finality_trust trust,
                                                     savanna::finality_witness_limits limits)
    : savanna_finality_verifier(std::move(trust), {}, limits) {}

savanna_finality_verifier::savanna_finality_verifier(savanna::finality_trust trust,
                                                     std::vector<savanna::finality_trust> additional_trusts,
                                                     savanna::finality_witness_limits limits)
    : impl_{make_impl(std::move(trust), std::move(additional_trusts), limits)} {}

savanna_finality_verifier::~savanna_finality_verifier() = default;

savanna_finality_verifier::savanna_finality_verifier(savanna_finality_verifier&&) noexcept = default;

savanna_finality_verifier& savanna_finality_verifier::operator=(savanna_finality_verifier&&) noexcept = default;

std::optional<protocol::chain_id> savanna_finality_verifier::trusted_chain() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   return impl_->trust_store.trusted_chain();
}

savanna::finality_trust savanna_finality_verifier::preferred_trust() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   return translate_finality_failure([&] { return impl_->trust_store.preferred_trust(); });
}

std::optional<protocol::block_id> savanna_finality_verifier::preferred_trust_anchor() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   return translate_finality_failure([&] { return impl_->trust_store.preferred_trust_anchor(); });
}

std::optional<protocol::block_id>
savanna_finality_verifier::trust_anchor_at_or_before(protocol::block_num target) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   return translate_finality_failure([&] { return impl_->trust_store.trust_anchor_at_or_before(target); });
}

savanna::header_state savanna_finality_verifier::replay_state(const protocol::state_anchor& expected,
                                                              const protocol::proof_blob& proof) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   return translate_finality_failure([&] {
      const auto proof_digest = forge::crypto::digest::sha256::hash(proof);
      const auto limits = impl_->trust_store.witness_limits();
      if (const auto cached = impl_->trust_store.cached_replay_state(expected, proof_digest)) {
         return *cached;
      }
      const auto witness = savanna::decode_finality_witness(proof, limits);
      const auto snapshot = impl_->trust_store.take_snapshot(expected, witness);
      return savanna::replay_finality_witness_state(*snapshot.source_trust, witness, expected, limits);
   });
}

savanna::finality_replay savanna_finality_verifier::verified_replay(const protocol::state_anchor& expected,
                                                                    const protocol::proof_blob& proof) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   return translate_finality_failure([&] {
      const auto proof_digest = forge::crypto::digest::sha256::hash(proof);
      const auto limits = impl_->trust_store.witness_limits();
      if (const auto cached = impl_->trust_store.cached_replay(expected, proof_digest)) {
         return project_replay_through(*cached, expected);
      }
      const auto witness = savanna::decode_finality_witness(proof, limits);
      const auto snapshot = impl_->trust_store.take_snapshot(expected, witness);
      auto replay = savanna::advance_finality_trust_with_replay(*snapshot.source_trust, witness, expected, limits).replay;
      return project_replay_through(std::move(replay), expected);
   });
}

void savanna_finality_verifier::verify(const protocol::state_anchor& anchor, const protocol::proof_blob& proof) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   auto verified = translate_finality_failure([&] {
      auto proof_digest = forge::crypto::digest::sha256::hash(proof);
      const auto limits = impl_->trust_store.witness_limits();
      const auto witness = savanna::decode_finality_witness(proof, limits);
      const auto snapshot = impl_->trust_store.take_snapshot(anchor, witness);
      auto advance = savanna::advance_finality_trust_with_replay(*snapshot.source_trust, witness, anchor, limits);
      const auto position = detail::savanna_finality_trust_store::checkpoint_position(advance.checkpoint);
      if (position.finalized_block_num > snapshot.preferred.finalized_block_num &&
          !detail::savanna_finality_trust_store::replay_contains(advance.replay, snapshot.preferred)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                               "Savanna finality checkpoint does not descend from the snapshot preferred checkpoint");
      }
      auto state = advance.checkpoint.value.state;
      return std::tuple{std::move(proof_digest), std::move(advance.checkpoint), std::move(advance.replay),
                        std::move(state)};
   });
   auto proof_digest = std::move(std::get<0>(verified));
   auto checkpoint = std::move(std::get<1>(verified));
   auto replay = std::move(std::get<2>(verified));
   auto state = std::move(std::get<3>(verified));
   translate_finality_failure([&] {
      impl_->trust_store.install_verified(std::move(checkpoint), std::move(replay), anchor, proof_digest,
                                          std::move(state));
   });
}

void savanna_finality_verifier::verify_ancestry(const protocol::state_anchor& finalized,
                                                std::span<const protocol::state_anchor> intermediate,
                                                const protocol::proof_blob& proof) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality verifier is not initialized");
   }
   const auto limits = impl_->trust_store.witness_limits();
   translate_finality_failure([&] {
      const auto witness = savanna::decode_finality_witness(proof, limits);
      const auto snapshot = impl_->trust_store.take_snapshot(finalized, witness);
      savanna::verify_finality_ancestry_witness(*snapshot.source_trust, proof, finalized, intermediate, limits);
   });
}

namespace {

template <typename... Arguments>
std::shared_ptr<savanna_finality_verifier> make_public_finality_verifier(Arguments&&... arguments) {
   try {
      return std::make_shared<savanna_finality_verifier>(std::forward<Arguments>(arguments)...);
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "Savanna finality verifier allocation failed");
   } catch (const std::length_error& error) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "Savanna finality verifier allocation exceeds its limit",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const forge::exceptions::base& error) {
      if (is_public_chain_api_failure(error)) {
         throw;
      }
      throw_public<exceptions::trust_required>(error);
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality trust is invalid",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality trust is invalid");
   }
}

} // namespace

std::shared_ptr<savanna_finality_verifier> make_savanna_finality_verifier(savanna::finality_trust trust,
                                                                          savanna::finality_witness_limits limits) {
   return make_public_finality_verifier(std::move(trust), limits);
}

std::shared_ptr<savanna_finality_verifier>
make_savanna_finality_verifier_with_trusts(savanna::finality_trust trust,
                                           std::vector<savanna::finality_trust> additional_trusts,
                                           savanna::finality_witness_limits limits) {
   return make_public_finality_verifier(std::move(trust), std::move(additional_trusts), limits);
}

} // namespace forge::chain::api
