#pragma once

namespace forge::plugins::crypto::wallet {

struct provider::impl : std::enable_shared_from_this<impl> {
   explicit impl(std::string name);
   void reserve();
   boost::asio::awaitable<forge::asio::gate::ticket> enter_reserved();
   boost::asio::awaitable<forge::asio::gate::ticket> enter();
   void require_unlocked() const;
   void require_initialized() const;
   void expire_if_due();
   void cancel_expiry();
   forge::crypto::wallet::wallet_status snapshot() const;
   void refresh();
   void schedule_expiry();
   boost::asio::awaitable<void> expire(std::uint64_t generation);

   // Use the existing bounded compute executor directly: scheduler expiry tasks
   // may wait for this gate, so compute must not wait for their awaitable slots.
   // Admitted work drains before releasing the gate, even on cancellation.
   template <typename Work> boost::asio::awaitable<std::invoke_result_t<Work>> blocking(Work work) {
      using result_type = std::invoke_result_t<Work>;
      auto result = std::make_shared<std::optional<result_type>>();
      auto callable = std::make_shared<Work>(std::move(work));
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         throw forge::api::core::exceptions::cancelled{"Wallet operation was canceled"};
      }
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      std::exception_ptr error;
      try {
         result->emplace(co_await compute.execute({.name = "wallet.keystore"}, [callable] { return (*callable)(); }));
      } catch (const forge::crypto::keystore::exceptions::durability_unknown&) {
         state = forge::crypto::wallet::state::fault;
         accepting_signatures.store(false, std::memory_order_release);
         store.reset();
         cancel_expiry();
         error = std::current_exception();
      } catch (const forge::crypto::keystore::exceptions::ownership_lost&) {
         state = forge::crypto::wallet::state::fault;
         accepting_signatures.store(false, std::memory_order_release);
         store.reset();
         cancel_expiry();
         error = std::current_exception();
      } catch (...) {
         error = std::current_exception();
      }
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_terminal_cancellation{});
      if (error) {
         std::rethrow_exception(error);
      }
      co_return std::move(result->value());
   }

   std::string name;
   std::filesystem::path path;
   forge::asio::task::scheduler* scheduler = nullptr;
   forge::asio::compute::executor compute;
   forge::asio::gate gate;
   std::atomic_bool stopping = false;
   std::atomic_bool accepting_signatures = false;
   std::atomic_uint64_t lock_generation = 0;
   std::atomic_uint32_t pending = 0;
   std::uint32_t max_pending = 64;
   std::uint32_t timeout = 300;
   bool started = false;
   forge::asio::task::handle timer;
   bool timer_scheduled = false;
   std::uint64_t timer_generation = 0;
   std::chrono::steady_clock::time_point timer_due = std::chrono::steady_clock::time_point::max();
   std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
   forge::crypto::wallet::state state = forge::crypto::wallet::state::locked;
   std::shared_ptr<forge::crypto::keystore::ownership> owner;
   std::optional<forge::crypto::keystore::store> store;
};

} // namespace forge::plugins::crypto::wallet
