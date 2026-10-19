module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.crypto.jose.trusted_keys;

export import forge.crypto.jose.header_policy;
export import forge.crypto.asymmetric.p256;
export import forge.crypto.asymmetric.values;

export namespace forge::crypto::jose {

struct trusted_key {
   std::string kid;
   asymmetric::p256::public_key_point_data public_key{};
};

struct verified_key_binding {
   std::string kid;
   asymmetric::r1_public_key public_key;
   std::string identity;
};

struct snapshot_descriptor {
   std::string trust_domain;
   std::string verifier_profile;
   std::uint64_t revision = 0;
};

[[nodiscard]] verified_key_binding identify_key(const trusted_key& key);

class trusted_keys {
 public:
   trusted_keys(snapshot_descriptor descriptor, std::vector<trusted_key> keys, header_policy policy);
   trusted_keys(const trusted_keys&) noexcept = default;
   trusted_keys& operator=(const trusted_keys&) noexcept = default;
   ~trusted_keys();

   [[nodiscard]] const snapshot_descriptor& descriptor() const& noexcept;
   const snapshot_descriptor& descriptor() const&& = delete;
   [[nodiscard]] const header_policy& policy() const& noexcept;
   const header_policy& policy() const&& = delete;
   [[nodiscard]] const std::string& identity() const& noexcept;
   const std::string& identity() const&& = delete;
   [[nodiscard]] std::span<const trusted_key> keys() const& noexcept;
   std::span<const trusted_key> keys() const&& = delete;
   [[nodiscard]] const verified_key_binding& key_binding(std::size_t index) const&;
   const verified_key_binding& key_binding(std::size_t) const&& = delete;
   [[nodiscard]] std::optional<std::size_t> find(std::string_view kid) const noexcept;

 private:
   struct impl;
   std::shared_ptr<const impl> _impl;
};

} // namespace forge::crypto::jose
