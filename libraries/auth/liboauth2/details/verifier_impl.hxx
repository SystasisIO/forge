#pragma once

#include "vendor_runtime.hxx"

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace forge::auth::liboauth2 {

struct verifier::impl : std::enable_shared_from_this<impl> {
   struct completion {
      void run(impl& owner, forge::crypto::core::secret_string bearer, forge::asio::compute::context& context,
               std::chrono::steady_clock::time_point deadline);
      boost::asio::awaitable<bool> async_wait(std::chrono::steady_clock::time_point deadline);

      std::mutex mutex;
      forge::asio::notification changed;
      bool finished = false;
      std::optional<oauth2::verified_token::claims> claims;
      std::exception_ptr error;
   };

   struct config_deleter {
      void operator()(oauth2_cfg_token_verify_t* value) const noexcept;
   };
   using config_ptr = std::unique_ptr<oauth2_cfg_token_verify_t, config_deleter>;

   struct text_deleter {
      void operator()(char* value) const noexcept;
   };

   struct secret_form {
      secret_form();
      ~secret_form();
      void append(const char* name, std::string_view content);
      std::string value;
   };

   struct ticket {
      explicit ticket(std::shared_ptr<impl> value);
      ~ticket();
      std::shared_ptr<impl> owner;
   };

   impl(options value, forge::asio::compute::executor executor);
   static void validate(const options& value);
   [[nodiscard]] std::shared_ptr<ticket> admit();
   void release() noexcept;
   void request_stop() noexcept;
   [[nodiscard]] bool stopped() const noexcept;
   [[nodiscard]] bool drained() const noexcept;
   boost::asio::awaitable<void> async_wait_stopped();
   [[nodiscard]] oauth2::verified_token::claims verify(forge::crypto::core::secret_string bearer,
                                                       forge::asio::compute::context& context,
                                                       std::chrono::steady_clock::time_point deadline);
   [[nodiscard]] oauth2::verified_token::claims normalize(json_t* payload, oauth2::time_point now) const;
   void validate_header(const forge::crypto::core::secret_string& bearer) const;

   detail::vendor_runtime runtime;
   const options configuration;
   forge::asio::compute::executor blocking;
   config_ptr prototype;
   std::chrono::milliseconds vendor_budget;
   mutable std::mutex mutex;
   std::size_t in_flight = 0;
   bool stopping = false;
   std::stop_source stop;
   forge::asio::notification changed;
};

} // namespace forge::auth::liboauth2
