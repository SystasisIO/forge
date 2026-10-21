#pragma once

namespace forge::swarm::runtime::detail {

// Owns inbound handler lifetime independently of API call completion. The API
// can publish cancellation before our disk work/handler has actually returned.
class call_scope : public std::enable_shared_from_this<call_scope> {
 public:
   class idle_pause;
   explicit call_scope(std::shared_ptr<operation_state>, std::string peer);
   ~call_scope();
   void bind(std::shared_ptr<forge::api::core::detail::stream_endpoint>,
             std::shared_ptr<forge::api::core::detail::stream_endpoint> = {});
   void permit(std::shared_ptr<quota>);
   void begin_transfer();
   void progress() noexcept;
   [[nodiscard]] idle_pause pause_idle();
   void rebind(forge::api::auth::authenticated_caller);
   void check() const;
   void interrupt() noexcept;
   void start();
   boost::asio::awaitable<void> finish();
   static boost::asio::awaitable<void> watch(std::shared_ptr<call_scope>);
   std::shared_ptr<operation_state> owner;
   std::string peer;
   std::shared_ptr<quota> grant;
   std::stop_source stop;
   std::chrono::steady_clock::time_point deadline;
 private:
   resource_budget::ticket _capacity;
   std::shared_ptr<forge::api::core::detail::stream_endpoint> _input;
   std::shared_ptr<forge::api::core::detail::stream_endpoint> _output;
   std::unique_ptr<std::stop_callback<std::function<void()>>> _owner_stop;
   std::unique_ptr<std::stop_callback<std::function<void()>>> _grant_stop;
   std::atomic_bool _finished{false};
   std::atomic_bool _watch_done{false};
   forge::asio::notification _changed;
   std::mutex _mutex;
   std::exception_ptr _watch_failure;
   std::chrono::steady_clock::time_point _last_useful;
   std::chrono::steady_clock::time_point _pause_started;
   std::size_t _paused = 0;
   bool _idle_active = false;
   void resume_idle() noexcept;
};

class call_scope::idle_pause {
 public:
   explicit idle_pause(std::shared_ptr<call_scope>);
   ~idle_pause();
   idle_pause(const idle_pause&) = delete;
   idle_pause& operator=(const idle_pause&) = delete;
   idle_pause(idle_pause&&) noexcept;
 private:
   std::shared_ptr<call_scope> _owner;
};

} // namespace forge::swarm::runtime::detail
