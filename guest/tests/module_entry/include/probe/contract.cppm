module;

#include <cstdint>

export module probe.contract;

import forge.contract;

export namespace probe {

class [[forge::contract("moduleprobe")]] contract final : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] std::uint64_t ping(std::uint64_t value);
   [[forge::call]] std::uint64_t read(std::uint64_t value) const;
};

} // namespace probe
