module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.plugins.crypto.secrets.version_write_api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.plugins.crypto.secrets.version_types;

export namespace forge::plugins::crypto::secrets {

class version_write_api : public forge::api::core::contract<version_write_api, forge::api::core::surface::local> {
 public:
   virtual ~version_write_api() = default;
   virtual boost::asio::awaitable<version_mutation_result> create_version(create_version_request value) = 0;
   virtual boost::asio::awaitable<version_mutation_result> revoke_version(revoke_version_request value) = 0;
};

} // namespace forge::plugins::crypto::secrets

FORGE_EXPORT_API(::forge::plugins::crypto::secrets::version_write_api,
                 FORGE_API_CONTRACT("forge.plugins.crypto.secrets.version_write", 1, 0),
                 FORGE_API_METHOD_TYPED(create_version, ::forge::plugins::crypto::secrets::create_version_request,
                                        ::forge::plugins::crypto::secrets::version_mutation_result),
                 FORGE_API_METHOD_TYPED(revoke_version, ::forge::plugins::crypto::secrets::revoke_version_request,
                                        ::forge::plugins::crypto::secrets::version_mutation_result))
