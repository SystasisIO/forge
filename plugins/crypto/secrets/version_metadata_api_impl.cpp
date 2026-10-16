module;

#include <boost/asio/awaitable.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.compute;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.keystore.ownership;
import forge.crypto.keystore.types;
import forge.plugins.crypto.secrets.version_metadata_api;
import forge.plugins.crypto.secrets.version_types;

#include "details/plugin_impl.hxx"
#include "details/version_store.hxx"
#include "details/version_metadata_api_impl.hxx"

namespace forge::plugins::crypto::secrets {

plugin::version_metadata_api_impl::version_metadata_api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<version_metadata> plugin::version_metadata_api_impl::describe_version(version_query value) {
   return state_->versions_store()->describe(std::move(value));
}

boost::asio::awaitable<version_page> plugin::version_metadata_api_impl::list_versions(version_list_request value) {
   return state_->versions_store()->list(std::move(value));
}

boost::asio::awaitable<version_snapshot> plugin::version_metadata_api_impl::status(query value) {
   return state_->versions_store()->status_async();
}

} // namespace forge::plugins::crypto::secrets
