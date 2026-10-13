module;

#include <forge/exceptions/macros.hpp>
extern "C" {
#include <oauth2/cfg.h>
}
#include <jansson.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

module forge.auth.liboauth2.verifier;

import forge.asio.notification;

#include "details/verifier_impl.hxx"

namespace forge::auth::liboauth2 {

verifier::verifier(std::shared_ptr<impl> state) : _impl{std::move(state)} {}

verifier::~verifier() {
   request_stop();
}

std::shared_ptr<verifier> verifier::create(options options, forge::asio::compute::executor blocking) {
   impl::validate(options);
   if (!blocking.valid()) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_request, "verification requires a blocking executor");
   }
   return std::shared_ptr<verifier>{new verifier{std::make_shared<impl>(std::move(options), std::move(blocking))}};
}

boost::asio::awaitable<oauth2::verified_token> verifier::async_verify(forge::crypto::core::secret_string bearer,
                                                                      oauth2::verification_policy policy) {
   return verify_owned(_impl, std::move(bearer), std::move(policy));
}

void verifier::request_stop() noexcept {
   _impl->request_stop();
}

boost::asio::awaitable<void> verifier::async_drain() {
   return drain_owned(_impl);
}

boost::asio::awaitable<void> verifier::drain_owned(std::shared_ptr<impl> state) {
   state->request_stop();
   while (true) {
      const auto epoch = state->changed.epoch();
      if (state->drained()) {
         co_return;
      }
      co_await state->changed.async_wait(epoch);
   }
}

boost::asio::awaitable<oauth2::verified_token> verifier::verify_owned(std::shared_ptr<impl> state,
                                                                      forge::crypto::core::secret_string bearer,
                                                                      oauth2::verification_policy policy) {
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
   }
   if (policy != state->configuration.policy) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::unsupported_request, "verification policy is not configured");
   }
   if (bearer.size() > state->configuration.max_token_bytes) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::input_too_large, "verification input exceeds limit");
   }
   if (bearer.empty() || bearer.view().find_first_of("\r\n\t") != std::string_view::npos ||
       bearer.view().find('\0') != std::string_view::npos) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential rejected");
   }
   const auto deadline = std::chrono::steady_clock::now() + state->configuration.timeout;
   const auto completed = std::make_shared<impl::completion>();
   auto ticket = state->admit();
   auto operation = std::optional<forge::asio::compute::operation<void>>{};
   try {
      operation = state->blocking.try_submit(
          {.name = "resource token verification", .parent_stop_token = state->stop.get_token()},
          [ticket = std::move(ticket), completed, bearer = std::move(bearer),
           deadline](forge::asio::compute::context& context) mutable {
             completed->run(*ticket->owner, std::move(bearer), context, deadline);
          });
   } catch (const forge::asio::exceptions::canceled&) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
   } catch (const forge::asio::exceptions::rejected&) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::busy, "verification executor is unavailable");
   }
   if (!operation) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::busy, "verification capacity is unavailable");
   }
   using namespace boost::asio::experimental::awaitable_operators;
   auto result = std::variant<bool, std::monostate>{};
   try {
      // Serialize peer cancellation with wait re-arming, not the physical blocking job.
      const auto executor = co_await boost::asio::this_coro::executor;
      result = co_await boost::asio::co_spawn(
          boost::asio::make_strand(executor),
          (completed->async_wait(deadline) || state->async_wait_stopped()), boost::asio::use_awaitable);
   } catch (const boost::system::system_error& error) {
      operation->cancel();
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
      }
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification wait failed");
   } catch (...) {
      operation->cancel();
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
      }
      if (state->stopped()) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::stopped, "verification is stopped");
      }
      FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification wait failed");
   }
   try {
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
      }
      if (state->stopped()) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::stopped, "verification is stopped");
      }
      if (result.index() != 0 || !std::get<0>(result) || std::chrono::steady_clock::now() >= deadline) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification deadline exceeded");
      }
      auto claims = std::optional<oauth2::verified_token::claims>{};
      auto error = std::exception_ptr{};
      {
         const auto lock = std::scoped_lock{completed->mutex};
         claims = std::move(completed->claims);
         error = completed->error;
      }
      if (error) {
         try {
            std::rethrow_exception(error);
         } catch (const forge::asio::exceptions::canceled&) {
            FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
         }
      }
      if (!claims) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification result is missing");
      }
      auto verified = make_verified_token(std::move(*claims));
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::canceled, "verification canceled");
      }
      if (state->stopped()) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::stopped, "verification is stopped");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::issuer_unavailable, "verification deadline exceeded");
      }
      if (!verified.valid_at(std::chrono::system_clock::now())) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "verification evidence expired");
      }
      co_return verified;
   } catch (...) {
      operation->cancel();
      throw;
   }
}

} // namespace forge::auth::liboauth2
