module;

#include <boost/describe.hpp>
#include <forge/raw/serialization.hpp>

#include <chrono>
#include <memory>

export module forge.api.auth.authenticated_subject;

export import forge.api.core.server_supplied;
export import forge.auth.oauth2.token_verifier;
import forge.crypto.digest.sha256;

import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.described;
import forge.variant.value;

export namespace forge::api::auth {

class authenticated_subject {
 public:
   authenticated_subject() = default;
   explicit authenticated_subject(forge::auth::oauth2::verified_token token);

   [[nodiscard]] const forge::auth::oauth2::verified_token* token() const noexcept;
   [[nodiscard]] bool authenticated() const noexcept;

 private:
   std::shared_ptr<const forge::auth::oauth2::verified_token> token_;
   bool authenticated_ = false;

   friend struct forge::api::core::server_supplied<authenticated_subject>;
};

// The wire value is an empty placeholder, never verified claims or evidence.
BOOST_DESCRIBE_STRUCT(authenticated_subject, (), ())

} // namespace forge::api::auth

export namespace forge::api::core {

template <> struct server_supplied<forge::api::auth::authenticated_subject> {
   static constexpr bool required = true;

   static void reset(forge::api::auth::authenticated_subject& value) {
      value = {};
   }

   [[nodiscard]] static bool apply(forge::api::auth::authenticated_subject& value,
                                   const trusted_invocation& trusted) {
      const auto* source = trusted.find<forge::api::auth::authenticated_subject>();
      if (source == nullptr || source->token() == nullptr ||
          !source->token()->valid_at(std::chrono::system_clock::now())) {
         return false;
      }
      value = *source;
      value.authenticated_ = true;
      return true;
   }
};

} // namespace forge::api::core

FORGE_DECLARE_SERIALIZATION(forge::api::auth::authenticated_subject)
