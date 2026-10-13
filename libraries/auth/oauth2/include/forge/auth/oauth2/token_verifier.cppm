module;

#include <boost/asio/awaitable.hpp>

export module forge.auth.oauth2.token_verifier;

export import forge.auth.oauth2.exceptions;
export import :verified_token;
export import forge.crypto.core.secret_string;

export namespace forge::auth::oauth2 {

class token_verifier {
 public:
   virtual ~token_verifier();

   // Implementations honor coroutine cancellation and discard late results.
   [[nodiscard]] virtual boost::asio::awaitable<verified_token> async_verify(forge::crypto::core::secret_string bearer,
                                                                             verification_policy policy) = 0;

   // Close admission first, then drain all admitted work before destroying dependencies.
   virtual void request_stop() noexcept = 0;
   [[nodiscard]] virtual boost::asio::awaitable<void> async_drain() = 0;

 protected:
   // Only a trusted verifier implementation may promote normalized, verified evidence.
   [[nodiscard]] static verified_token make_verified_token(verified_token::claims claims);
};

} // namespace forge::auth::oauth2
