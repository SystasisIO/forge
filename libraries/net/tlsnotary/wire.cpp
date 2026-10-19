module;

#include <cstdint>
#include <span>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.net.tlsnotary.engine;

#include "details/wire.hxx"

namespace forge::net::tlsnotary::detail {

void wire::put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned width) {
   for (auto n = width; n > 0; --n)
      out.push_back(static_cast<std::uint8_t>(value >> ((n - 1) * 8)));
}

void wire::blob(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
   put(out, bytes.size(), 4);
   out.insert(out.end(), bytes.begin(), bytes.end());
}

wire::wire(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}
std::uint64_t wire::take(unsigned width) {
   if (bytes_.size() < width)
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "truncated native frame");
   auto value = std::uint64_t{};
   for (unsigned n = 0; n < width; ++n)
      value = (value << 8) | bytes_[n];
   bytes_ = bytes_.subspan(width);
   return value;
}

std::span<const std::uint8_t> wire::blob(std::size_t limit) {
   const auto size = take(4);
   if (size > limit || size > bytes_.size())
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "invalid native field size");
   auto result = bytes_.first(size);
   bytes_ = bytes_.subspan(size);
   return result;
}

void wire::finish() const {
   if (!bytes_.empty())
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "unexpected native frame suffix");
}
} // namespace forge::net::tlsnotary::detail
