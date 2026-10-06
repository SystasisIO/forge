#include "details/engine_connector_impl.hxx"
#include "details/engine_connection_impl.hxx"
#include "details/engine_listener_impl.hxx"

namespace forge::net::quic::detail {
[[nodiscard]] bool engine_connector::impl::active_connect::mark_timed_out() noexcept {
   auto expected = state_value::pending;
   return state.compare_exchange_strong(expected, state_value::timed_out, std::memory_order_acq_rel);
}

[[nodiscard]] bool engine_connector::impl::active_connect::mark_canceled() noexcept {
   auto expected = state_value::pending;
   return state.compare_exchange_strong(expected, state_value::canceled, std::memory_order_acq_rel);
}

[[nodiscard]] bool engine_connector::impl::active_connect::finish() noexcept {
   auto expected = state_value::pending;
   return state.compare_exchange_strong(expected, state_value::completed, std::memory_order_acq_rel);
}

[[nodiscard]] bool engine_connector::impl::active_connect::timed_out() const noexcept {
   return state.load(std::memory_order_acquire) == state_value::timed_out;
}

[[nodiscard]] bool engine_connector::impl::active_connect::canceled() const noexcept {
   return state.load(std::memory_order_acquire) == state_value::canceled;
}

void engine_connector::impl::active_connect::complete_resolution(boost::system::error_code error,
                                                                 udp::resolver::results_type results) noexcept {
   {
      auto lock = std::scoped_lock{mutex};
      resolver.reset();
      resolution_error = error;
      try {
         resolution_results.emplace(std::move(results));
      } catch (...) {
         resolution_error = asio::error::no_memory;
         resolution_results.reset();
      }
      resolution_completed = true;
   }
   resolution_changed.notify();
}

[[nodiscard]] bool engine_connector::impl::active_connect::take_resolution(boost::system::error_code& error,
                                                                           udp::resolver::results_type& results) {
   auto lock = std::scoped_lock{mutex};
   if (!resolution_completed) {
      return false;
   }
   error = resolution_error;
   if (resolution_results) {
      results = std::move(*resolution_results);
      resolution_results.reset();
   }
   return true;
}

void engine_connector::impl::active_connect::release_resolver() noexcept {
   auto lock = std::scoped_lock{mutex};
   resolver.reset();
}

void engine_connector::impl::active_connect::cancel_resolver(const std::shared_ptr<udp::resolver>& value) noexcept {
   if (!value) {
      return;
   }
   try {
      asio::dispatch(value->get_executor(), [value] {
         try {
            value->cancel();
         } catch (...) {
            // A terminal result is still authoritative if resolver cancellation fails.
         }
      });
   } catch (...) {
      // A terminal result is still authoritative if executor teardown rejects dispatch.
   }
}

void engine_connector::impl::active_connect::cancel_socket(const std::shared_ptr<udp::socket>& value) noexcept {
   if (!value) {
      return;
   }
   try {
      asio::post(value->get_executor(), [value] {
         auto ignored = boost::system::error_code{};
         value->cancel(ignored);
         value->close(ignored);
      });
   } catch (...) {
      // A terminal result is still authoritative if executor teardown rejects the post.
   }
}

void engine_connector::impl::active_connect::cancel_io() noexcept {
   if (!mark_canceled()) {
      return;
   }
   auto resolver_value = std::shared_ptr<udp::resolver>{};
   auto socket_value = std::shared_ptr<udp::socket>{};
   auto connection_value = std::shared_ptr<engine_connection::impl>{};
   {
      auto lock = std::scoped_lock{mutex};
      resolver_value = resolver;
      socket_value = socket.lock();
      connection_value = connection.lock();
   }
   cancel_resolver(resolver_value);
   if (connection_value) {
      try {
         asio::post(connection_value->strand, [connection_value] {
            connection_value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
            connection_value->fail_all();
         });
      } catch (...) {
         // A terminal result is still authoritative if executor teardown rejects the post.
      }
   } else if (socket_value) {
      cancel_socket(socket_value);
   }
   resolution_changed.notify();
}

void engine_connector::impl::active_connect::timeout_io() noexcept {
   if (!mark_timed_out()) {
      return;
   }
   auto resolver_value = std::shared_ptr<udp::resolver>{};
   auto socket_value = std::shared_ptr<udp::socket>{};
   auto connection_value = std::shared_ptr<engine_connection::impl>{};
   {
      auto lock = std::scoped_lock{mutex};
      resolver_value = resolver;
      socket_value = socket.lock();
      connection_value = connection.lock();
   }
   cancel_resolver(resolver_value);
   if (connection_value) {
      try {
         asio::post(connection_value->strand, [connection_value] {
            connection_value->metrics.timeouts.fetch_add(1, std::memory_order_relaxed);
            connection_value->fail_all();
         });
      } catch (...) {
         // A terminal result is still authoritative if executor teardown rejects the post.
      }
   } else if (socket_value) {
      cancel_socket(socket_value);
   }
   resolution_changed.notify();
}

engine_connector::impl::impl(boost::asio::io_context& context_value) : context(context_value) {}

[[nodiscard]] bool engine_connector::impl::valid() const noexcept {
   return !canceled.load(std::memory_order_acquire);
}

[[nodiscard]] std::shared_ptr<engine_connector::impl::active_connect>
engine_connector::impl::track_connect(std::shared_ptr<udp::resolver> resolver) {
   auto connect = std::make_shared<active_connect>();
   {
      auto lock = std::scoped_lock{connect->mutex};
      connect->resolver = std::move(resolver);
   }
   auto lock = std::scoped_lock{mutex};
   if (!valid()) {
      throw_engine(engine_error_kind::canceled, "QUIC connector is canceled");
   }
   active.erase(std::remove_if(active.begin(), active.end(), [](const auto& value) { return value.expired(); }),
                active.end());
   active.push_back(connect);
   return connect;
}

void engine_connector::impl::cancel() {
   auto connections = std::vector<std::shared_ptr<active_connect>>{};
   {
      auto lock = std::scoped_lock{mutex};
      canceled.store(true, std::memory_order_release);
      connections.reserve(active.size());
      for (auto& value : active) {
         if (auto connect = value.lock()) {
            connections.push_back(std::move(connect));
         }
      }
   }
   for (auto& connect : connections) {
      connect->cancel_io();
   }
}
} // namespace forge::net::quic::detail
