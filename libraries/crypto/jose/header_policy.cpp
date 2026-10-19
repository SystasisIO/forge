module;

#include <algorithm>
#include <forge/exceptions/macros.hpp>

module forge.crypto.jose.header_policy;

namespace forge::crypto::jose {

void header_policy::validate() const {
   const auto maximum = limits{};
   if (bounds.compact_bytes == 0 || bounds.compact_bytes > maximum.compact_bytes ||
       bounds.protected_header_bytes == 0 || bounds.protected_header_bytes > maximum.protected_header_bytes ||
       bounds.payload_bytes == 0 || bounds.payload_bytes > maximum.payload_bytes ||
       (key_id != key_id_policy::required && key_id != key_id_policy::absent)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_policy, "invalid JOSE policy bounds");
   }
   if (typ && (typ->empty() || typ->size() > bounds.protected_header_bytes ||
               !std::ranges::all_of(*typ, [](unsigned char value) { return value >= 0x21 && value <= 0x7e; }))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_policy, "invalid JOSE type restriction");
   }
}

} // namespace forge::crypto::jose
