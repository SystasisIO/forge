#include "details/interface_state.hxx"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace forge::net::p2p::detail {

interface_state::interface_state(limits bounds) : _bounds(bounds) {
   if (bounds.interfaces == 0 || bounds.interfaces > 4096 ||
       bounds.addresses_per_interface == 0 || bounds.addresses_per_interface > 1024) {
      throw std::invalid_argument{"invalid interface state limits"};
   }
}

interface_state::update interface_state::reconcile(std::vector<interface> observed,
                                                  std::span<const std::uint32_t> invalidated, bool reset) {
   if (observed.size() > _bounds.interfaces || invalidated.size() > _bounds.interfaces) {
      throw std::length_error{"interface state limit exceeded"};
   }
   for (auto& item : observed) {
      if (item.index == 0 || item.name.empty() || item.name.size() > 255 ||
          item.name.find('\0') != std::string::npos) {
         throw std::invalid_argument{"invalid interface identity"};
      }
      if (item.addresses.size() > _bounds.addresses_per_interface) {
         throw std::length_error{"interface address limit exceeded"};
      }
      item.generation = 0;
      for (auto& address : item.addresses) {
         const auto bits = address.value.is_v4() ? 32 : 128;
         if (address.prefix_length && *address.prefix_length > bits) {
            throw std::invalid_argument{"invalid interface prefix"};
         }
         if (address.value.is_v4()) {
            if (address.scope_id != 0) {
               throw std::invalid_argument{"IPv4 interface address has scope"};
            }
         } else {
            auto v6 = address.value.to_v6();
            if ((v6.scope_id() != 0 && v6.scope_id() != item.index) ||
                (address.scope_id != 0 && address.scope_id != item.index)) {
               throw std::invalid_argument{"interface address scope mismatch"};
            }
            if (v6.is_link_local()) {
               address.scope_id = item.index;
            }
            v6.scope_id(0);
            address.value = v6;
         }
      }
      std::sort(item.addresses.begin(), item.addresses.end(), [](const auto& a, const auto& b) {
         return std::tie(a.value, a.scope_id, a.prefix_length, a.flags_known, a.tentative,
                         a.duplicate, a.deprecated, a.detached) <
                std::tie(b.value, b.scope_id, b.prefix_length, b.flags_known, b.tentative,
                         b.duplicate, b.deprecated, b.detached);
      });
      item.addresses.erase(std::unique(item.addresses.begin(), item.addresses.end()), item.addresses.end());
   }
   std::sort(observed.begin(), observed.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
   auto next = _next_generation;
   for (std::size_t i = 0; i < observed.size(); ++i) {
      auto& item = observed[i];
      if (i != 0 && observed[i - 1].index == item.index) {
         throw std::invalid_argument{"duplicate interface index"};
      }
      const auto old = std::find_if(_current.interfaces.begin(), _current.interfaces.end(),
                                    [&](const auto& value) { return value.index == item.index; });
      if (!reset && old != _current.interfaces.end() &&
          std::find(invalidated.begin(), invalidated.end(), item.index) == invalidated.end()) {
         item.generation = old->generation;
         if (item == *old) {
            continue;
         }
      }
      if (next == std::numeric_limits<std::uint64_t>::max()) {
         throw std::overflow_error{"interface generation exhausted"};
      }
      item.generation = next++;
   }
   if (_current.revision == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error{"interface revision exhausted"};
   }
   auto result = update{_current.revision + 1, reset, std::move(observed)};
   auto committed = result;
   _current = std::move(committed);
   _next_generation = next;
   return result;
}

const interface_state::update& interface_state::current() const noexcept {
   return _current;
}

std::optional<std::uint8_t> interface_state::prefix(std::span<const std::uint8_t> mask) {
   if (mask.size() != 4 && mask.size() != 16) {
      return std::nullopt;
   }
   auto zeros = false;
   auto result = std::uint8_t{0};
   for (const auto byte : mask) {
      for (unsigned bit = 0x80; bit != 0; bit >>= 1) {
         if ((byte & bit) == 0) {
            zeros = true;
         } else if (zeros) {
            return std::nullopt;
         } else {
            ++result;
         }
      }
   }
   return result;
}

} // namespace forge::net::p2p::detail
