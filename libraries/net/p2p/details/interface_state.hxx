#pragma once

#include <boost/asio/ip/address.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace forge::net::p2p::detail {

class interface_state {
 public:
   struct limits {
      std::size_t interfaces = 256;
      std::size_t addresses_per_interface = 64;
   };

   struct address {
      boost::asio::ip::address value;
      std::optional<std::uint8_t> prefix_length;
      std::uint32_t scope_id = 0;
      bool flags_known = false;
      bool tentative = false;
      bool duplicate = false;
      bool deprecated = false;
      bool detached = false;
      friend bool operator==(const address&, const address&) = default;
   };

   struct interface {
      std::uint32_t index = 0;
      std::uint64_t generation = 0;
      std::string name;
      bool up = false;
      bool running = false;
      bool multicast = false;
      bool loopback = false;
      bool point_to_point = false;
      std::vector<address> addresses;
      friend bool operator==(const interface&, const interface&) = default;
   };

   struct update {
      std::uint64_t revision = 0;
      bool continuity_lost = false;
      std::vector<interface> interfaces;
   };

   explicit interface_state(limits bounds);
   // Strong exception guarantee. Incoming generation fields are ignored.
   // Invalidation survives a remove/recreate even if the final snapshot is identical.
   [[nodiscard]] update reconcile(std::vector<interface> observed,
                                  std::span<const std::uint32_t> invalidated = {}, bool reset = false);
   [[nodiscard]] const update& current() const noexcept;
   [[nodiscard]] static std::optional<std::uint8_t> prefix(std::span<const std::uint8_t> mask);

 private:
   limits _bounds;
   update _current;
   std::uint64_t _next_generation = 1;
};

} // namespace forge::net::p2p::detail
