module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.swarm.runtime.engine;

import forge.asio.gate;
import forge.asio.notification;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"
#include "details/call_scope.hxx"
#include "details/peer_session.hxx"
#include "details/engine_impl.hxx"

namespace forge::swarm::runtime {

engine::engine(forge::asio::task::scheduler& scheduler, forge::asio::compute::executor cpu,
   std::shared_ptr<admission> policy, std::shared_ptr<capabilities> providers)
   : engine{scheduler, cpu, std::move(policy), std::move(providers), options{}} {}
engine::engine(forge::asio::task::scheduler& scheduler, forge::asio::compute::executor cpu,
   std::shared_ptr<admission> policy, std::shared_ptr<capabilities> providers, options config)
   : _impl{std::make_shared<impl>(scheduler, cpu, std::move(policy), std::move(providers), config)} {}
engine::~engine() { cancel(); }
void engine::cancel() noexcept { _impl->cancel(); }
boost::asio::awaitable<void> engine::async_close() { return impl::close(_impl); }
boost::asio::awaitable<sharing> engine::async_share(core::descriptor value, std::shared_ptr<core::store> store, std::string realm) {
   return share_owned(_impl, std::move(value), std::move(store), std::move(realm));
}
boost::asio::awaitable<sharing> engine::share_owned(std::shared_ptr<impl> owner,
   core::descriptor value, std::shared_ptr<core::store> store, std::string realm) {
   co_return sharing{co_await impl::create(std::move(owner), std::move(value), std::move(store), std::move(realm), false, {}, std::nullopt)};
}
boost::asio::awaitable<runtime::download> engine::async_download(core::descriptor value, std::shared_ptr<core::store> store,
   std::string realm, std::vector<std::string> providers,
   std::optional<std::chrono::steady_clock::time_point> total_deadline) {
   return download_owned(_impl, std::move(value), std::move(store), std::move(realm), std::move(providers), total_deadline);
}
boost::asio::awaitable<runtime::download> engine::download_owned(std::shared_ptr<impl> owner,
   core::descriptor value, std::shared_ptr<core::store> store, std::string realm, std::vector<std::string> providers,
   std::optional<std::chrono::steady_clock::time_point> total_deadline) {
   co_return runtime::download{co_await impl::create(std::move(owner), std::move(value), std::move(store),
      std::move(realm), true, std::move(providers), total_deadline)};
}
boost::asio::awaitable<void> engine::metadata(protocol::metadata_request request,
   forge::api::auth::authenticated_caller caller, forge::api::core::stream_writer<protocol::metadata_chunk> output) {
   protocol::validate(request);
   return detail::peer_session::metadata(_impl->find(request.swarm, request.realm), caller, std::move(output));
}
boost::asio::awaitable<void> engine::exchange(protocol::exchange_request request,
   forge::api::auth::authenticated_caller caller,
   forge::api::core::duplex_stream<protocol::peer_message, protocol::peer_message> stream) {
   protocol::validate(request);
   return detail::peer_session::serve(_impl->find(request.swarm, request.realm), caller, std::move(stream));
}

} // namespace forge::swarm::runtime
