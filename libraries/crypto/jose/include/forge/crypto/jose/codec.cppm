module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

export module forge.crypto.jose.codec;

export import forge.crypto.jose.trusted_keys;
export import forge.crypto.digest.sha256;

export namespace forge::crypto::jose {

class prepared_message;
[[nodiscard]] prepared_message prepare(std::string_view payload, const trusted_key& key, const header_policy& policy);

class prepared_message {
 public:
   prepared_message(prepared_message&&) noexcept;
   prepared_message& operator=(prepared_message&&) noexcept;
   prepared_message(const prepared_message&) = delete;
   prepared_message& operator=(const prepared_message&) = delete;
   ~prepared_message();

   [[nodiscard]] std::string_view signing_input() const&;
   std::string_view signing_input() const&& = delete;
   [[nodiscard]] digest::sha256 signing_digest() const;
   [[nodiscard]] const verified_key_binding& key_binding() const&;
   const verified_key_binding& key_binding() const&& = delete;
   [[nodiscard]] std::string complete(std::span<const std::uint8_t> signature);

 private:
   struct impl;
   std::unique_ptr<impl> _impl;
   explicit prepared_message(std::unique_ptr<impl> state) noexcept;
   friend prepared_message prepare(std::string_view, const trusted_key&, const header_policy&);
};

class verifier;

class verified_payload {
 public:
   verified_payload(const verified_payload&) = default;
   verified_payload& operator=(const verified_payload&) = default;

   [[nodiscard]] std::string_view payload() const& noexcept;
   std::string_view payload() const&& = delete;
   [[nodiscard]] const verified_key_binding& key_binding() const&;
   const verified_key_binding& key_binding() const&& = delete;
   [[nodiscard]] const trusted_keys& trust_snapshot() const& noexcept;
   const trusted_keys& trust_snapshot() const&& = delete;

 private:
   verified_payload(std::string payload, const trusted_keys& snapshot, std::size_t key_index);
   std::string _payload;
   trusted_keys _snapshot;
   std::size_t _key_index;
   friend class verifier;
};

class verifier {
 public:
   [[nodiscard]] verified_payload verify(std::string_view compact, const trusted_keys& snapshot) const;
};

[[nodiscard]] constexpr bool diagnostic_is_secret(const prepared_message&) noexcept { return true; }
[[nodiscard]] constexpr bool diagnostic_is_secret(const verified_payload&) noexcept { return true; }

} // namespace forge::crypto::jose
