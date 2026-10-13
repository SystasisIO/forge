module;

#include <forge/raw/serialization.hpp>

#include <memory>
#include <new>
#include <utility>

module forge.api.auth.authenticated_subject;

import forge.raw.datastream;
import forge.raw.raw;

namespace forge::api::auth {

authenticated_subject::authenticated_subject(forge::auth::oauth2::verified_token token)
    : token_{std::make_shared<const forge::auth::oauth2::verified_token>(std::move(token))} {}

const forge::auth::oauth2::verified_token* authenticated_subject::token() const noexcept {
   return token_.get();
}

bool authenticated_subject::authenticated() const noexcept {
   return authenticated_;
}

} // namespace forge::api::auth

FORGE_IMPLEMENT_SERIALIZATION(forge::api::auth::authenticated_subject)
