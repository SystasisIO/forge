module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

export module forge.api.websocket.binding;

export import forge.api.core.binding;
export import forge.api.core.invocation_context;
export import forge.api.stream.options;
export import forge.api.websocket.connection;
export import forge.net.websocket.connection;
export import forge.net.http.router;
export import forge.net.http.route_context;

export namespace forge::api::websocket {

struct api_backpressure_options {
   std::size_t max_inflight = 128;
   std::uint64_t max_buffered_bytes = 16U * 1024U * 1024U;
};

using invocation_provider = std::function<boost::asio::awaitable<forge::api::core::invocation_context>(
   const forge::net::http::route_context&)>;

[[nodiscard]] forge::api::core::capability_set binding_capabilities() noexcept;

class api_binding {
 public:
   api_binding(forge::api::core::binding_plan plan,
               forge::api::stream::options options);

   boost::asio::awaitable<void>
   accept(forge::net::websocket::connection::ptr connection) const;
   boost::asio::awaitable<void>
   accept(forge::net::websocket::connection::ptr connection, forge::api::core::invocation_context context) const;
   [[nodiscard]] forge::net::http::websocket_upgrade_handler guarded(invocation_provider provider) const;
   boost::asio::awaitable<forge::api::websocket::connection>
   connect(forge::net::websocket::connection::ptr connection) const;

   [[nodiscard]] const forge::api::core::codec_id& codec() const noexcept;
   [[nodiscard]] std::size_t max_frame_size() const noexcept;
   [[nodiscard]] api_backpressure_options backpressure() const noexcept;
   [[nodiscard]] const forge::api::stream::options& options() const noexcept;

 private:
   forge::api::core::binding_plan plan_;
   forge::api::stream::options options_{
      .max_frame_size = 1024U * 1024U,
   };
};

class api_builder {
 public:
   api_builder& use(forge::api::core::binding_plan plan);
   api_builder& codec(forge::api::core::codec_id value);
   api_builder& max_frame_size(std::size_t value);
   api_builder& backpressure(api_backpressure_options value);
   api_builder& deadline(std::chrono::milliseconds value);
   api_builder& initial_window(std::uint32_t items, std::uint64_t bytes);

   [[nodiscard]] api_binding build();

 private:
   forge::api::core::binding_plan plan_;
   forge::api::stream::options options_;
};

[[nodiscard]] api_builder api();

} // namespace forge::api::websocket
