#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>

#include <boost/system/error_code.hpp>

namespace forge::net::http::detail {

class server_log {
 public:
   enum class event { listening, drained, runtime_stopped, start_failed, accept_failed, drain_failed,
                      session_failed, handler_failed };
   enum class rejection { tls_limit, tls_context, header_limit, body_limit };

   struct error {
      int code = 0;
      const char* category = "none";
      bool expected = false;
      bool client = false;
      bool tls = false;
   };

   // One strand-owned observation per parsed/partially received request.
   struct request {
      void begin(bool enabled) noexcept;
      void finish(std::exception_ptr failure = {}) noexcept;

      bool enabled = false;
      bool active = false;
      bool sent = false;
      bool sent_bytes_known = true;
      bool aborted = false;
      unsigned method = 0;
      unsigned status = 0;
      std::uint64_t received_bytes = 0;
      std::uint64_t sent_bytes = 0;
      std::uint64_t response_chunks = 0;
      error failure;
      std::chrono::steady_clock::time_point started;
   };

   static error classify(std::exception_ptr failure) noexcept;
   static error classify(const boost::system::error_code& failure) noexcept;
   static unsigned upgrade_rejection_status(const boost::system::error_code& failure) noexcept;
   static void emit(event value, std::exception_ptr failure = {}, std::uint16_t port = 0,
                    bool tls = false) noexcept;

   void reject(rejection value) noexcept;
   void handshake_failed(const boost::system::error_code& failure) noexcept;
   void handshake_failed(std::exception_ptr failure) noexcept;
   void session_failed(std::exception_ptr failure) noexcept;
   void flush() noexcept;

 private:
   struct summary {
      std::uint64_t tls_failed = 0;
      std::uint64_t tls_closed = 0;
      std::uint64_t tls_session_failed = 0;
      std::uint64_t tls_rejected = 0;
      std::uint64_t tls_context_missing = 0;
      std::uint64_t header_rejected = 0;
      std::uint64_t body_rejected = 0;
      error last_tls_error;
      error last_session_error;
   };

   void record_tls_failure(error failure) noexcept;
   void flush(bool force) noexcept;
   std::mutex _mutex;
   summary _pending;
   std::chrono::steady_clock::time_point _next_summary;
};

} // namespace forge::net::http::detail
