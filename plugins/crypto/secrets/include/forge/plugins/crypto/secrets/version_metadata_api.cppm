module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.plugins.crypto.secrets.version_metadata_api;

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

class version_metadata_api : public forge::api::core::contract<version_metadata_api, forge::api::core::surface::local> {
 public:
   virtual ~version_metadata_api() = default;
   virtual boost::asio::awaitable<version_metadata> describe_version(version_query value) = 0;
   virtual boost::asio::awaitable<version_page> list_versions(version_list_request value) = 0;
   virtual boost::asio::awaitable<version_snapshot> status(query value) = 0;
};

} // namespace forge::plugins::crypto::secrets

FORGE_EXPORT_API(::forge::plugins::crypto::secrets::version_metadata_api,
                 FORGE_API_CONTRACT("forge.plugins.crypto.secrets.version_metadata", 1, 0),
                 FORGE_API_METHOD_TYPED(describe_version, ::forge::plugins::crypto::secrets::version_query,
                                        ::forge::plugins::crypto::secrets::version_metadata),
                 FORGE_API_METHOD_TYPED(list_versions, ::forge::plugins::crypto::secrets::version_list_request,
                                        ::forge::plugins::crypto::secrets::version_page),
                 FORGE_API_METHOD_TYPED(status, ::forge::plugins::crypto::secrets::query,
                                        ::forge::plugins::crypto::secrets::version_snapshot))
