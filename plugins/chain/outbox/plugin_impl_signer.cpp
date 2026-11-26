module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

module forge.plugins.chain.outbox.plugin;

import forge.api.chain.outbox;
import forge.api.core.types;
import forge.asio.notification;
import forge.asio.task;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.info;
import forge.chain.transaction.types;
import forge.api.auth.authenticated_caller;
import forge.chain.api.transaction_signer;

import forge.net.http.base_url;

#include "details/plugin_impl.hxx"

namespace forge::plugins::chain::outbox {

void plugin::impl::resolve_signer() {
   using signer = forge::chain::api::transaction_signer;
   auto signing = apis->get<signer>(signer::ref()).shared();
   sign = [signing](forge::chain::transaction::unsigned_transaction transaction) {
      return signing->sign(std::move(transaction), {});
   };
}

} // namespace forge::plugins::chain::outbox
