module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <vector>

export module forge.chain.api.block_signer;

export import forge.api.auth.authenticated_caller;
export import forge.chain.api.exceptions;
export import forge.chain.protocol.block_signing;

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.dispatcher;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.http.binding;
import forge.api.http.client_request;
import forge.api.http.mapping;
import forge.api.http.openapi;
import forge.api.http.proxy;
import forge.chain.api.json_schema;
import forge.crypto.asymmetric;
import forge.crypto.digest.sha256;
import forge.net.http.types;
import forge.raw.datastream;
import forge.raw.raw;
import forge.raw.varint;
import forge.variant.described;
import forge.variant.value;
import forge.variant.variant_dynamic_bitset;

export namespace forge::chain::api {

class block_signer
    : public forge::api::core::contract<block_signer,
                                        forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~block_signer() = default;

   virtual boost::asio::awaitable<std::vector<chain::protocol::signature>>
   sign(chain::protocol::block_sign_request request, forge::api::auth::authenticated_caller caller) = 0;
};

} // namespace forge::chain::api

export namespace forge::api::core {

template <> struct method_descriptor_customization<::forge::chain::api::block_signer> {
   template <auto Method, bool EnableRaw>
   static void apply(method_builder<::forge::chain::api::block_signer, EnableRaw>& method) {
      static_cast<void>(Method);
      ::forge::chain::api::exceptions::descriptor::declare_signing(method);
   }
};

} // namespace forge::api::core

FORGE_EXPORT_API(::forge::chain::api::block_signer, FORGE_API_CONTRACT("forge.chain.api.block_signer", 1, 0),
                 FORGE_API_METHOD(sign, request, caller))

FORGE_HTTP_API(::forge::chain::api::block_signer,
               FORGE_HTTP_POST(sign, "/v1/signer/sign_block", ok, FORGE_HTTP_CACHE(no_store)))
