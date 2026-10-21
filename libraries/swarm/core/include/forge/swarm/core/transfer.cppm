module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <vector>

export module forge.swarm.core.transfer;

export import forge.swarm.core.checkpoint;
export import forge.swarm.core.peer_observation;

export namespace forge::swarm::core {

// Single-owner, deterministic state. No payload bytes, callbacks, clock or I/O.
class transfer {
 public:
   enum class selection { rarest_first, sequential };
   enum class verification_result { ignored, verified, retry };

   struct config {
      std::uint32_t peers = 128;
      std::uint32_t requests = 256;
      std::uint32_t requests_per_peer = 16;
      std::uint64_t request_bytes = 4U * 1024U * 1024U;
      std::uint64_t request_bytes_per_peer = 256U * 1024U;
      std::uint32_t unfinished_pieces = 32;
      std::uint64_t unfinished_bytes = 64U * 1024U * 1024U;
      std::chrono::milliseconds request_timeout{30000};
      std::uint32_t endgame_blocks = 16;
      std::uint32_t endgame_replicas = 2;
      std::uint32_t endgame_requests = 8;
   };

   struct request {
      std::uint64_t id = 0;
      peer_id peer = 0;
      piece_id piece;
      std::uint32_t offset = 0;
      std::uint32_t length = 0;
      transfer_time started;
      transfer_time deadline;
      bool operator==(const request&) const = default;
   };

   struct verification {
      piece_id piece;
      std::uint64_t generation = 0;
      bool operator==(const verification&) const = default;
   };

   struct receive_result {
      bool accepted = false;
      std::optional<verification> ready;
      // Provisional block winner cancels siblings; it is not verified availability.
      std::vector<request> canceled;
   };

   struct status {
      std::size_t peers = 0;
      std::size_t requests = 0;
      std::uint64_t request_bytes = 0;
      std::size_t unfinished_pieces = 0;
      // Full piece reservations, retained through the verification stage.
      std::uint64_t unfinished_bytes = 0;
      std::uint32_t duplicate_requests = 0;
      std::uint32_t verified_pieces = 0;
      std::uint64_t integrity_failures = 0;
      std::uint64_t unattributed_integrity_failures = 0;
      bool stopped = false;
      bool complete = false;
   };

   // Resume is trusted local state after storage revalidation, never peer input.
   transfer(descriptor value, config limits, std::uint64_t tie_seed, std::optional<resume_state> local = std::nullopt);
   transfer(const transfer&) = delete;
   transfer& operator=(const transfer&) = delete;

   [[nodiscard]] peer_id add_peer(std::span<const std::uint8_t> availability);
   void set_availability(peer_id, std::span<const std::uint8_t> availability);
   [[nodiscard]] std::vector<request> remove_peer(peer_id, transfer_time now);
   // New connections start choked. Choking cancels that peer's active requests.
   [[nodiscard]] std::vector<request> set_choked(peer_id, bool choked, transfer_time now);
   void set_selection(selection);
   // Zero filters future picks; changing priority does not cancel owned requests.
   void set_priority(piece_id, std::uint8_t priority);

   [[nodiscard]] std::vector<request> pick(peer_id, transfer_time now, std::uint32_t count);
   // nullopt preserves unrestricted selection; an empty span permits no pieces.
   // Admission filters eligibility only, never inventory, priority or global endgame.
   [[nodiscard]] std::vector<request> pick(peer_id, transfer_time now, std::uint32_t count,
                                          std::optional<std::span<const piece_id>> allowed);
   // Caller reports only payload retained by its writer; exact owned length required.
   [[nodiscard]] receive_result receive(peer_id, std::uint64_t id, std::uint32_t length, transfer_time now);
   [[nodiscard]] bool cancel(peer_id, std::uint64_t id, transfer_time now);
   [[nodiscard]] std::vector<request> expire(transfer_time now);
   [[nodiscard]] std::vector<request> discard(piece_id, transfer_time now);
   [[nodiscard]] std::vector<request> stop(transfer_time now);
   [[nodiscard]] std::optional<verification> verification_for(piece_id) const;
   [[nodiscard]] verification_result finish(verification, bool verified);

   [[nodiscard]] bool verified(piece_id) const;
   [[nodiscard]] std::uint32_t availability(piece_id) const;
   [[nodiscard]] peer_observation observations(peer_id) const;
   [[nodiscard]] status inspect() const noexcept;
   // Snapshot only: caller owns durability, checkpoint generation and publication.
   [[nodiscard]] resume_state progress() const;

 private:
   struct piece_state {
      piece_id id;
      std::uint32_t length = 0;
      std::uint32_t available = 0;
      std::uint8_t priority = 1;
      std::uint64_t seed_rank = 0;
   };
   struct block_state {
      peer_id source = 0;
      std::uint32_t requests = 0;
      bool received = false;
   };
   struct partial_state {
      std::uint64_t generation = 0;
      std::vector<block_state> blocks;
   };
   struct peer_state {
      std::vector<std::uint8_t> availability;
      peer_observation observation;
      std::uint32_t requests = 0;
      std::uint64_t bytes = 0;
      bool choked = true;
   };
   using order_key = std::tuple<std::uint8_t, std::uint32_t, std::uint64_t, std::uint32_t>;

   void validate_time(transfer_time) const;
   [[nodiscard]] peer_state& peer(peer_id);
   [[nodiscard]] const peer_state& peer(peer_id) const;
   void validate_bitmap(std::span<const std::uint8_t>) const;
   [[nodiscard]] order_key key(std::uint32_t index) const noexcept;
   [[nodiscard]] order_key key(std::uint32_t index, std::uint32_t available, std::uint8_t priority,
                               selection) const noexcept;
   void change_availability(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after);
   [[nodiscard]] std::uint32_t ordinal(piece_id) const;
   [[nodiscard]] std::uint32_t block_length(std::uint32_t ordinal, std::uint32_t block) const;
   [[nodiscard]] bool endgame() const;
   [[nodiscard]] request issue(peer_id, std::uint32_t ordinal, std::uint32_t block, transfer_time,
                               transfer_time deadline);
   void release(std::map<std::uint64_t, request>::iterator);
   void prune_empty();

   descriptor _descriptor;
   swarm_id _id;
   config _limits;
   std::uint64_t _seed;
   selection _selection = selection::rarest_first;
   std::vector<piece_state> _pieces;
   std::vector<std::uint8_t> _verified;
   // Only unverified, nonfiltered pieces with at least one available peer.
   std::set<order_key> _order;
   std::map<peer_id, peer_state> _peers;
   std::map<std::uint32_t, partial_state> _partials;
   std::map<std::uint64_t, request> _requests;
   std::uint64_t _next_peer = 1;
   std::uint64_t _next_request = 1;
   std::uint64_t _next_generation = 1;
   std::uint64_t _request_bytes = 0;
   std::uint64_t _unfinished_bytes = 0;
   std::uint32_t _duplicates = 0;
   std::uint32_t _verified_count = 0;
   std::uint32_t _wanted = 0;
   std::uint64_t _integrity_failures = 0;
   std::uint64_t _unattributed = 0;
   transfer_time _time{};
   bool _stopped = false;
};

} // namespace forge::swarm::core
