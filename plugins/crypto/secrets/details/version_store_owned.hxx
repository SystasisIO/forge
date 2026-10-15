#pragma once

namespace forge::plugins::crypto::secrets {

template <typename Result>
boost::asio::awaitable<Result> version_store::isolated(boost::asio::awaitable<Result> work, bool admit) {
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (admit && cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   using stored_type = std::conditional_t<std::is_void_v<Result>, bool, Result>;
   auto result = std::optional<stored_type>{};
   auto error = std::exception_ptr{};
   try {
      if constexpr (std::is_void_v<Result>) {
         co_await boost::asio::co_spawn(
             co_await boost::asio::this_coro::executor, std::move(work),
             boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
         result.emplace(true);
      } else {
         result.emplace(co_await boost::asio::co_spawn(
             co_await boost::asio::this_coro::executor, std::move(work),
             boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable)));
      }
   } catch (...) {
      error = std::current_exception();
   }
   co_await boost::asio::this_coro::throw_if_cancelled(throws);
   if (error) {
      try {
         std::rethrow_exception(error);
      } catch (const exceptions::purpose_denied&) {
         FORGE_THROW_EXCEPTION(exceptions::purpose_denied, "Managed secret purpose is not allowed");
      }
   }
   if constexpr (std::is_void_v<Result>) {
      co_return;
   } else {
      co_return std::move(*result);
   }
}

template <typename Work> boost::asio::awaitable<std::invoke_result_t<Work>> version_store::blocking(Work work) {
   return _compute.execute({.name = "secrets.version"}, std::move(work));
}

} // namespace forge::plugins::crypto::secrets
