module;

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.core.transfer;

namespace forge::swarm::core {
namespace {

std::uint64_t rank(std::uint64_t seed, std::uint64_t value) noexcept {
   // SplitMix64 finalizer: specified integer operations, no library RNG state.
   auto x = seed + value + 0x9e3779b97f4a7c15ULL;
   x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
   x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
   return x ^ (x >> 31U);
}

void add(std::uint64_t& counter, std::uint64_t amount = 1) noexcept {
   counter += std::min(amount, std::numeric_limits<std::uint64_t>::max() - counter);
}

std::uint64_t next(std::uint64_t& counter) {
   if (counter == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "transfer identity space exhausted");
   }
   return counter++;
}

void set_bit(std::vector<std::uint8_t>& bitmap, std::uint32_t index) {
   bitmap[index / 8U] |= static_cast<std::uint8_t>(1U << (index % 8U));
}

} // namespace

transfer::transfer(descriptor value, config limits, std::uint64_t tie_seed, std::optional<resume_state> local)
    : _descriptor(std::move(value)), _limits(limits), _seed(tie_seed) {
   if (limits.peers == 0U || limits.peers > 4096U || limits.requests == 0U || limits.requests > 65536U ||
       limits.requests_per_peer == 0U || limits.requests_per_peer > limits.requests || limits.request_bytes == 0U ||
       limits.request_bytes > std::uint64_t{65536} * block_size || limits.request_bytes_per_peer == 0U ||
       limits.request_bytes_per_peer > limits.request_bytes || limits.unfinished_pieces == 0U ||
       limits.unfinished_pieces > max_unfinished_pieces || limits.unfinished_bytes == 0U ||
       limits.unfinished_bytes > std::uint64_t{max_unfinished_pieces} * max_piece_size ||
       limits.request_timeout.count() <= 0 || limits.endgame_blocks > 4096U || limits.endgame_replicas == 0U ||
       limits.endgame_replicas > 4U || limits.endgame_requests > limits.requests) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "invalid transfer resource bounds");
   }
   const auto summary = validate_descriptor(_descriptor);
   _id = calculate_swarm_id(_descriptor);
   _pieces.reserve(summary.total_pieces);
   for (auto file = 0U; file < _descriptor.files.size(); ++file) {
      const auto count = piece_count(_descriptor.files[file].size, _descriptor.piece_size);
      if (count == 0U)
         continue;
      const auto tail = piece_length(_descriptor, {file, count - 1U});
      for (auto index = 0U; index < count; ++index) {
         _pieces.push_back(
             {{file, index}, index + 1U == count ? tail : _descriptor.piece_size, 0U, 1U, rank(_seed, _pieces.size())});
      }
   }
   _wanted = summary.total_pieces;
   _verified.resize((summary.total_pieces + 7U) / 8U);
   if (!local)
      return;
   validate_checkpoint(*local, _descriptor);
   auto bytes = std::uint64_t{};
   for (const auto& item : local->unfinished)
      bytes += piece_length(_descriptor, item.piece);
   if (local->unfinished.size() > limits.unfinished_pieces || bytes > limits.unfinished_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "resume exceeds unfinished transfer bounds");
   }
   _verified = local->verified;
   for (auto i = 0U; i < _pieces.size(); ++i) {
      if (bitmap_test(_verified, i))
         ++_verified_count;
   }
   _wanted -= _verified_count;
   for (const auto& item : local->unfinished) {
      const auto index = ordinal(item.piece);
      auto state = partial_state{next(_next_generation),
                                 std::vector<block_state>((_pieces[index].length + block_size - 1U) / block_size)};
      for (auto block = 0U; block < state.blocks.size(); ++block) {
         state.blocks[block].received = bitmap_test(item.blocks, block);
      }
      _partials.emplace(index, std::move(state));
   }
   _unfinished_bytes = bytes;
}

void transfer::validate_time(transfer_time now) const {
   if (now < _time) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "transfer time must be nonnegative and monotonic");
   }
}

transfer::peer_state& transfer::peer(peer_id id) {
   const auto found = _peers.find(id);
   if (found == _peers.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "unknown transfer peer");
   }
   return found->second;
}

const transfer::peer_state& transfer::peer(peer_id id) const {
   const auto found = _peers.find(id);
   if (found == _peers.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "unknown transfer peer");
   }
   return found->second;
}

void transfer::validate_bitmap(std::span<const std::uint8_t> bitmap) const {
   const auto count = static_cast<std::uint32_t>(_pieces.size());
   if (bitmap.size() != (count + 7U) / 8U || (count % 8U != 0U && (bitmap.back() >> (count % 8U)) != 0U)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "peer bitmap must have exact piece geometry");
   }
}

transfer::order_key transfer::key(std::uint32_t index) const noexcept {
   return key(index, _pieces[index].available, _pieces[index].priority, _selection);
}

transfer::order_key transfer::key(std::uint32_t index, std::uint32_t available, std::uint8_t priority,
                                  selection policy) const noexcept {
   return {static_cast<std::uint8_t>(255U - priority), policy == selection::rarest_first ? available : 0U,
           policy == selection::rarest_first ? _pieces[index].seed_rank : index, index};
}

void transfer::change_availability(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after) {
   // Scan bytes, but visit and reindex only changed bits. Empty means all absent.
   const auto changes = [&](auto&& apply) {
      for (auto byte = std::size_t{}; byte < std::max(before.size(), after.size()); ++byte) {
         const auto old_bits = byte < before.size() ? before[byte] : 0U;
         const auto new_bits = byte < after.size() ? after[byte] : 0U;
         auto changed = static_cast<std::uint32_t>(old_bits ^ new_bits);
         while (changed != 0U) {
            const auto bit = static_cast<std::uint32_t>(std::countr_zero(changed));
            apply(static_cast<std::uint32_t>(byte * 8U + bit), (new_bits & (1U << bit)) != 0U);
            changed &= changed - 1U;
         }
      }
   };
   // Only zero-to-one availability needs new nodes. Prepare all of them before
   // changing counts; retained nodes are rekeyed without allocation below.
   auto added = std::set<order_key>{};
   changes([&](std::uint32_t index, bool present) {
      const auto& piece = _pieces[index];
      if (present && piece.available == 0U && piece.priority != 0U && !bitmap_test(_verified, index)) {
         added.insert(key(index, 1U, piece.priority, _selection));
      }
   });
   changes([&](std::uint32_t index, bool present) {
      auto& piece = _pieces[index];
      const auto available = present ? piece.available + 1U : piece.available - 1U;
      if (piece.available != 0U && piece.priority != 0U && !bitmap_test(_verified, index)) {
         if (available == 0U)
            _order.erase(key(index));
         else if (_selection == selection::rarest_first) {
            auto node = _order.extract(key(index));
            node.value() = key(index, available, piece.priority, _selection);
            _order.insert(std::move(node));
         }
      }
      piece.available = available;
   });
   _order.merge(added);
}

std::uint32_t transfer::ordinal(piece_id piece) const {
   return piece_ordinal(_descriptor, piece);
}

std::uint32_t transfer::block_length(std::uint32_t index, std::uint32_t block) const {
   return std::min(block_size, _pieces[index].length - block * block_size);
}

peer_id transfer::add_peer(std::span<const std::uint8_t> bitmap) {
   if (_stopped) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "transfer is stopped");
   }
   validate_bitmap(bitmap);
   if (_peers.size() == _limits.peers) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "transfer peer bound reached");
   }
   const auto id = next(_next_peer);
   _peers.emplace(id, peer_state{std::vector<std::uint8_t>(bitmap.begin(), bitmap.end())});
   try {
      change_availability({}, bitmap);
   } catch (...) {
      _peers.erase(id);
      throw;
   }
   return id;
}

void transfer::set_availability(peer_id id, std::span<const std::uint8_t> bitmap) {
   validate_bitmap(bitmap);
   auto& state = peer(id);
   if (std::equal(state.availability.begin(), state.availability.end(), bitmap.begin()))
      return;
   auto replacement = std::vector<std::uint8_t>(bitmap.begin(), bitmap.end());
   change_availability(state.availability, bitmap);
   state.availability = std::move(replacement);
}

std::vector<transfer::request> transfer::remove_peer(peer_id id, transfer_time now) {
   auto canceled = set_choked(id, true, now);
   const auto& state = peer(id);
   change_availability(state.availability, {});
   _peers.erase(id);
   return canceled;
}

std::vector<transfer::request> transfer::set_choked(peer_id id, bool choked, transfer_time now) {
   validate_time(now);
   auto& state = peer(id);
   auto result = std::vector<request>{};
   if (choked) {
      result.reserve(state.requests);
      for (auto it = _requests.begin(); it != _requests.end();) {
         if (it->second.peer != id) {
            ++it;
            continue;
         }
         result.push_back(it->second);
         release(it++);
      }
      prune_empty();
   }
   state.choked = choked;
   _time = now;
   return result;
}

void transfer::set_selection(selection policy) {
   if (policy != selection::rarest_first && policy != selection::sequential) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "unknown transfer selection policy");
   }
   if (policy == _selection)
      return;
   auto reordered = std::set<order_key>{};
   while (!_order.empty()) {
      auto node = _order.extract(_order.begin());
      const auto index = std::get<3>(node.value());
      node.value() = key(index, _pieces[index].available, _pieces[index].priority, policy);
      reordered.insert(std::move(node));
   }
   _order.swap(reordered);
   _selection = policy;
}

void transfer::set_priority(piece_id piece, std::uint8_t priority) {
   const auto index = ordinal(piece);
   auto& state = _pieces[index];
   if (state.priority == priority)
      return;
   if (!bitmap_test(_verified, index)) {
      if (state.available != 0U) {
         if (state.priority == 0U)
            _order.insert(key(index, state.available, priority, _selection));
         else if (priority == 0U)
            _order.erase(key(index));
         else {
            auto node = _order.extract(key(index));
            node.value() = key(index, state.available, priority, _selection);
            _order.insert(std::move(node));
         }
      }
      if (state.priority == 0U)
         ++_wanted;
      if (priority == 0U)
         --_wanted;
   }
   state.priority = priority;
}

transfer::request transfer::issue(peer_id id, std::uint32_t index, std::uint32_t block, transfer_time now,
                                  transfer_time deadline) {
   auto partial = _partials.find(index);
   const auto created = partial == _partials.end();
   if (partial == _partials.end()) {
      auto state = partial_state{next(_next_generation),
                                 std::vector<block_state>((_pieces[index].length + block_size - 1U) / block_size)};
      partial = _partials.emplace(index, std::move(state)).first;
      _unfinished_bytes += _pieces[index].length;
   }
   try {
      auto value = request{next(_next_request),        id,  _pieces[index].id, block * block_size,
                           block_length(index, block), now, deadline};
      _requests.emplace(value.id, value);
      auto& entry = partial->second.blocks[block];
      if (entry.requests++ != 0U)
         ++_duplicates;
      auto& source = peer(id);
      ++source.requests;
      source.bytes += value.length;
      _request_bytes += value.length;
      return value;
   } catch (...) {
      if (created) {
         _partials.erase(partial);
         _unfinished_bytes -= _pieces[index].length;
      }
      throw;
   }
}

bool transfer::endgame() const {
   if (_limits.endgame_blocks == 0U || _limits.endgame_requests == 0U || _limits.endgame_replicas == 1U ||
       _duplicates >= _limits.endgame_requests)
      return false;
   if (_wanted > _partials.size())
      return false;
   auto covered = 0U;
   auto missing = 0U;
   for (const auto& [index, partial] : _partials) {
      if (_pieces[index].priority == 0U)
         continue;
      ++covered;
      for (const auto& block : partial.blocks) {
         if (block.received)
            continue;
         if (block.requests == 0U || ++missing > _limits.endgame_blocks)
            return false;
      }
   }
   return covered == _wanted && missing != 0U;
}

std::vector<transfer::request> transfer::pick(peer_id id, transfer_time now, std::uint32_t count) {
   validate_time(now);
   auto& source = peer(id);
   auto result = std::vector<request>{};
   const auto remaining_bytes = [&] {
      return std::min(_limits.request_bytes - _request_bytes, _limits.request_bytes_per_peer - source.bytes);
   };
   if (_stopped || source.choked || count == 0U || remaining_bytes() == 0U) {
      _time = now;
      return result;
   }
   if (_limits.request_timeout.count() >
       transfer_time::max().time_since_epoch().count() - now.time_since_epoch().count()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "request deadline overflows chrono time");
   }
   const auto deadline = now + _limits.request_timeout;
   count = std::min({count, _limits.requests - static_cast<std::uint32_t>(_requests.size()),
                     _limits.requests_per_peer - source.requests});
   if (count == 0U) {
      _time = now;
      return result;
   }
   result.reserve(count);

   // Only partial pieces need a per-batch view. Global order is maintained by
   // availability/priority transitions, never recreated by receiving a block.
   auto partials = std::vector<std::uint32_t>{};
   partials.reserve(_partials.size());
   for (const auto& [index, partial] : _partials) {
      if (_pieces[index].priority != 0U && bitmap_test(source.availability, index))
         partials.push_back(index);
   }
   std::sort(partials.begin(), partials.end(), [&](auto a, auto b) { return key(a) < key(b); });
   const auto fits = [&](std::uint32_t length) { return length <= remaining_bytes(); };
   const auto minimum_block_length = [&](std::uint32_t index) {
      return block_length(index, (_pieces[index].length - 1U) / block_size);
   };
   const auto first_new_generation = _next_generation;
   const auto previous_time = _time;
   try {
      _time = now;
      auto cursor = _order.begin();
      const auto next_base = [&]() -> std::optional<std::uint32_t> {
         if (_partials.size() == _limits.unfinished_pieces || _unfinished_bytes == _limits.unfinished_bytes) {
            return std::nullopt;
         }
         while (cursor != _order.end()) {
            const auto index = std::get<3>(*cursor++);
            if (_partials.contains(index) || !bitmap_test(source.availability, index) ||
                _pieces[index].length > _limits.unfinished_bytes - _unfinished_bytes ||
                !fits(minimum_block_length(index)))
               continue;
            return index;
         }
         return std::nullopt;
      };
      auto base = next_base();
      auto partial_position = std::size_t{};
      while (base || partial_position < partials.size()) {
         auto use_partial = partial_position < partials.size();
         if (use_partial && base) {
            const auto index = partials[partial_position];
            const auto& left = _pieces[index];
            const auto& right = _pieces[*base];
            const auto pressure = _partials.size() >= _limits.unfinished_pieces - _limits.unfinished_pieces / 4U ||
                                  _unfinished_bytes >= _limits.unfinished_bytes - _limits.unfinished_bytes / 4U;
            if (left.priority != right.priority)
               use_partial = left.priority > right.priority;
            else if (pressure)
               use_partial = true;
            else if (_selection == selection::sequential)
               use_partial = index < *base;
            else
               use_partial = left.available <= right.available;
         }
         const auto index = use_partial ? partials[partial_position++] : *base;
         auto partial = _partials.find(index);
         const auto blocks = (_pieces[index].length + block_size - 1U) / block_size;
         for (auto block = 0U; block < blocks && fits(minimum_block_length(index)); ++block) {
            if (partial != _partials.end() &&
                (partial->second.blocks[block].received || partial->second.blocks[block].requests != 0U))
               continue;
            if (!fits(block_length(index, block)))
               continue;
            result.push_back(issue(id, index, block, now, deadline));
            partial = _partials.find(index);
            if (result.size() == count || remaining_bytes() == 0U)
               return result;
         }
         if (!use_partial)
            base = next_base();
      }
      // Like the donor, duplicate only after exhausting open work, one busy block
      // per pick call. Global tail coverage prevents premature endgame islands.
      if (!result.empty() || !endgame())
         return result;
      auto chosen = std::optional<std::pair<std::uint32_t, std::uint32_t>>{};
      auto best = std::uint64_t{};
      auto copies = std::numeric_limits<std::uint32_t>::max();
      auto owned = std::set<std::pair<piece_id, std::uint32_t>>{};
      for (const auto& [token, value] : _requests) {
         if (value.peer == id)
            owned.emplace(value.piece, value.offset);
      }
      for (const auto& [index, partial] : _partials) {
         if (_pieces[index].priority == 0U || !bitmap_test(source.availability, index) ||
             !fits(minimum_block_length(index)))
            continue;
         for (auto block = 0U; block < partial.blocks.size(); ++block) {
            const auto& state = partial.blocks[block];
            if (state.received || state.requests == 0U || state.requests >= _limits.endgame_replicas ||
                !fits(block_length(index, block)))
               continue;
            if (owned.contains({_pieces[index].id, block * block_size}))
               continue;
            const auto score = rank(_seed ^ id, (std::uint64_t{index} << 32U) | block);
            if (!chosen || _pieces[index].priority > _pieces[chosen->first].priority ||
                (_pieces[index].priority == _pieces[chosen->first].priority &&
                 (state.requests < copies || (state.requests == copies && score < best)))) {
               chosen = std::pair{index, block};
               best = score;
               copies = state.requests;
            }
         }
      }
      if (chosen)
         result.push_back(issue(id, chosen->first, chosen->second, now, deadline));
      return result;
   } catch (...) {
      // No request from a failed batch may become invisible to its caller.
      // Consumed IDs remain consumed; rollback itself allocates nothing.
      for (const auto& value : result)
         release(_requests.find(value.id));
      for (auto it = _partials.begin(); it != _partials.end();) {
         if (it->second.generation < first_new_generation) {
            ++it;
            continue;
         }
         _unfinished_bytes -= _pieces[it->first].length;
         it = _partials.erase(it);
      }
      _time = previous_time;
      throw;
   }
}

void transfer::release(std::map<std::uint64_t, request>::iterator it) {
   const auto& value = it->second;
   auto& source = peer(value.peer);
   --source.requests;
   source.bytes -= value.length;
   _request_bytes -= value.length;
   auto& block = _partials.at(ordinal(value.piece)).blocks[value.offset / block_size];
   if (block.requests-- > 1U)
      --_duplicates;
   _requests.erase(it);
}

void transfer::prune_empty() {
   for (auto it = _partials.begin(); it != _partials.end();) {
      const auto active = std::any_of(it->second.blocks.begin(), it->second.blocks.end(),
                                      [](const auto& block) { return block.received || block.requests != 0U; });
      if (active) {
         ++it;
         continue;
      }
      _unfinished_bytes -= _pieces[it->first].length;
      it = _partials.erase(it);
   }
}

transfer::receive_result transfer::receive(peer_id id, std::uint64_t token, std::uint32_t length, transfer_time now) {
   validate_time(now);
   auto result = receive_result{};
   const auto found = _requests.find(token);
   if (found == _requests.end()) {
      _time = now;
      return result;
   }
   const auto value = found->second;
   if (value.peer != id) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "response does not own this request");
   }
   if (now >= value.deadline) {
      result.canceled.push_back(value);
      _time = now;
      add(peer(id).observation.timeouts);
      release(found);
      prune_empty();
      return result;
   }
   if (length != value.length) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "response must match the exact requested block length");
   }
   const auto index = ordinal(value.piece);
   auto& partial = _partials.at(index);
   auto& block = partial.blocks[value.offset / block_size];
   result.canceled.reserve(block.requests - 1U);
   _time = now;
   auto& observation = peer(id).observation;
   add(observation.received_bytes, length);
   add(observation.responses);
   add(observation.latency_milliseconds, static_cast<std::uint64_t>((now - value.started).count()));
   block.received = true;
   block.source = id;
   release(found);
   for (auto it = _requests.begin(); it != _requests.end();) {
      if (it->second.piece != value.piece || it->second.offset != value.offset) {
         ++it;
         continue;
      }
      result.canceled.push_back(it->second);
      release(it++);
   }
   result.accepted = true;
   result.ready = verification_for(value.piece);
   return result;
}

bool transfer::cancel(peer_id id, std::uint64_t token, transfer_time now) {
   validate_time(now);
   const auto found = _requests.find(token);
   if (found == _requests.end()) {
      _time = now;
      return false;
   }
   if (found->second.peer != id) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "cancel does not own this request");
   }
   release(found);
   prune_empty();
   _time = now;
   return true;
}

std::vector<transfer::request> transfer::expire(transfer_time now) {
   validate_time(now);
   auto result = std::vector<request>{};
   result.reserve(_requests.size());
   _time = now;
   for (auto it = _requests.begin(); it != _requests.end();) {
      if (it->second.deadline > now) {
         ++it;
         continue;
      }
      result.push_back(it->second);
      add(peer(it->second.peer).observation.timeouts);
      release(it++);
   }
   prune_empty();
   return result;
}

std::vector<transfer::request> transfer::discard(piece_id piece, transfer_time now) {
   validate_time(now);
   const auto index = ordinal(piece);
   auto result = std::vector<request>{};
   result.reserve(_requests.size());
   if (bitmap_test(_verified, index)) {
      // Re-admission can allocate. Do it before retracting verified state/time.
      if (_pieces[index].priority != 0U && _pieces[index].available != 0U)
         _order.insert(key(index));
      _verified[index / 8U] &= static_cast<std::uint8_t>(~(1U << (index % 8U)));
      --_verified_count;
      if (_pieces[index].priority != 0U)
         ++_wanted;
   }
   _time = now;
   for (auto it = _requests.begin(); it != _requests.end();) {
      if (it->second.piece != piece) {
         ++it;
         continue;
      }
      result.push_back(it->second);
      release(it++);
   }
   if (_partials.erase(index) != 0U)
      _unfinished_bytes -= _pieces[index].length;
   return result;
}

std::vector<transfer::request> transfer::stop(transfer_time now) {
   validate_time(now);
   auto result = std::vector<request>{};
   result.reserve(_requests.size());
   _time = now;
   for (auto it = _requests.begin(); it != _requests.end();) {
      result.push_back(it->second);
      release(it++);
   }
   prune_empty();
   _stopped = true;
   return result;
}

std::optional<transfer::verification> transfer::verification_for(piece_id piece) const {
   const auto found = _partials.find(ordinal(piece));
   if (_stopped || found == _partials.end() ||
       !std::all_of(found->second.blocks.begin(), found->second.blocks.end(),
                    [](const auto& block) { return block.received; }))
      return std::nullopt;
   return verification{piece, found->second.generation};
}

transfer::verification_result transfer::finish(verification token, bool valid) {
   const auto index = ordinal(token.piece);
   const auto found = _partials.find(index);
   if (_stopped || found == _partials.end() || found->second.generation != token.generation)
      return verification_result::ignored;
   if (!verification_for(token.piece)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "piece verification requires all retained blocks");
   }
   if (valid) {
      _order.erase(key(index));
      for (auto block = 0U; block < found->second.blocks.size(); ++block) {
         const auto source = _peers.find(found->second.blocks[block].source);
         if (source != _peers.end())
            add(source->second.observation.useful_bytes, block_length(index, block));
      }
      set_bit(_verified, index);
      ++_verified_count;
      if (_pieces[index].priority != 0U)
         --_wanted;
   } else {
      add(_integrity_failures);
      const auto source = found->second.blocks.front().source;
      const auto single = source != 0U && _peers.contains(source) &&
                          std::all_of(found->second.blocks.begin(), found->second.blocks.end(),
                                      [&](const auto& block) { return block.source == source; });
      if (single)
         add(peer(source).observation.integrity_failures);
      else
         add(_unattributed);
   }
   _unfinished_bytes -= _pieces[index].length;
   _partials.erase(found);
   return valid ? verification_result::verified : verification_result::retry;
}

bool transfer::verified(piece_id piece) const {
   return bitmap_test(_verified, ordinal(piece));
}

std::uint32_t transfer::availability(piece_id piece) const {
   return _pieces[ordinal(piece)].available;
}

peer_observation transfer::observations(peer_id id) const {
   return peer(id).observation;
}

transfer::status transfer::inspect() const noexcept {
   return {_peers.size(),
           _requests.size(),
           _request_bytes,
           _partials.size(),
           _unfinished_bytes,
           _duplicates,
           _verified_count,
           _integrity_failures,
           _unattributed,
           _stopped,
           _verified_count == _pieces.size()};
}

resume_state transfer::progress() const {
   auto result = resume_state{1U, _id, 0U, false, _verified, {}};
   result.unfinished.reserve(_partials.size());
   for (const auto& [index, partial] : _partials) {
      auto item = unfinished_piece{_pieces[index].id, std::vector<std::uint8_t>((partial.blocks.size() + 7U) / 8U)};
      for (auto block = 0U; block < partial.blocks.size(); ++block) {
         if (partial.blocks[block].received)
            set_bit(item.blocks, block);
      }
      result.unfinished.push_back(std::move(item));
   }
   return result;
}

} // namespace forge::swarm::core
