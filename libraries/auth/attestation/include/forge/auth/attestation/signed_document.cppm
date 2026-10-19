module;

#include <string>
#include <utility>

export module forge.auth.attestation.signed_document;

export import forge.auth.attestation.verification_policy;

import forge.raw.codec;

export namespace forge::auth::attestation {

// This is an untrusted credential value, not evidence of successful verification.
class signed_document {
 public:
   signed_document() = default;
   explicit signed_document(std::string compact);

   [[nodiscard]] const std::string& compact() const noexcept;
   void validate() const;
   bool operator==(const signed_document&) const = default;

 private:
   std::string compact_;
};

[[nodiscard]] constexpr bool diagnostic_is_secret(const signed_document&) noexcept {
   return true;
}

template <typename Stream> void raw_pack(Stream& stream, const signed_document& value) {
   value.validate();
   forge::raw::pack(stream, value.compact());
}

template <typename Stream> void raw_unpack(Stream& stream, signed_document& value) {
   auto compact = std::string{};
   forge::raw::unpack(stream, compact);
   value = signed_document{std::move(compact)};
}

} // namespace forge::auth::attestation
