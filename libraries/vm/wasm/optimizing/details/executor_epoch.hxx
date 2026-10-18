#pragma once

namespace forge::vm::wasm::optimizing {

// Private part of the product executor. Lifecycle calls and activation
// construction/destruction run in the existing whole-NULL host region. The
// supplied scheduler/runtime must remain alive through shutdown(). This owner
// neither owns a pool nor creates a timer thread for an activation.
class executor_epoch {
 public:
   explicit executor_epoch(asio::task::scheduler& scheduler);
   ~executor_epoch();

   executor_epoch(const executor_epoch&) = delete;
   executor_epoch& operator=(const executor_epoch&) = delete;

   // Cold, single-shot lifecycle; calls are externally serialized. Readiness
   // means the ticker actually started, not just that submission was accepted.
   boost::asio::awaitable<void> start(std::chrono::steady_clock::time_point deadline, std::stop_token stop = {});
   boost::asio::awaitable<void> shutdown();

   [[nodiscard]] bool ready() const noexcept;
   [[nodiscard]] bool failed() const noexcept;
   [[nodiscard]] std::uint64_t value() const noexcept;

   class activation {
    public:
      activation(executor_epoch& owner, std::chrono::steady_clock::time_point deadline, std::stop_token caller_stop,
                 std::stop_token compute_stop);
      ~activation();

      activation(const activation&) = delete;
      activation& operator=(const activation&) = delete;

      // Epoch expiry itself is never interpreted as a real deadline expiry.
      [[nodiscard]] trap_reason poll() noexcept;
      [[nodiscard]] trap_reason reason() const noexcept;
      // NULL -> remove/drain callbacks -> final sticky/absolute-clock check
      // while words remain pinned. Call before publishing successful execution.
      [[nodiscard]] trap_reason finish() noexcept;
      [[nodiscard]] std::stop_token guest_stop() const noexcept;

    private:
      struct stop_handler {
         activation* owner;
         void operator()() const noexcept;
      };

      void publish(trap_reason reason) noexcept;
      void observe_deadline(std::chrono::steady_clock::time_point) noexcept;
      friend class executor_epoch;

      executor_epoch& _owner;
      std::chrono::steady_clock::time_point _deadline;
      alignas(8) mutable std::uint32_t _reason = static_cast<std::uint32_t>(trap_reason::none);
      // Initialize backing first, register even already-stopped tokens,
      // then remove/drain every callback before backing can disappear.
      std::optional<std::stop_callback<stop_handler>> _caller_stop;
      std::optional<std::stop_callback<stop_handler>> _compute_stop;
      std::optional<std::stop_callback<stop_handler>> _executor_stop;
      bool _finished = false; // Logical invocation thread only.
      std::stop_source _guest_stop;
      activation* _next = nullptr;
      bool _linked = false; // Owner completion gate.
   };

 private:
   friend struct executor_epoch_test_access;
   enum class phase : std::uint8_t { idle, starting, running, stopping, stopped, failed };

   boost::asio::awaitable<void> tick(asio::task::context& context);
   void bump() noexcept;
   void admit();
   void retire() noexcept;

   asio::task::scheduler& _scheduler;
   asio::task::handle _ticker;
   asio::notification _changed;
   std::stop_source _stop;
   alignas(8) mutable std::uint64_t _epoch = 0;
   std::atomic<phase> _phase{phase::idle};
   std::atomic<bool> _failed{false};
   std::atomic<std::size_t> _active{0};
   // Only admission/stopping and complete retirement/zero observation share
   // this per-owner gate. Never hold it across guest work, callbacks or await.
   std::mutex _completion_mutex;
   bool _drained = true; // Externally serialized cold lifecycle only.
   activation* _activations = nullptr;
};

} // namespace forge::vm::wasm::optimizing
