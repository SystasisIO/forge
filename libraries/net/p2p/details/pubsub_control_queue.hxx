#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forge::net::p2p::detail {

// The node mutex serializes this pool. It owns no clock, task, mesh or transport.
class pubsub_control_queue {
 public:
   enum class kind { graft, prune };
   struct command {
      peer_id peer;
      std::uint64_t generation = 0;
      kind operation = kind::graft;
      pubsub::control::prune args;
   };
   struct intent {
      peer_id peer;
      std::uint64_t generation = 0;
      std::uint64_t revision = 0;
      kind operation = kind::graft;
      pubsub::control::prune args;
      std::size_t wire_bytes = 0;
      std::size_t control_bytes = 0;
      std::size_t charged_bytes = 0;
   };
   struct prepared {
      std::map<peer_id, std::map<std::string, std::shared_ptr<const intent>>> rows;
   };
   struct batch {
      peer_id peer;
      std::uint64_t generation = 0;
      std::vector<std::shared_ptr<const intent>> items;
      std::size_t ephemeral_bytes = 0;
   };

   explicit pubsub_control_queue(const pubsub::limits& limits);
   [[nodiscard]] static std::size_t capacity_for(const pubsub::limits& limits) noexcept;
   [[nodiscard]] std::optional<prepared> prepare(std::vector<command> commands);
   void commit(prepared value) noexcept;
   [[nodiscard]] std::vector<peer_id> peers() const;
   [[nodiscard]] std::shared_ptr<const batch> acquire(const peer_id& peer, std::uint64_t generation = 0,
                                                     std::size_t ephemeral_bytes = 0);
   [[nodiscard]] static pubsub::rpc rpc(const batch& value);
   [[nodiscard]] bool current(const intent& value) const noexcept;
   [[nodiscard]] bool current(const batch& value) const noexcept;
   void discard(const intent& value) noexcept;
   void discard(const peer_id& peer, const std::string& topic, kind operation) noexcept;
   void acknowledge(const std::shared_ptr<const batch>& value) noexcept;
   void finish(const std::shared_ptr<const batch>& value, bool written) noexcept;
   void forget(const peer_id& peer) noexcept;
   void clear() noexcept;
   [[nodiscard]] std::size_t size() const noexcept;
   [[nodiscard]] std::size_t bytes() const noexcept;

 private:
   [[nodiscard]] bool leased(const intent& value) const noexcept;
   void release(const intent& value) noexcept;

   pubsub::limits _limits;
   std::size_t _capacity;
   std::uint64_t _revision = 0;
   std::size_t _size = 0;
   std::size_t _bytes = 0;
   std::map<peer_id, std::map<std::string, std::shared_ptr<const intent>>> _rows;
   std::map<peer_id, std::shared_ptr<const batch>> _inflight;
};

} // namespace forge::net::p2p::detail
