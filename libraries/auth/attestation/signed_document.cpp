module;

#include <forge/exceptions/policy.hpp>

#include <string>
#include <utility>

module forge.auth.attestation.signed_document;

namespace forge::auth::attestation {

signed_document::signed_document(std::string compact) : compact_{std::move(compact)} {
   validate();
}

void signed_document::validate() const {
   if (compact_.empty() || compact_.size() > max_compact_bytes) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "attestation Compact size is outside profile limits");
   }
   for (const auto ch : compact_) {
      if (ch <= ' ' || static_cast<unsigned char>(ch) >= 127U) {
         FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_document, "attestation Compact must contain printable ASCII");
      }
   }
}

const std::string& signed_document::compact() const noexcept {
   return compact_;
}

} // namespace forge::auth::attestation
