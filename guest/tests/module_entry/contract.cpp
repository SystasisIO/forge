module;

#include <cstdint>

module probe.contract;

namespace probe {

std::uint64_t contract::ping(std::uint64_t value)
{
   return value;
}

std::uint64_t contract::read(std::uint64_t value) const
{
   return value;
}

} // namespace probe
