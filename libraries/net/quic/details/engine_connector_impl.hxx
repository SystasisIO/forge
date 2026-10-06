#pragma once

#include "quic_engine_support.hxx"

namespace forge::net::quic::detail {
struct engine_connector::impl {
   struct active_connect {
      enum class state_value : std::uint8_t {
         pending,
         completed,
         timed_out,
         canceled,
      };

      std::atomic<state_value> state{state_value::pending};
      std::mutex mutex;
      std::shared_ptr<udp::resolver> resolver;
      forge::asio::notification resolution_changed;
      std::optional<udp::resolver::results_type> resolution_results;
      boost::system::error_code resolution_error;
      bool resolution_completed = false;
      std::weak_ptr<udp::socket> socket;
      std::weak_ptr<engine_connection::impl> connection;

      [[nodiscard]] bool mark_timed_out() noexcept;

      [[nodiscard]] bool mark_canceled() noexcept;

      [[nodiscard]] bool finish() noexcept;

      [[nodiscard]] bool timed_out() const noexcept;

      [[nodiscard]] bool canceled() const noexcept;

      void complete_resolution(boost::system::error_code error, udp::resolver::results_type results) noexcept;

      [[nodiscard]] bool take_resolution(boost::system::error_code& error, udp::resolver::results_type& results);

      void release_resolver() noexcept;

      static void cancel_resolver(const std::shared_ptr<udp::resolver>& value) noexcept;

      static void cancel_socket(const std::shared_ptr<udp::socket>& value) noexcept;

      void cancel_io() noexcept;

      void timeout_io() noexcept;
   };

   explicit impl(boost::asio::io_context& context_value);

   [[nodiscard]] bool valid() const noexcept;

   [[nodiscard]] std::shared_ptr<active_connect> track_connect(std::shared_ptr<udp::resolver> resolver);

   void cancel();

   boost::asio::io_context& context;
   std::shared_ptr<engine_listener::impl> source;
   std::optional<udp::endpoint> source_endpoint;
   std::mutex mutex;
   std::vector<std::weak_ptr<active_connect>> active;
   std::atomic_bool canceled = false;
};
} // namespace forge::net::quic::detail
