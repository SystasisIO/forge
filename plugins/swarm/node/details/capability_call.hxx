#pragma once

namespace forge::plugins::swarm::node {

// This bridge owns one native call, not a background worker. All state and
// cancellation delivery are confined to the caller's strand; stop callbacks
// only wake its notification. Even a cancellation-ignoring native call is joined.
template <typename T> struct capability_call {
   struct state {
      enum class interruption { none, stopped, expired };
      forge::asio::notification changed;
      boost::asio::cancellation_signal cancellation;
      std::optional<T> result;
      std::exception_ptr failure;
      bool native_finished = false;
      bool done = false;
      bool cancellation_sent = false;
      interruption terminal_interruption = interruption::none;
   };

   static boost::asio::awaitable<T> run(boost::asio::awaitable<T> work,
      std::stop_token stop, std::stop_token lifetime, std::chrono::steady_clock::time_point deadline,
      std::function<boost::asio::awaitable<void>()> cleanup = {}) {
      const auto executor = co_await boost::asio::this_coro::executor;
      co_return co_await boost::asio::co_spawn(boost::asio::make_strand(executor),
         control(std::move(work), stop, lifetime, deadline, std::move(cleanup)), boost::asio::use_awaitable);
   }

 private:
   static void record_terminal(const std::shared_ptr<state>& pending, std::stop_token stop,
      std::stop_token lifetime, std::chrono::steady_clock::time_point deadline,
      boost::asio::cancellation_state parent) noexcept {
      if (!pending->cancellation_sent) {
         if (stop.stop_requested() || lifetime.stop_requested() ||
             parent.cancelled() != boost::asio::cancellation_type::none)
            pending->terminal_interruption = state::interruption::stopped;
         else if (std::chrono::steady_clock::now() >= deadline)
            pending->terminal_interruption = state::interruption::expired;
      }
      pending->native_finished = true;
      pending->changed.notify();
   }

   static boost::asio::awaitable<void> invoke(std::shared_ptr<state> pending, boost::asio::awaitable<T> work,
      std::stop_token stop, std::stop_token lifetime, std::chrono::steady_clock::time_point deadline,
      boost::asio::cancellation_state parent) {
      try { pending->result.emplace(co_await std::move(work)); }
      catch (...) { pending->failure = std::current_exception(); }
      // Record native termination before co_spawn dispatches its join callback.
      // Later stop/deadline cannot rewrite an earlier result during that gap.
      record_terminal(pending, stop, lifetime, deadline, parent);
   }

   static boost::asio::awaitable<T> control(boost::asio::awaitable<T> work,
      std::stop_token stop, std::stop_token lifetime, std::chrono::steady_clock::time_point deadline,
      std::function<boost::asio::awaitable<void>()> cleanup) {
      static_assert(std::is_nothrow_move_constructible_v<T>);
      auto pending = std::make_shared<state>();
      const auto wake = [pending] { pending->changed.notify(); };
      auto stopped = std::stop_callback{stop, wake};
      auto owner_stopped = std::stop_callback{lifetime, wake};
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      auto parent = co_await boost::asio::this_coro::cancellation_state;
      if (stop.stop_requested() || lifetime.stop_requested() || parent.cancelled() != boost::asio::cancellation_type::none)
         FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm capability call stopped before launch");
      if (std::chrono::steady_clock::now() >= deadline)
         FORGE_THROW_EXCEPTION(exceptions::timeout, "Swarm capability call expired before launch");

      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(executor, invoke(pending, std::move(work), stop, lifetime, deadline, parent),
         boost::asio::bind_cancellation_slot(pending->cancellation.slot(),
         [pending, stop, lifetime, deadline, parent](std::exception_ptr error) noexcept {
            if (error && !pending->failure) pending->failure = std::move(error);
            if (!pending->native_finished) record_terminal(pending, stop, lifetime, deadline, parent);
            pending->done = true;
            pending->changed.notify();
         }));

      auto primary = std::exception_ptr{};
      bool interrupted = false;
      bool disconnected = false;
      while (true) {
         const auto epoch = pending->changed.epoch();
         if (pending->done) {
            if (!primary) {
               try {
                  if (pending->terminal_interruption == state::interruption::stopped)
                     FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm capability stopped before native completion");
                  if (pending->terminal_interruption == state::interruption::expired)
                     FORGE_THROW_EXCEPTION(exceptions::timeout, "Swarm capability expired before native completion");
               } catch (...) { primary = std::current_exception(); }
            }
            break;
         }
         if (!primary && !pending->native_finished) {
            try {
               if (stop.stop_requested() || lifetime.stop_requested() ||
                   parent.cancelled() != boost::asio::cancellation_type::none)
                  FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm capability call stopped");
               if (std::chrono::steady_clock::now() >= deadline)
                  FORGE_THROW_EXCEPTION(exceptions::timeout, "Swarm capability call expired");
            } catch (...) { primary = std::current_exception(); }
         }
         if (primary && !interrupted) {
            interrupted = true;
            pending->cancellation_sent = true;
            pending->cancellation.emit(boost::asio::cancellation_type::all);
            // This is our isolated controller, never the caller's coroutine.
            co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
            disconnected = true;
         }
         if (pending->native_finished && !disconnected) {
            // Only the completion callback remains. Preserve the recorded first
            // result and join it without consuming the caller's cancellation.
            co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
            disconnected = true;
         }
         try {
            if (interrupted || pending->native_finished) (void)co_await pending->changed.async_wait(epoch);
            else (void)co_await pending->changed.async_wait_until(epoch, deadline);
         } catch (...) {
            if (!primary && !pending->native_finished) primary = std::current_exception();
         }
      }
      if (!primary) primary = pending->failure;
      if (primary) {
         auto secondary = std::exception_ptr{};
         if (cleanup) {
            try {
               co_await boost::asio::co_spawn(executor, cleanup(), boost::asio::bind_cancellation_slot(
                  boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
            } catch (...) { secondary = std::current_exception(); }
         }
         if (secondary)
            FORGE_THROW_EXCEPTION_WITH_DATA(exceptions::operation_failed,
               (exceptions::failures{primary, secondary}), "Swarm capability call and cleanup failed");
         std::rethrow_exception(primary);
      }
      co_return std::move(*pending->result);
   }
};

} // namespace forge::plugins::swarm::node
