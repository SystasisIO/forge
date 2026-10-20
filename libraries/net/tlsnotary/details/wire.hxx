#pragma once

namespace forge::net::tlsnotary::detail {

class wire {
 public:
   static void put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned width);
   static void blob(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes);
   explicit wire(std::span<const std::uint8_t> bytes);
   std::uint64_t take(unsigned width);
   std::span<const std::uint8_t> blob(std::size_t limit);
   void finish() const;

 private:
   std::span<const std::uint8_t> bytes_;
};
} // namespace forge::net::tlsnotary::detail
