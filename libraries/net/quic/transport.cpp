module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.quic.transport;

import forge.asio.runtime;
import forge.net.quic.exceptions;
import forge.net.transport.exceptions;

namespace forge::net::quic {
namespace {

[[noreturn]] void raise_transport_failure(
   const forge::exceptions::base& error) {
   const auto code = exceptions::code_of(error);
   if (code == exceptions::code::connection_closed ||
       code == exceptions::code::stream_closed) {
      FORGE_THROW_EXCEPTION(
         forge::net::transport::exceptions::closed, error.what());
   }
   if (code == exceptions::code::canceled) {
      FORGE_THROW_EXCEPTION(
         forge::net::transport::exceptions::canceled, error.what());
   }
   if (code == exceptions::code::frame_too_large) {
      FORGE_THROW_EXCEPTION(
         forge::net::transport::exceptions::frame_too_large, error.what());
   }
   std::rethrow_exception(std::current_exception());
}

class quic_stream_concept final : public forge::net::transport::detail::stream_concept {
 public:
   explicit quic_stream_concept(stream value) : value_(std::move(value)) {}

   [[nodiscard]] bool valid() const noexcept override {
      return value_.valid();
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return value_.id();
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override {
      try {
         co_await value_.async_write(bytes);
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk bytes) override {
      try {
         co_await detail::stream_access::async_write_chunk(value_, std::move(bytes));
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      try {
         co_return co_await value_.async_read();
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk() override {
      try {
         co_return forge::net::transport::chunk{co_await value_.async_read()};
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   boost::asio::awaitable<void> async_close() override {
      try {
         co_await value_.async_close();
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   void cancel() override {
      value_.cancel();
   }

   void request_cancel() noexcept {
      value_.request_cancel();
   }

 private:
   stream value_;
};

class quic_session_concept final : public forge::net::transport::detail::session_concept {
 public:
   explicit quic_session_concept(connection value) : value_(std::move(value)) {}

   [[nodiscard]] bool valid() const noexcept override {
      return value_.valid();
   }

   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      try {
         co_return as_transport_stream(co_await value_.async_open_stream());
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      try {
         co_return as_transport_stream(co_await value_.async_accept_stream());
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   boost::asio::awaitable<void> async_close() override {
      try {
         co_await value_.async_close();
      } catch (const forge::exceptions::base& error) {
         raise_transport_failure(error);
      }
   }

   void cancel() override {
      value_.cancel();
   }

   void request_cancel() noexcept override {
      value_.request_cancel();
   }

 private:
   connection value_;
};

[[nodiscard]] bool same_limits(const forge::net::transport::limits& left, const forge::net::transport::limits& right) noexcept {
   return left.max_connections == right.max_connections &&
          left.max_streams_per_connection == right.max_streams_per_connection &&
          left.max_queued_bytes == right.max_queued_bytes &&
          left.max_inbound_queued_bytes == right.max_inbound_queued_bytes &&
          left.max_inbound_queued_packets == right.max_inbound_queued_packets &&
          left.max_frame_size == right.max_frame_size;
}

[[nodiscard]] bool has_custom_limits(const forge::net::transport::limits& value) noexcept {
   return !same_limits(value, forge::net::transport::limits{});
}

[[noreturn]] void throw_invalid_transport_endpoint(const forge::net::transport::endpoint& value, std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, std::move(message),
                       forge::exceptions::ctx("host", value.host),
                       forge::exceptions::ctx("port", value.port),
                       forge::exceptions::ctx("protocol", static_cast<int>(value.protocol)));
}

[[nodiscard]] endpoint validate_connect_endpoint(const forge::net::transport::endpoint& value) {
   if (value.protocol != forge::net::transport::endpoint::protocol_kind::quic_v1) {
      throw_invalid_transport_endpoint(value, "QUIC transport connector requires quic_v1 endpoint protocol");
   }
   if (value.host.empty()) {
      throw_invalid_transport_endpoint(value, "QUIC transport connector requires non-empty host");
   }
   if (value.port == 0) {
      throw_invalid_transport_endpoint(value, "QUIC transport connector requires non-zero remote port");
   }

   try {
      if (value.host_type == forge::net::transport::endpoint::host_kind::ip4 ||
          value.host_type == forge::net::transport::endpoint::host_kind::ip6) {
         static_cast<void>(value.literal_address());
      } else if (!value.zone.empty()) {
         throw_invalid_transport_endpoint(value, "QUIC DNS endpoint cannot carry an interface zone");
      }
   } catch (const boost::system::system_error&) {
      throw_invalid_transport_endpoint(value, "invalid QUIC literal endpoint or interface zone");
   }

   auto error = boost::system::error_code{};
   switch (value.host_type) {
   case forge::net::transport::endpoint::host_kind::ip4:
      (void)boost::asio::ip::make_address_v4(value.host, error);
      if (error) {
         throw_invalid_transport_endpoint(value, "QUIC transport connector requires valid IPv4 host");
      }
      return from_transport_endpoint(value);
   case forge::net::transport::endpoint::host_kind::ip6:
      (void)boost::asio::ip::make_address_v6(value.host, error);
      if (error) {
         throw_invalid_transport_endpoint(value, "QUIC transport connector requires valid IPv6 host");
      }
      return from_transport_endpoint(value);
   case forge::net::transport::endpoint::host_kind::dns:
   case forge::net::transport::endpoint::host_kind::dns4:
   case forge::net::transport::endpoint::host_kind::dns6:
      return from_transport_endpoint(value);
   }
   throw_invalid_transport_endpoint(value, "QUIC transport connector received unsupported host kind");
}

[[nodiscard]] endpoint validate_listen_endpoint(const forge::net::transport::endpoint& value) {
   if (value.protocol != forge::net::transport::endpoint::protocol_kind::quic_v1) {
      throw_invalid_transport_endpoint(value, "QUIC transport listener requires quic_v1 endpoint protocol");
   }
   if (value.host.empty()) {
      throw_invalid_transport_endpoint(value, "QUIC transport listener requires non-empty host");
   }
   try {
      static_cast<void>(value.literal_address());
   } catch (const boost::system::system_error&) {
      throw_invalid_transport_endpoint(value, "invalid QUIC listener literal endpoint or interface zone");
   }
   switch (value.host_type) {
   case forge::net::transport::endpoint::host_kind::ip4: {
      auto error = boost::system::error_code{};
      (void)boost::asio::ip::make_address_v4(value.host, error);
      if (error) {
         throw_invalid_transport_endpoint(value, "QUIC transport listener requires valid IPv4 host");
      }
      return from_transport_endpoint(value);
   }
   case forge::net::transport::endpoint::host_kind::ip6: {
      auto error = boost::system::error_code{};
      (void)boost::asio::ip::make_address_v6(value.host, error);
      if (error) {
         throw_invalid_transport_endpoint(value, "QUIC transport listener requires valid IPv6 host");
      }
      return from_transport_endpoint(value);
   }
   case forge::net::transport::endpoint::host_kind::dns:
   case forge::net::transport::endpoint::host_kind::dns4:
   case forge::net::transport::endpoint::host_kind::dns6:
      throw_invalid_transport_endpoint(value, "QUIC transport listener cannot bind DNS host kind");
   }
   throw_invalid_transport_endpoint(value, "QUIC transport listener received unsupported host kind");
}

[[nodiscard]] client_options apply_limits(client_options value, const forge::net::transport::connect_options& options) {
   if (has_custom_limits(options.limits)) {
      value.limits = from_transport_limits(options.limits);
   }
   return value;
}

[[nodiscard]] server_options apply_limits(server_options value, const forge::net::transport::listen_options& options) {
   if (has_custom_limits(options.limits)) {
      value.limits = from_transport_limits(options.limits);
   }
   return value;
}

class quic_session_connector_concept final : public forge::net::transport::detail::session_connector_concept {
 public:
   quic_session_connector_concept(forge::asio::runtime& runtime, client_options options)
       : connector_(runtime), options_(std::move(options)) {}

   [[nodiscard]] bool valid() const noexcept override {
      return active_.load(std::memory_order_acquire);
   }

   boost::asio::awaitable<forge::net::transport::session_connection>
   async_connect(forge::net::transport::endpoint remote, forge::net::transport::connect_options options) override {
      if (!valid()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "QUIC transport connector canceled");
      }
      const auto requested = validate_connect_endpoint(remote);
      auto connection = co_await connector_.async_connect(requested, apply_limits(options_, options));
      co_return forge::net::transport::session_connection{
          .local_endpoint = to_transport_endpoint(connection.local_endpoint()),
          .remote_endpoint = to_transport_endpoint(connection.remote_endpoint()),
          .session = as_transport_session(std::move(connection)),
      };
   }

   void cancel() override {
      active_.store(false, std::memory_order_release);
      connector_.cancel();
   }

 private:
   connector connector_;
   client_options options_;
   std::atomic_bool active_ = true;
};

class quic_session_listener_concept final : public forge::net::transport::detail::session_listener_concept {
 public:
   quic_session_listener_concept(forge::asio::runtime& runtime, forge::net::transport::endpoint local, server_options options,
                                 forge::net::transport::listen_options listen_options)
       : listener_(std::make_unique<listener>(runtime, validate_listen_endpoint(local),
                                              apply_limits(std::move(options), listen_options))) {}

   [[nodiscard]] bool valid() const noexcept override {
      return active_.load(std::memory_order_acquire) && listener_ != nullptr;
   }

   [[nodiscard]] forge::net::transport::endpoint local_endpoint() const override {
      if (!valid()) {
         FORGE_THROW_EXCEPTION(exceptions::connection_closed, "invalid QUIC transport listener");
      }
      return to_transport_endpoint(listener_->local_endpoint());
   }

   boost::asio::awaitable<forge::net::transport::session_connection> async_accept() override {
      if (!valid()) {
         FORGE_THROW_EXCEPTION(exceptions::connection_closed, "invalid QUIC transport listener");
      }
      auto connection = co_await listener_->async_accept();
      co_return forge::net::transport::session_connection{
          .local_endpoint = to_transport_endpoint(connection.local_endpoint()),
          .remote_endpoint = to_transport_endpoint(connection.remote_endpoint()),
          .session = as_transport_session(std::move(connection)),
      };
   }

   boost::asio::awaitable<void> async_close() override {
      active_.store(false, std::memory_order_release);
      if (listener_) {
         listener_->stop();
      }
      co_return;
   }

   void cancel() override {
      active_.store(false, std::memory_order_release);
      if (listener_) {
         listener_->stop();
      }
   }

 private:
   std::unique_ptr<listener> listener_;
   std::atomic_bool active_ = true;
};

} // namespace

forge::net::transport::limits to_transport_limits(const transport_limits& value) {
   return forge::net::transport::limits{
       .max_connections = value.max_connections,
       .max_streams_per_connection = value.max_streams_per_connection,
       .max_queued_bytes = value.max_queued_bytes,
       .max_inbound_queued_bytes = value.max_inbound_queued_bytes,
       .max_inbound_queued_packets = value.max_inbound_queued_packets,
       .max_frame_size = value.max_frame_size,
   };
}

transport_limits from_transport_limits(const forge::net::transport::limits& value) {
   return transport_limits{
       .max_connections = value.max_connections,
       .max_streams_per_connection = value.max_streams_per_connection,
       .max_queued_bytes = value.max_queued_bytes,
       .max_inbound_queued_bytes = value.max_inbound_queued_bytes,
       .max_inbound_queued_packets = value.max_inbound_queued_packets,
       .max_frame_size = value.max_frame_size,
   };
}

[[nodiscard]] forge::net::transport::endpoint::host_kind host_kind_for(const endpoint& value) {
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.host, error);
   if (error) {
      switch (value.family) {
      case endpoint::address_family::any:
         return forge::net::transport::endpoint::host_kind::dns;
      case endpoint::address_family::ipv4:
         return forge::net::transport::endpoint::host_kind::dns4;
      case endpoint::address_family::ipv6:
         return forge::net::transport::endpoint::host_kind::dns6;
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "unsupported QUIC endpoint address family");
   }
   if (address.is_v4()) {
      return forge::net::transport::endpoint::host_kind::ip4;
   }
   return forge::net::transport::endpoint::host_kind::ip6;
}

forge::net::transport::endpoint to_transport_endpoint(const endpoint& value) {
   return forge::net::transport::endpoint{
       .host_type = host_kind_for(value),
       .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
       .host = value.host,
       .port = value.port,
       .zone = value.zone,
   };
}

endpoint from_transport_endpoint(const forge::net::transport::endpoint& value) {
   if (value.protocol != forge::net::transport::endpoint::protocol_kind::quic_v1) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "transport endpoint is not QUIC");
   }
   auto family = endpoint::address_family::any;
   switch (value.host_type) {
   case forge::net::transport::endpoint::host_kind::ip4:
   case forge::net::transport::endpoint::host_kind::ip6:
   case forge::net::transport::endpoint::host_kind::dns:
      break;
   case forge::net::transport::endpoint::host_kind::dns4:
      family = endpoint::address_family::ipv4;
      break;
   case forge::net::transport::endpoint::host_kind::dns6:
      family = endpoint::address_family::ipv6;
      break;
   }
   return endpoint{.host = value.host, .port = value.port, .family = family, .zone = value.zone};
}

forge::net::transport::stream as_transport_stream(stream value) {
   auto model = std::make_shared<quic_stream_concept>(std::move(value));
   auto cancel_on_failure = std::unique_ptr<quic_stream_concept, void (*)(quic_stream_concept*)>{
       model.get(), [](quic_stream_concept* stream) { stream->request_cancel(); }};
   auto weak = std::weak_ptr<quic_stream_concept>{model};
   auto result = forge::net::transport::detail::stream_access::make_cancelable(
       std::move(model), [weak = std::move(weak)]() noexcept {
          if (auto stream = weak.lock()) {
             stream->request_cancel();
          }
       });
   static_cast<void>(cancel_on_failure.release());
   return result;
}

forge::net::transport::session as_transport_session(connection value) {
   auto model = std::make_shared<quic_session_concept>(std::move(value));
   auto cancel_on_failure = std::unique_ptr<quic_session_concept, void (*)(quic_session_concept*)>{
       model.get(), [](quic_session_concept* session) { session->request_cancel(); }};
   auto result = forge::net::transport::detail::session_access::make(model);
   static_cast<void>(cancel_on_failure.release());
   return result;
}

forge::net::transport::session_connector make_session_connector(forge::asio::runtime& runtime, client_options options) {
   return forge::net::transport::detail::session_connector_access::make(
       std::make_shared<quic_session_connector_concept>(runtime, std::move(options)));
}

forge::net::transport::session_listener make_session_listener(forge::asio::runtime& runtime, forge::net::transport::endpoint local,
                                                       server_options options,
                                                       forge::net::transport::listen_options listen_options) {
   return forge::net::transport::detail::session_listener_access::make(std::make_shared<quic_session_listener_concept>(
       runtime, std::move(local), std::move(options), listen_options));
}

void register_session(forge::net::transport::registry& registry, forge::asio::runtime& runtime, client_options client,
                      server_options server) {
   registry.register_session(
       forge::net::transport::endpoint::protocol_kind::quic_v1,
       [&runtime, client] {
          return make_session_connector(runtime, client);
       },
       [&runtime, server](forge::net::transport::endpoint local,
                          forge::net::transport::listen_options options) -> boost::asio::awaitable<forge::net::transport::session_listener> {
          co_return make_session_listener(runtime, std::move(local), server, options);
       });
}

} // namespace forge::net::quic
