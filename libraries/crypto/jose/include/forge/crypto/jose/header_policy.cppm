module;

#include <cstddef>
#include <optional>
#include <string>

export module forge.crypto.jose.header_policy;

export import forge.crypto.jose.exceptions;

export namespace forge::crypto::jose {

struct limits {
   std::size_t compact_bytes = 16'384;
   std::size_t protected_header_bytes = 512;
   std::size_t payload_bytes = 8'192;

   bool operator==(const limits&) const = default;
};

enum class key_id_policy {
   required,
   absent,
};

struct header_policy {
   // nullopt requires an absent typ; it does not mean "accept any type".
   std::optional<std::string> typ;
   key_id_policy key_id = key_id_policy::required;
   limits bounds;

   void validate() const;
   bool operator==(const header_policy&) const = default;
};

} // namespace forge::crypto::jose
