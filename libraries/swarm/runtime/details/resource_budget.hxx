#pragma once

namespace forge::swarm::runtime::detail {

// A single lock commits all three dimensions or none. Tickets follow payload
// ownership through API, verification and disk completion.
class resource_budget : public std::enable_shared_from_this<resource_budget> {
 public:
   class ticket;
   resource_budget(resource_limits, resource_limits, resource_limits);
   [[nodiscard]] ticket reserve(std::uint64_t operation, std::string peer, resource_limits);
   [[nodiscard]] std::optional<ticket> try_reserve(std::uint64_t operation, std::string peer, resource_limits);
   [[nodiscard]] bool available(std::uint64_t operation, const std::string& peer, resource_limits) const;
   forge::asio::notification changed;
 private:
   void validate_capacity(resource_limits) const;
   bool available_locked(std::uint64_t, const std::string&, resource_limits) const;
   void release(std::uint64_t, const std::string&, resource_limits) noexcept;
   static bool fits(resource_limits, resource_limits, resource_limits);
   static void add(resource_limits&, resource_limits);
   static void subtract(resource_limits&, resource_limits);
   mutable std::mutex _mutex;
   resource_limits _limits;
   resource_limits _operation_limits;
   resource_limits _peer_limits;
   resource_limits _used{0, 0, 0, 0};
   std::map<std::uint64_t, resource_limits> _operations;
   std::map<std::string, resource_limits> _peers;
};

class resource_budget::ticket {
 public:
   ticket();
   ~ticket();
   ticket(ticket&&) noexcept;
   ticket& operator=(ticket&&) noexcept;
   ticket(const ticket&) = delete;
   ticket& operator=(const ticket&) = delete;
   void release() noexcept;
   [[nodiscard]] ticket split(resource_limits);
   void rebind(std::string peer);
 private:
   ticket(std::shared_ptr<resource_budget>, std::uint64_t, std::string, resource_limits);
   std::shared_ptr<resource_budget> _owner;
   std::uint64_t _operation = 0;
   std::string _peer;
   resource_limits _amount{0, 0, 0, 0};
   friend class resource_budget;
};

} // namespace forge::swarm::runtime::detail
