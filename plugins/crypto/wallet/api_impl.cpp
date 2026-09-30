module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module forge.plugins.crypto.wallet.plugin;

import forge.api.core.exceptions;
import forge.asio.task;
import forge.asio.compute;
import forge.crypto.asymmetric;
import forge.crypto.keystore.exceptions;
import forge.crypto.wallet.api;
import forge.plugins.net.http.server.api;
import forge.raw.raw;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::crypto::wallet {
namespace management = forge::crypto::wallet;
using forge::api::auth::authenticated_caller;

plugin::api_impl::api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::create(management::password_request request,
                                                                           authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::create);
   state_->check_size(request);
   if (request.password.empty() || request.password.size() > 4096) {
      throw management::exceptions::invalid_request{"Invalid password length"};
   }
   co_return co_await sanitized(state_->find(request.wallet, true)->create(std::move(request.password)));
}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::open(management::wallet_request request,
                                                                         authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::open);
   state_->check_size(request);
   co_return co_await sanitized(state_->find(request.wallet, true)->open());
}

boost::asio::awaitable<std::vector<management::wallet_status>> plugin::api_impl::list(authenticated_caller caller) {
   state_->authorize(caller, {}, management::operation::list);
   std::vector<management::wallet_status> result;
   for (const auto& wallet : state_->snapshot()) {
      if (state_->allowed(caller, wallet->name(), management::operation::list)) {
         result.push_back(co_await sanitized(wallet->status()));
      }
   }
   co_return result;
}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::status(management::wallet_request request,
                                                                           authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::status);
   state_->check_size(request);
   co_return co_await sanitized(state_->find(request.wallet, false)->status());
}

boost::asio::awaitable<std::vector<management::key_info>>
plugin::api_impl::list_public_keys(management::wallet_request request, authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::list_public_keys);
   state_->check_size(request);
   auto keys = co_await sanitized(state_->find(request.wallet, false)->keys());
   std::vector<management::key_info> result;
   for (const auto& key : keys) {
      result.push_back(
          {.id = key.id.value, .key = forge::crypto::asymmetric::encoding::forge().format(key.public_key)});
   }
   co_return result;
}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::unlock(management::password_request request,
                                                                           authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::unlock);
   state_->check_size(request);
   if (request.password.empty() || request.password.size() > 4096) {
      throw management::exceptions::invalid_request{"Invalid password length"};
   }
   co_return co_await sanitized(state_->find(request.wallet, false)->unlock(std::move(request.password)));
}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::lock(management::wallet_request request,
                                                                         authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::lock);
   state_->check_size(request);
   co_return co_await sanitized(state_->find(request.wallet, false)->lock());
}

boost::asio::awaitable<std::vector<management::wallet_status>> plugin::api_impl::lock_all(authenticated_caller caller) {
   state_->authorize(caller, {}, management::operation::lock_all);
   std::vector<management::wallet_status> result;
   for (const auto& wallet : state_->snapshot()) {
      if (state_->allowed(caller, wallet->name(), management::operation::lock_all)) {
         result.push_back(co_await sanitized(wallet->lock()));
      }
   }
   co_return result;
}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::set_timeout(management::timeout_request request,
                                                                                authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::set_timeout);
   state_->check_size(request);
   co_return co_await sanitized(state_->find(request.wallet, false)->set_timeout(request.seconds));
}

boost::asio::awaitable<management::key_info> plugin::api_impl::create_key(management::key_request request,
                                                                          authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::create_key);
   state_->check_size(request);
   co_return co_await sanitized(state_->find(request.wallet, false)->create_key(std::move(request.id)));
}

boost::asio::awaitable<management::key_info> plugin::api_impl::import_key(management::import_request request,
                                                                          authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::import_key);
   state_->check_size(request);
   if (request.private_key.empty() || request.private_key.size() > 16384) {
      throw management::exceptions::invalid_request{"Invalid private key length"};
   }
   co_return co_await sanitized(
       state_->find(request.wallet, false)->import_key(std::move(request.id), std::move(request.private_key)));
}

boost::asio::awaitable<management::wallet_status> plugin::api_impl::remove_key(management::key_request request,
                                                                               authenticated_caller caller) {
   state_->authorize(caller, request.wallet, management::operation::remove_key);
   state_->check_size(request);
   co_return co_await sanitized(state_->find(request.wallet, false)->remove_key(std::move(request.id)));
}

} // namespace forge::plugins::crypto::wallet
