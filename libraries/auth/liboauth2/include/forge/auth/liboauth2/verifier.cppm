module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>

export module forge.auth.liboauth2.verifier;

export import forge.auth.oauth2.token_verifier;
export import forge.asio.compute;

export namespace forge::auth::liboauth2 {

class verifier final : public oauth2::token_verifier {
 public:
   struct jwks {
      std::string endpoint;
      std::size_t max_keys = 32;
      std::chrono::seconds cache_lifetime{60};
      std::chrono::seconds refresh_interval{30};
      std::chrono::milliseconds refresh_wait_timeout{1000};
   };

   struct introspection {
      std::string endpoint;
      std::string client_id;
      forge::crypto::core::secret_string client_secret;
   };

   struct options {
      oauth2::verification_policy policy;
      std::variant<jwks, introspection> source;
      std::filesystem::path ca_bundle;
      std::chrono::seconds http_timeout{5};
      std::chrono::milliseconds timeout{30'000};
      std::size_t max_in_flight = 64;
      std::size_t max_token_bytes = 16 * 1024;
      std::size_t max_response_bytes = 256 * 1024;
      std::size_t max_claim_bytes = 16 * 1024;
      std::size_t max_audiences = 32;
      std::size_t max_scopes = 128;
   };

   // Construct on the application initialization thread, before vendor work starts.
   [[nodiscard]] static std::shared_ptr<verifier> create(options options, forge::asio::compute::executor blocking);
   ~verifier() override;

   verifier(const verifier&) = delete;
   verifier& operator=(const verifier&) = delete;

   [[nodiscard]] boost::asio::awaitable<oauth2::verified_token>
   async_verify(forge::crypto::core::secret_string bearer, oauth2::verification_policy policy) override;
   void request_stop() noexcept override;
   [[nodiscard]] boost::asio::awaitable<void> async_drain() override;

 private:
   struct impl;
   explicit verifier(std::shared_ptr<impl> state);

   static boost::asio::awaitable<oauth2::verified_token> verify_owned(std::shared_ptr<impl> state,
                                                                      forge::crypto::core::secret_string bearer,
                                                                      oauth2::verification_policy policy);
   static boost::asio::awaitable<void> drain_owned(std::shared_ptr<impl> state);

   std::shared_ptr<impl> _impl;
};

} // namespace forge::auth::liboauth2
