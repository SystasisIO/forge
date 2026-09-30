#pragma once

extern "C++" {
namespace forge::plugins::net::p2p::resolver::detail {

class managed_remote_state final {
 public:
   struct generation {
      std::shared_ptr<forge::api::transport::connection> connection;
      std::shared_ptr<forge::api::core::remote_invoker> invoker;
      forge::api::core::api_ref selected;
      std::size_t peer_index = 0;
   };

   class timer_state final {
    public:
      timer_state(boost::asio::any_io_executor executor, std::chrono::milliseconds delay);

      [[nodiscard]] boost::asio::steady_timer& timer() noexcept;

    private:
      boost::asio::steady_timer timer_;
   };

   class reconnect_flight final {
    public:
      explicit reconnect_flight(boost::asio::any_io_executor executor);

      [[nodiscard]] const boost::asio::any_io_executor& executor() const noexcept;
      [[nodiscard]] forge::asio::notification& completed() noexcept;
      [[nodiscard]] forge::asio::notification& stop_changed() noexcept;
      [[nodiscard]] forge::asio::notification& cleanup_completed() noexcept;
      [[nodiscard]] boost::asio::cancellation_signal& cancellation() noexcept;

    private:
      boost::asio::any_io_executor executor_;
      forge::asio::notification completed_;
      forge::asio::notification stop_changed_;
      forge::asio::notification cleanup_completed_;
      boost::asio::cancellation_signal cancellation_;
      std::shared_ptr<generation> result_;
      std::shared_ptr<timer_state> stop_timer_;
      std::exception_ptr error_;
      std::size_t waiters_ = 0;
      bool done_ = false;
      bool stop_requested_ = false;
      bool watcher_done_ = false;
      bool child_done_ = false;

      friend class managed_remote_state;
   };

   using generation_ptr = std::shared_ptr<generation>;
   using flight_ptr = std::shared_ptr<reconnect_flight>;
   using timer_ptr = std::shared_ptr<timer_state>;

   enum class acquire_status {
      current,
      joined,
      draining,
      stopped,
      backpressure,
   };

   struct acquisition {
      acquire_status status = acquire_status::stopped;
      generation_ptr current;
      flight_ptr flight;
      forge::asio::notification::epoch_type observed = 0;
      bool start = false;
   };

   struct stop_effects {
      generation_ptr current;
      flight_ptr flight;
      bool initiated = false;
   };

   struct flight_observation {
      flight_ptr flight;
      forge::asio::notification::epoch_type observed = 0;
      bool done = true;
   };

   struct completion_snapshot {
      generation_ptr result;
      std::exception_ptr error;
      bool done = false;
      bool stopped = false;
   };

   struct completion_effects {
      generation_ptr canceled;
   };

   struct stop_observation {
      timer_ptr timer;
      forge::asio::notification::epoch_type observed = 0;
      bool requested = false;
      bool done = true;
   };

   struct cleanup_observation {
      forge::asio::notification::epoch_type observed = 0;
      bool done = true;
   };

   explicit managed_remote_state(std::size_t max_waiters) noexcept;

   [[nodiscard]] acquisition acquire_or_join(boost::asio::any_io_executor executor);
   [[nodiscard]] stop_effects request_stop() noexcept;
   [[nodiscard]] flight_observation observe_active_flight() const noexcept;
   [[nodiscard]] completion_snapshot read_completion(const flight_ptr& flight) const noexcept;
   [[nodiscard]] completion_effects complete(const flight_ptr& flight, generation_ptr result, std::exception_ptr error,
                                             std::exception_ptr stopped_error) noexcept;
   [[nodiscard]] stop_observation observe_stop(const flight_ptr& flight) const noexcept;
   void finish_watcher(const flight_ptr& flight) noexcept;
   void finish_child(const flight_ptr& flight, std::exception_ptr error) noexcept;
   [[nodiscard]] cleanup_observation observe_cleanup(const flight_ptr& flight) const noexcept;
   [[nodiscard]] generation_ptr invalidate(const generation_ptr& value, std::size_t peer_count) noexcept;
   void leave(const flight_ptr& flight) noexcept;

   [[nodiscard]] bool stopped() const noexcept;
   [[nodiscard]] std::size_t next_peer() const noexcept;
   [[nodiscard]] bool install_timer(const timer_ptr& timer) noexcept;
   void clear_timer(const timer_ptr& timer) noexcept;

 private:
   const std::size_t max_waiters_;
   mutable std::mutex mutex_;
   generation_ptr current_;
   flight_ptr flight_;
   timer_ptr timer_;
   std::size_t next_peer_ = 0;
   bool stopped_ = false;
};

using managed_remote_generation = managed_remote_state::generation;
using managed_remote_reconnect_flight = managed_remote_state::reconnect_flight;
using managed_remote_timer_state = managed_remote_state::timer_state;

} // namespace forge::plugins::net::p2p::resolver::detail
}
