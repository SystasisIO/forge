module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>
#include <vector>

export module forge.crypto.wallet.api;

export import forge.crypto.wallet.protocol;
export import forge.crypto.wallet.exceptions;
export import forge.api.auth.authenticated_caller;

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
import forge.net.http.types;

export namespace forge::crypto::wallet {

class api
    : public forge::api::core::contract<api, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<wallet_status> create(password_request request,
                                                        forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<wallet_status> open(wallet_request request,
                                                      forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<std::vector<wallet_status>> list(forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<wallet_status> status(wallet_request request,
                                                        forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<std::vector<key_info>>
   list_public_keys(wallet_request request, forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<wallet_status> unlock(password_request request,
                                                        forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<wallet_status> lock(wallet_request request,
                                                      forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<std::vector<wallet_status>>
   lock_all(forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<wallet_status> set_timeout(timeout_request request,
                                                             forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<key_info> create_key(key_request request,
                                                       forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<key_info> import_key(import_request request,
                                                       forge::api::auth::authenticated_caller caller) = 0;
   virtual boost::asio::awaitable<wallet_status> remove_key(key_request request,
                                                            forge::api::auth::authenticated_caller caller) = 0;
};

} // namespace forge::crypto::wallet

export namespace forge::api::core {

template <> struct method_descriptor_customization<::forge::crypto::wallet::api> {
   template <auto Method, bool EnableRaw>
   static void apply(method_builder<::forge::crypto::wallet::api, EnableRaw>& method) {
      static_cast<void>(Method);
      ::forge::crypto::wallet::exceptions::declare(method);
   }
};

} // namespace forge::api::core

FORGE_EXPORT_API(::forge::crypto::wallet::api, FORGE_API_CONTRACT("forge.crypto.wallet.api", 1, 0),
                 FORGE_API_METHOD(create, request, caller), FORGE_API_METHOD(open, request, caller),
                 FORGE_API_METHOD(list, caller), FORGE_API_METHOD(status, request, caller),
                 FORGE_API_METHOD(list_public_keys, request, caller), FORGE_API_METHOD(unlock, request, caller),
                 FORGE_API_METHOD(lock, request, caller), FORGE_API_METHOD(lock_all, caller),
                 FORGE_API_METHOD(set_timeout, request, caller), FORGE_API_METHOD(create_key, request, caller),
                 FORGE_API_METHOD(import_key, request, caller), FORGE_API_METHOD(remove_key, request, caller))

FORGE_HTTP_API(::forge::crypto::wallet::api,
               FORGE_HTTP_POST(create, "/v1/wallet/create", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(open, "/v1/wallet/open", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(list, "/v1/wallet/list", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(status, "/v1/wallet/status", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(list_public_keys, "/v1/wallet/list_public_keys", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(unlock, "/v1/wallet/unlock", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(lock, "/v1/wallet/lock", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(lock_all, "/v1/wallet/lock_all", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(set_timeout, "/v1/wallet/set_timeout", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(create_key, "/v1/wallet/create_key", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(import_key, "/v1/wallet/import_key", ok, FORGE_HTTP_CACHE(no_store)),
               FORGE_HTTP_POST(remove_key, "/v1/wallet/remove_key", ok, FORGE_HTTP_CACHE(no_store)))
