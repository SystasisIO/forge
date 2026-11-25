module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
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
import forge.asio.notification;
import forge.asio.task;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.info;
import forge.chain.transaction.types;
import forge.plugins.db.store.api;

import forge.net.http.base_url;

#include "details/plugin_impl.hxx"

namespace forge::plugins::chain::outbox {

boost::asio::awaitable<void> plugin::impl::open(forge::api::chain::outbox::transport transport) {
   using database = forge::plugins::db::store::api;
   auto stores = apis->get<database>(database::ref()).shared();
   auto named = co_await stores->store(options.store);
   flush = [stores, name = options.store] { return stores->flush(name, true); };
   queue = std::make_shared<forge::api::chain::outbox>(named.objects(), std::move(transport), options.delivery);
}

} // namespace forge::plugins::chain::outbox
