module;

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

export module forge.vm.wasm.interpret.watchdog;

export namespace forge::vm::wasm::interpret {

struct interrupt_options {
   std::stop_token cancel;
   std::stop_token external;
};

/// \brief Triggers a callback after a given time elapses.
///
class watchdog {
   class guard;
   enum class delivery { timer, requester };

 public:
   enum class interruption { deadline, canceled, none, interrupted };

   /// \tparam TimeUnits must be a chrono duration type
   /// \pre duration must be a non-negative value.
   template <typename TimeUnits> explicit watchdog(const TimeUnits& duration) : _duration(duration) {}
   template <typename TimeUnits>
   watchdog(const TimeUnits& duration, std::stop_token stop) : _duration(duration), _options{std::move(stop), {}} {}
   // Only this absolute API may notify stop synchronously on the requester.
   // Duration/stop callbacks and every deadline stay on the single timer
   // thread; teardown joins all delivery before releasing borrowed state.
   watchdog(std::chrono::steady_clock::time_point deadline, interrupt_options options);
   /// Starts the timer.  If the timer
   /// expires during the lifetime of the returned object, the callback
   /// will be executed. Duration constructors deliver asynchronously; an
   /// absolute constructor may deliver an explicit stop on its requester.
   /// If invoking the callback throws an exception, terminate will
   /// be called.
   template <typename F> [[nodiscard]] guard scoped_run(F&& callback) {
      auto notify = [function = std::forward<F>(callback)](interruption cause) mutable {
         if constexpr (std::is_invocable_v<F&, interruption>)
            function(cause);
         else
            function();
      };
      return guard(deadline(), _options, std::move(notify), _deadline ? delivery::requester : delivery::timer);
   }

 private:
   std::chrono::steady_clock::time_point deadline() const noexcept;

   class guard {
    public:
      guard(const guard&) = delete;
      guard& operator=(const guard&) = delete;

      guard(std::chrono::steady_clock::time_point deadline, interrupt_options options,
            std::function<void(interruption)> callback, delivery policy);
      ~guard();
      // The absolute-deadline API admits Work only after this gate. A request
      // after admission may interrupt Work; a preexisting request never runs it.
      bool enter();

    private:
      // worker thread
      // Implementation notes: This implementation creates a thread for
      // each watchdog.  If there are many watchdogs, it might be more
      // efficient to create a single thread for them all.
      void runner();
      bool update(bool admission, bool timer_delivery = false);
      void request(bool timer_delivery = false) noexcept;
      struct stop_callbacks;
      std::thread _timer;
      std::mutex _mutex;
      std::condition_variable _cond;
      std::function<void(interruption)> _callback;
      std::chrono::steady_clock::time_point _deadline;
      interrupt_options _options;
      delivery _policy;
      interruption _reason = interruption::none;
      bool _delivered = false;
      bool _closed = false;
      // Declared last: partial construction unregisters and joins callbacks
      // before destroying the state borrowed by their native notification.
      std::unique_ptr<stop_callbacks> _stops;
   };
   std::chrono::steady_clock::duration _duration{};
   std::optional<std::chrono::steady_clock::time_point> _deadline;
   interrupt_options _options;
};

class null_watchdog {
 public:
   template <typename F> null_watchdog scoped_run(F&&) {
      return *this;
   }
   ~null_watchdog() {} // avoid unused variable warnings
};

} // namespace forge::vm::wasm::interpret
