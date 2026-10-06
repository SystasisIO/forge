module;

#include <forge/exceptions/macros.hpp>
#include "details/socket_reuse.hxx"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.tcp.listener;

import forge.asio.notification;

#include "details/connector_access.hxx"

#include "details/listener_impl.hxx"

namespace forge::net::tcp {

listener::listener() = default;
listener::listener(boost::asio::any_io_executor executor, transport::endpoint local,
                   transport::listen_options listen_options, options tcp_options)
    : impl_(std::make_shared<impl>(std::move(executor), std::move(local), listen_options, tcp_options)) {}
listener::~listener() = default;
listener::listener(listener&&) noexcept = default;
listener& listener::operator=(listener&&) noexcept = default;

bool listener::valid() const noexcept {
   return impl_ && impl_->valid();
}

transport::endpoint listener::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   return impl_->local_endpoint();
}

connector listener::make_coordinated_connector(transport::endpoint local) const {
   return make_connector(std::move(local), connector::reuse_policy::required);
}

connector listener::make_connector(transport::endpoint local, connector::reuse_policy policy) const {
   if (policy != connector::reuse_policy::preferred && policy != connector::reuse_policy::required) {
      impl::throw_invalid_options("invalid tcp source reuse policy");
   }
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated tcp source listener is closed");
   }
   if (!impl_->tcp_options.reuse_port) {
      impl::throw_invalid_options("coordinated tcp source listener was not opened with reuse_port");
   }
   const auto requested = impl::to_bind_endpoint(local);
   const auto bound = impl::to_bind_endpoint(impl_->local);
   if (!detail::is_assigned_local_address(requested.address()) || requested.port() != bound.port() ||
       requested.protocol() != bound.protocol() ||
       (!bound.address().is_unspecified() && requested.address() != bound.address())) {
      impl::throw_invalid_endpoint(local, "coordinated tcp local endpoint does not belong to listener");
   }
   auto lock = std::scoped_lock{impl_->reuse_mutex};
   if (!impl_->valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated tcp source listener closed during admission");
   }
   std::erase_if(impl_->connector_drains, [](const auto& signal) { return signal.expired(); });
   if (impl_->connector_drains.size() >= impl_->tcp_options.max_pending_connects) {
      impl::throw_invalid_options("coordinated tcp connector owner limit exceeded");
   }
   auto drained = std::make_shared<forge::asio::notification>();
   auto result = detail::connector_access::make(
       impl_->strand, impl_->tcp_options, std::move(local), policy,
       [source = std::weak_ptr<impl>{impl_}] {
          const auto state = source.lock();
          return state && state->valid();
       },
       impl_->reuse_closed, drained, impl_);
   impl_->connector_drains.push_back(std::move(drained));
   return result;
}

boost::asio::awaitable<connection> listener::async_accept_connection(std::shared_ptr<void> lifetime) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   auto state = impl_;
   co_return co_await state->async_accept_connection(std::move(lifetime));
}

boost::asio::awaitable<transport::stream_connection> listener::async_accept() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   auto state = impl_;
   co_return co_await state->async_accept();
}

boost::asio::awaitable<void> listener::async_close() {
   if (!impl_) {
      co_return;
   }
   auto state = impl_;
   co_await state->async_close();
}

void listener::close() {
   if (impl_) {
      impl_->close();
   }
}

void listener::cancel() {
   if (impl_) {
      impl_->cancel();
   }
}

transport::stream_listener listener::as_transport() const {
   if (!valid()) {
      return {};
   }
   return transport::detail::stream_listener_access::make(impl_);
}

} // namespace forge::net::tcp
