module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.crypto.wallet.plugin;
import forge.api.auth.authenticated_caller;
import forge.asio.task;
import forge.asio.compute;
import forge.crypto.wallet.exceptions;
import forge.plugins.crypto.wallet.provider_source;
import forge.plugins.net.http.server.api;
import forge.raw.raw;

#include "details/plugin_impl.hxx"
#include "details/provider_source_impl.hxx"

namespace forge::plugins::crypto::wallet {

plugin::provider_source_impl::provider_source_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

std::shared_ptr<forge::crypto::signer::provider> plugin::provider_source_impl::get(std::string name) {
   return state_->find(std::move(name), false);
}

} // namespace forge::plugins::crypto::wallet
