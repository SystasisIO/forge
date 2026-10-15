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
import forge.plugins.crypto.secrets.version_write_api;
import forge.plugins.crypto.secrets.version_types;

#include "details/plugin_impl.hxx"
#include "details/version_store.hxx"
#include "details/version_write_api_impl.hxx"

namespace forge::plugins::crypto::secrets {

plugin::version_write_api_impl::version_write_api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<version_mutation_result>
plugin::version_write_api_impl::create_version(create_version_request value) {
   return state_->versions_store()->create(std::move(value));
}

boost::asio::awaitable<version_mutation_result>
plugin::version_write_api_impl::revoke_version(revoke_version_request value) {
   return state_->versions_store()->revoke(std::move(value));
}

} // namespace forge::plugins::crypto::secrets
