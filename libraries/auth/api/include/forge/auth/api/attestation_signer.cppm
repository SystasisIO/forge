module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.auth.api.attestation_signer;

export import forge.api.auth.authenticated_caller;
export import forge.auth.api.exceptions;
export import forge.auth.attestation.document_schema;

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.dispatcher;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.variant.value;

export namespace forge::auth::api {

class attestation_signer
    : public forge::api::core::contract<attestation_signer,
                                        forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~attestation_signer() = default;

   virtual boost::asio::awaitable<forge::auth::attestation::signed_document>
   sign(forge::auth::attestation::document<forge::variant_object> request,
        forge::api::auth::authenticated_caller caller) = 0;
};

} // namespace forge::auth::api

export namespace forge::api::core {

template <> struct method_descriptor_customization<::forge::auth::api::attestation_signer> {
   template <auto Method, bool EnableRaw>
   static void apply(method_builder<::forge::auth::api::attestation_signer, EnableRaw>& method) {
      static_cast<void>(Method);
      ::forge::auth::api::exceptions::descriptor::declare_signing(method);
   }
};

} // namespace forge::api::core

FORGE_EXPORT_API(::forge::auth::api::attestation_signer,
                 FORGE_API_CONTRACT("forge.auth.api.attestation_signer", 1, 0),
                 FORGE_API_METHOD(sign, request, caller))
