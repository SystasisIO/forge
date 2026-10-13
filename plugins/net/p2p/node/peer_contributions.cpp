module;

#include <forge/exceptions/macros.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.plugins.net.p2p.node.peer_contribution;

#include "details/peer_contributions.hxx"

namespace forge::plugins::net::p2p::node::detail {

peer_contributions::peer_contributions() = default;
peer_contributions::~peer_contributions() noexcept { close(); }

void peer_contributions::attach(std::shared_ptr<forge::net::p2p::node> node,
                                const forge::net::p2p::topology::policy& limits) {
   const auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P peer contribution admission is closed");
   }
   if (node_ || !node || !limits.max_tagged_peers || !limits.max_tags_per_peer || !limits.max_tag_size) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options, "P2P peer contribution owner is invalid");
   }
   node_ = std::move(node);
   max_peers_ = limits.max_tagged_peers;
   max_per_peer_ = limits.max_tags_per_peer;
   max_key_size_ = limits.max_tag_size;
}

std::uint64_t peer_contributions::magnitude(std::int64_t value) noexcept {
   return value < 0 ? std::uint64_t(-(value + 1)) + 1 : std::uint64_t(value);
}

peer_contribution peer_contributions::protect(forge::net::p2p::peer_id peer) {
   return add(std::move(peer), kind::protection, 0);
}
peer_contribution peer_contributions::tag(forge::net::p2p::peer_id peer, std::int64_t value) {
   return add(std::move(peer), kind::score, value);
}

peer_contribution peer_contributions::add(forge::net::p2p::peer_id peer, kind type, std::int64_t value) {
   auto contribution = std::make_shared<record>();
   contribution->cleanup_peer = peer;
   contribution->type = type;
   contribution->value = value;
   const auto weak = std::weak_ptr<peer_contributions>{shared_from_this()};
   auto token = peer_contribution_access::make(
       [weak, contribution] { if (const auto owner = weak.lock()) { owner->release(contribution); } },
       [contribution] { return contribution->active.load(std::memory_order_acquire); });
   // Token closures and release arguments exist before any native mutation.
   const auto lock = std::scoped_lock{mutex_};
   if (closed_ || !node_) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P peer contribution admission is closed");
   }
   if (peer.value.empty()) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options, "P2P peer contribution requires a peer");
   }
   auto found = peers_.find(peer);
   if ((found == peers_.end() && peers_.size() >= max_peers_) ||
       (found != peers_.end() && found->second.records.size() >= max_per_peer_) ||
       next_id_ == (std::numeric_limits<std::uint64_t>::max)()) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::backpressure_rejected, "P2P peer contribution limit reached");
   }
   const auto amount = magnitude(value);
   const auto positive = found == peers_.end() ? 0 : found->second.positive;
   const auto negative = found == peers_.end() ? 0 : found->second.negative;
   if (type == kind::score &&
       ((value >= 0 && amount > std::uint64_t((std::numeric_limits<std::int64_t>::max)()) - positive) ||
        (value < 0 && amount > (std::uint64_t{1} << 63) - negative))) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options, "P2P peer contribution subtotal overflow");
   }
   contribution->id = ++next_id_;
   contribution->cleanup_key = "forge:plugin:" + std::to_string(contribution->id);
   if (contribution->cleanup_key.size() > max_key_size_) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options, "P2P peer contribution key exceeds native bound");
   }
   auto native_key = contribution->cleanup_key;
   auto [position, inserted] = peers_.try_emplace(peer);
   try {
      position->second.records.emplace(contribution->id, contribution);
      if (type == kind::protection) { node_->protect_peer(std::move(peer), std::move(native_key)); }
      else { node_->tag_peer(std::move(peer), std::move(native_key), value); }
   } catch (...) {
      position->second.records.erase(contribution->id);
      if (inserted) { peers_.erase(position); }
      throw;
   }
   // Native protect/tag provide the strong exception guarantee. Commit cannot throw.
   if (type == kind::score) {
      if (value >= 0) { position->second.positive += amount; }
      else { position->second.negative += amount; }
   }
   contribution->active.store(true, std::memory_order_release);
   return token;
}

void peer_contributions::remove_native(record& contribution) noexcept {
   // Native keyed removal performs no allocation. Unexpected failures are not
   // swallowed as successful cleanup; noexcept terminates on a broken invariant.
   if (contribution.type == kind::protection) {
      static_cast<void>(node_->unprotect_peer(std::move(contribution.cleanup_peer),
                                            std::move(contribution.cleanup_key)));
   } else {
      static_cast<void>(node_->untag_peer(std::move(contribution.cleanup_peer), contribution.cleanup_key));
   }
   contribution.active.store(false, std::memory_order_release);
}

void peer_contributions::release(const std::shared_ptr<record>& contribution) noexcept {
   const auto lock = std::scoped_lock{mutex_};
   if (!contribution->active.load(std::memory_order_acquire)) { return; }
   const auto found = peers_.find(contribution->cleanup_peer);
   remove_native(*contribution);
   if (contribution->type == kind::score) {
      if (contribution->value >= 0) { found->second.positive -= magnitude(contribution->value); }
      else { found->second.negative -= magnitude(contribution->value); }
   }
   found->second.records.erase(contribution->id);
   if (found->second.records.empty()) { peers_.erase(found); }
}

void peer_contributions::close() noexcept {
   auto retired = decltype(peers_){};
   auto node = std::shared_ptr<forge::net::p2p::node>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      closed_ = true;
      for (auto& peer : peers_) {
         for (auto& contribution : peer.second.records) { remove_native(*contribution.second); }
      }
      retired.swap(peers_);
      node.swap(node_);
   }
   // Release native ownership and all token records outside the registry lock.
}

} // namespace forge::plugins::net::p2p::node::detail
