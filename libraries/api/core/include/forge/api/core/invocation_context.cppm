module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

export module forge.api.core.invocation_context;

export import forge.api.core.trusted_invocation;

export namespace forge::api::core {

struct invocation_lifetime {
   invocation_lifetime() = default;
   std::optional<std::chrono::system_clock::time_point> expires_at;
   std::chrono::milliseconds recheck_interval{0};
   std::chrono::milliseconds recheck_timeout{0};
   std::function<boost::asio::awaitable<bool>(trusted_invocation)> recheck;

   [[nodiscard]] bool active() const noexcept;
   void bind_expiry();
   void limit_for(std::chrono::milliseconds duration);
   void check() const;
   boost::asio::awaitable<std::exception_ptr>
   async_wait(trusted_invocation trusted, std::function<void(std::exception_ptr)> on_denied = {}) const;

 private:
   std::optional<std::chrono::steady_clock::time_point> expires_by_;
   std::optional<std::chrono::system_clock::time_point> bound_expiry_;
};

struct invocation_context {
   trusted_invocation trusted;
   invocation_lifetime lifetime;
};

namespace detail {

template <typename T> struct invocation_completion {
   std::optional<std::conditional_t<std::is_void_v<T>, std::monostate, T>> value;
   std::exception_ptr error;
};

template <typename T>
boost::asio::awaitable<invocation_completion<T>> complete_invocation(boost::asio::awaitable<T> operation) {
   auto completion = invocation_completion<T>{};
   try {
      if constexpr (std::is_void_v<T>) {
         co_await std::move(operation);
         completion.value.emplace();
      } else {
         completion.value.emplace(co_await std::move(operation));
      }
   } catch (...) {
      completion.error = std::current_exception();
   }
   co_return completion;
}

} // namespace detail

// Both branches are joined before returning; there is no detached watchdog.
template <typename T>
boost::asio::awaitable<T> enforce_invocation(
   boost::asio::awaitable<T> operation, invocation_context context,
   std::function<void(std::exception_ptr)> on_denied = {}) {
   context.lifetime.bind_expiry();
   context.lifetime.check();
   if (!context.lifetime.active()) {
      co_return co_await std::move(operation);
   }
   using namespace boost::asio::experimental::awaitable_operators;
   auto result = co_await (detail::complete_invocation(std::move(operation)) ||
                          context.lifetime.async_wait(std::move(context.trusted), std::move(on_denied)));
   if (result.index() == 1) {
      const auto failure = std::get<1>(result);
      if (!failure) {
         throw exceptions::protocol_error{"invocation lifetime ended without a cause"};
      }
      std::rethrow_exception(failure);
   }
   auto completion = std::move(std::get<0>(result));
   if (completion.error) {
      std::rethrow_exception(completion.error);
   }
   // A result completing concurrently with expiry must not reopen admission.
   context.lifetime.check();
   if constexpr (std::is_void_v<T>) {
      co_return;
   } else {
      co_return std::move(*completion.value);
   }
}

} // namespace forge::api::core
