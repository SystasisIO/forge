module;

#include <forge/exceptions/macros.hpp>

extern "C" {
#include <oidc-agent/api.h>
}

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module forge.auth.oidc_agent.client;

import forge.asio.compute;
import forge.auth.oauth2.exceptions;
import forge.crypto.core.secret_string;

#include "details/client_impl.hxx"

namespace forge::auth::oidc_agent {
namespace {

void validate_text(const std::string& value) {
   if (value.empty() || value.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "oidc-agent text option is invalid");
   }
}

void validate_options(const client_options& options) {
   validate_text(options.selection.value);
   validate_text(options.application_hint);
}

[[nodiscard]] std::optional<std::string> encode_scopes(const forge::auth::oauth2::scope_set& scopes) {
   if (scopes.empty()) {
      return std::nullopt;
   }

   auto result = std::string{};
   for (const auto& scope : scopes) {
      if (scope.empty() || scope.find('\0') != std::string::npos ||
          scope.find_first_of(" \t\r\n") != std::string::npos) {
         FORGE_THROW_EXCEPTION(forge::auth::oauth2::exceptions::invalid_request, "oidc-agent scope request is invalid");
      }
      if (!result.empty()) {
         result.push_back(' ');
      }
      result += scope;
   }
   return result;
}

void validate_request(const forge::auth::oauth2::token_request& request) {
   const auto validity = request.minimum_validity.count();
   if (validity < 0 ||
       static_cast<std::uintmax_t>(validity) > static_cast<std::uintmax_t>(std::numeric_limits<std::time_t>::max()) ||
       (request.audience.has_value() &&
        (request.audience->empty() || request.audience->find('\0') != std::string::npos))) {
      FORGE_THROW_EXCEPTION(forge::auth::oauth2::exceptions::invalid_request, "oidc-agent token request is invalid");
   }
}

[[nodiscard]] std::chrono::system_clock::time_point response_expiry(std::time_t expires_at,
                                                                   std::chrono::seconds minimum_validity) {
   using clock = std::chrono::system_clock;
   const auto maximum_seconds = std::chrono::duration_cast<std::chrono::seconds>(clock::duration::max());
   if (expires_at <= 0 || std::cmp_greater(expires_at, maximum_seconds.count())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_response, "oidc-agent returned a missing or unrepresentable token expiry");
   }
   if (minimum_validity > maximum_seconds) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_response, "oidc-agent token lifetime does not meet minimum validity");
   }

   const auto expiry = clock::time_point{std::chrono::duration_cast<clock::duration>(
       std::chrono::seconds{static_cast<std::chrono::seconds::rep>(expires_at)})};
   const auto validity = std::chrono::duration_cast<clock::duration>(minimum_validity);
   const auto now = clock::now();
   // Both durations are bounded; subtracting validity from a positive expiry cannot overflow.
   if (expiry <= now || expiry - validity < now) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_response, "oidc-agent returned an expired or insufficiently valid token");
   }
   return expiry;
}

class response_guard {
 public:
   explicit response_guard(struct agent_response response) : response_{response} {}

   ~response_guard() {
      secFreeAgentResponse(response_);
   }

   response_guard(const response_guard&) = delete;
   response_guard& operator=(const response_guard&) = delete;

   [[nodiscard]] const struct agent_response& get() const noexcept {
      return response_;
   }

 private:
   struct agent_response response_;
};

class token_wait_state final : public std::enable_shared_from_this<token_wait_state> {
 public:
   explicit token_wait_state(boost::asio::any_io_executor executor)
       : executor_{std::move(executor)}, ready_{executor_} {
      ready_.expires_at((boost::asio::steady_timer::time_point::max)());
   }

   void complete(forge::auth::oauth2::access_token value) {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (completed_) {
            return;
         }
         result_.emplace(std::move(value));
         completed_ = true;
      }
      wake();
   }

   void fail(std::exception_ptr error) {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (completed_) {
            return;
         }
         error_ = std::move(error);
         completed_ = true;
      }
      wake();
   }

   void abandon() {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (completed_) {
            return;
         }
         abandoned_ = true;
         completed_ = true;
      }
      wake();
   }

   [[nodiscard]] bool abandoned() const {
      const auto lock = std::scoped_lock{mutex_};
      return abandoned_;
   }

   boost::asio::awaitable<void> wait() {
      auto ignored = boost::system::error_code{};
      co_await ready_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
   }

   forge::auth::oauth2::access_token take_result() {
      const auto lock = std::scoped_lock{mutex_};
      if (error_) {
         std::rethrow_exception(error_);
      }
      if (!result_) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_response, "oidc-agent completion did not contain an access token");
      }
      return std::move(*result_);
   }

 private:
   void wake() {
      boost::asio::post(executor_, [state = shared_from_this()] {
         state->ready_.expires_at((boost::asio::steady_timer::time_point::min)());
      });
   }

   boost::asio::any_io_executor executor_;
   boost::asio::steady_timer ready_;
   mutable std::mutex mutex_;
   std::optional<forge::auth::oauth2::access_token> result_;
   std::exception_ptr error_;
   bool completed_ = false;
   bool abandoned_ = false;
};

[[nodiscard]] forge::auth::oauth2::access_token request_token(const client_options& options,
                                                              forge::auth::oauth2::token_request request) {
   validate_request(request);
   auto scopes = encode_scopes(request.scopes);
   const auto* scope = scopes.has_value() ? scopes->c_str() : nullptr;
   const auto* audience = request.audience.has_value() ? request.audience->c_str() : nullptr;
   const auto minimum_validity = static_cast<std::time_t>(request.minimum_validity.count());

   auto response =
       response_guard{options.selection.kind == selection_kind::account
                          ? getAgentTokenResponse(options.selection.value.c_str(), minimum_validity, scope,
                                                  options.application_hint.c_str(), audience)
                          : getAgentTokenResponseForIssuer(options.selection.value.c_str(), minimum_validity, scope,
                                                           options.application_hint.c_str(), audience)};
   const auto& raw = response.get();
   if (raw.type == AGENT_RESPONSE_TYPE_ERROR) {
      FORGE_THROW_EXCEPTION(
          forge::auth::oauth2::exceptions::setup_required, "oidc-agent requires account setup or user action",
          forge::exceptions::ctx(options.selection.kind == selection_kind::account ? "account" : "issuer",
                                 options.selection.value),
          forge::exceptions::ctx("recommended-flow", "oidc-gen --flow=device"));
   }
   if (raw.type != AGENT_RESPONSE_TYPE_TOKEN || raw.token_response.token == nullptr ||
       raw.token_response.token[0] == '\0') {
      FORGE_THROW_EXCEPTION(exceptions::invalid_response, "oidc-agent returned an invalid token response");
   }

   auto metadata = forge::auth::oauth2::access_token_metadata{};
   metadata.expires_at = response_expiry(raw.token_response.expires_at, request.minimum_validity);
   if (raw.token_response.issuer != nullptr) {
      metadata.issuer = raw.token_response.issuer;
   }
   return forge::auth::oauth2::access_token{
       forge::crypto::core::secret_string{raw.token_response.token},
       std::move(metadata),
   };
}

boost::asio::awaitable<forge::auth::oauth2::access_token>
await_token(forge::asio::compute::operation<forge::auth::oauth2::access_token> operation) {
   const auto executor = co_await boost::asio::this_coro::executor;
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   auto state = std::make_shared<token_wait_state>(executor);

   boost::asio::co_spawn(
       executor,
       [state, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
          try {
             state->complete(co_await std::move(operation).wait());
          } catch (...) {
             state->fail(std::current_exception());
          }
       },
       boost::asio::detached);

   auto cancellation_slot = cancellation.slot();
   const auto clear_slot = [](boost::asio::cancellation_slot* slot) noexcept { slot->clear(); };
   auto slot_cleanup = std::unique_ptr<boost::asio::cancellation_slot, decltype(clear_slot)>{
       &cancellation_slot,
       clear_slot,
   };
   if (cancellation_slot.is_connected()) {
      cancellation_slot.assign([state](boost::asio::cancellation_type type) {
         if (type != boost::asio::cancellation_type::none) {
            state->abandon();
         }
      });
   }
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      state->abandon();
   }

   co_await state->wait();
   if (state->abandoned() || cancellation.cancelled() != boost::asio::cancellation_type::none) {
      state->abandon();
      FORGE_THROW_EXCEPTION(forge::auth::oauth2::exceptions::canceled, "oidc-agent token wait was canceled");
   }
   co_return state->take_result();
}

} // namespace

client::client(std::shared_ptr<impl> implementation) : impl_{std::move(implementation)} {}

client::~client() = default;

std::shared_ptr<client> client::create(client_options options, forge::asio::compute::executor blocking_executor) {
   validate_options(options);
   if (!blocking_executor.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "oidc-agent blocking executor is unavailable");
   }
   return std::shared_ptr<client>{new client{std::make_shared<impl>(std::move(options), std::move(blocking_executor))}};
}

boost::asio::awaitable<forge::auth::oauth2::access_token>
client::async_access_token(forge::auth::oauth2::token_request request) {
   auto state = impl_;
   auto operation = forge::asio::compute::operation<forge::auth::oauth2::access_token>{};
   try {
      operation = co_await state->blocking.submit({.name = "oidc-agent access token"},
                                                  [options = state->options, request = std::move(request)]() mutable {
                                                     return request_token(options, std::move(request));
                                                  });
   } catch (const forge::asio::exceptions::canceled&) {
      FORGE_THROW_EXCEPTION(forge::auth::oauth2::exceptions::canceled, "oidc-agent token submission was canceled");
   } catch (const forge::asio::exceptions::rejected&) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "oidc-agent token request capacity is unavailable");
   }
   co_return co_await await_token(std::move(operation));
}

} // namespace forge::auth::oidc_agent
